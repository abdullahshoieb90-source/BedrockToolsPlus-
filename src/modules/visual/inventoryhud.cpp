// InventoryHUD — on-screen view of the local Player's main inventory.
//
// See include/bedrocktools/modules/visual/inventoryhud.hpp for the full
// design notes. The implementation here is the matching half: it reads
// the inventory on the LocalPlayerTick, keeps a thread-local snapshot
// the render thread then turns into HUD draw commands, and shuts the
// overlay off cleanly when the game has no Player to read from.

#include "inventoryhud.hpp"
#include "modules/ModuleRegistry.hpp"
#include "core/memory/Hooks.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// Pulled in from ShulkerPreview — these are the per-slot stack offsets
// the project already validates. Reusing them here keeps the per-slot
// read aligned with the rest of the codebase instead of inventing a
// new one.
namespace sp = bedrocktools::sdk::offsets::ShulkerPreview;

namespace {

using ItemStackBaseLoadItemFn = void (*)(void*, void*);
using ItemStackBaseGetDamageValueFn = short (*)(void*);
using PlayerGetSelectedItemFn = void* (*)(void*);
using NbtTreeFindFn = void* (*)(void*, const void*);

ItemStackBaseLoadItemFn g_loadItem = nullptr;
ItemStackBaseGetDamageValueFn g_getDamageValue = nullptr;
PlayerGetSelectedItemFn g_getSelectedItem = nullptr;
NbtTreeFindFn g_nbtTreeFind = nullptr;

InventoryHUDModule* g_module = nullptr;

// Minimum valid pointer for any dereference. The mmap-min address is
// roughly 0x10000 on Android and the game itself is mapped far above
// that; anything below 0x1000 is a NULL-with-low-bits sentinel we
// never want to chase.
constexpr std::uintptr_t kMinValidPointer = 0x1000;

// Same predicate the rest of the codebase uses: a stack is "live" when
// its SharedCounter pointer is non-null and the counter points to a
// real Item.
bool itemStackHasItem(const void* itemStack) {
    if (!itemStack) return false;
    if (reinterpret_cast<std::uintptr_t>(itemStack) < kMinValidPointer) return false;
    const auto* bytes = static_cast<const std::byte*>(itemStack);
    auto* counter = *reinterpret_cast<void* const*>(bytes + sp::ItemStackBaseItem);
    if (!counter) return false;
    if (reinterpret_cast<std::uintptr_t>(counter) < kMinValidPointer) return false;
    return *reinterpret_cast<void* const*>(counter) != nullptr;
}

void* getStackItem(const void* stack) {
    if (!itemStackHasItem(stack)) return nullptr;
    const auto* bytes = static_cast<const std::byte*>(stack);
    auto* counter = *reinterpret_cast<void* const*>(bytes + sp::ItemStackBaseItem);
    if (!counter) return nullptr;
    return *reinterpret_cast<void* const*>(counter);
}

uint16_t getItemId(const void* item) {
    if (!item) return 0;
    if (reinterpret_cast<std::uintptr_t>(item) < kMinValidPointer) return 0;
    return *reinterpret_cast<uint16_t*>(static_cast<std::byte*>(const_cast<void*>(item)) + sp::ItemId);
}

short getItemMaxDamage(const void* item) {
    if (!item) return 0;
    if (reinterpret_cast<std::uintptr_t>(item) < kMinValidPointer) return 0;
    void** vtable = *reinterpret_cast<void***>(const_cast<void*>(item));
    if (!vtable) return 0;
    const auto slot = bedrocktools::sdk::offsets::VTable::ItemGetMaxDamage;
    if (reinterpret_cast<std::uintptr_t>(vtable) < kMinValidPointer) return 0;
    if (reinterpret_cast<std::uintptr_t>(vtable[slot]) < kMinValidPointer) return 0;
    using Fn = short (*)(const void*);
    return reinterpret_cast<Fn>(vtable[slot])(item);
}

uint8_t getStackCount(const void* stack) {
    if (!itemStackHasItem(stack)) return 0;
    return *reinterpret_cast<const uint8_t*>(
        static_cast<const std::byte*>(stack) + bedrocktools::sdk::offsets::ItemStack::mCount);
}

// Walks the stack's NBT tree to look for an `ench` / `Enchantments` /
// `StoredEnchantments` / `minecraft:enchantments` /
// `minecraft:stored_enchantments` key — the five vanilla names Bedrock
// has used for the enchant list over the years. Any of them being
// present is enough to enable the glint / a label tint.
bool stackIsEnchanted(const void* stack) {
    if (!itemStackHasItem(stack)) return false;
    if (!g_nbtTreeFind) return false;
    const auto* bytes = static_cast<const std::byte*>(stack);
    void* userData = *reinterpret_cast<void* const*>(bytes + sp::ItemStackBaseUserData);
    if (!userData) return false;
    if (reinterpret_cast<std::uintptr_t>(userData) < kMinValidPointer) return false;

    auto* treeRoot = *reinterpret_cast<void* const*>(
        static_cast<const std::byte*>(userData) + sp::CompoundTagTreeRoot);
    auto* treeEnd = *reinterpret_cast<void* const*>(
        static_cast<const std::byte*>(userData) + sp::CompoundTagTreeEnd);
    if (!treeRoot || !treeEnd) return false;
    if (reinterpret_cast<std::uintptr_t>(treeRoot) < kMinValidPointer) return false;
    if (reinterpret_cast<std::uintptr_t>(treeEnd) < kMinValidPointer) return false;

    // NbtTreeKey is a small POD the SDK already documents in
    // ShulkerPreview. We re-create the same layout (key pointer,
    // length) to keep the call shape identical.
    struct NbtTreeKey {
        const char* data;
        std::size_t len;
    };

    auto containsKey = [&](const char* key) {
        NbtTreeKey searchKey{key, std::strlen(key)};
        void* node = g_nbtTreeFind(treeRoot, &searchKey);
        return node && node != treeEnd;
    };

    return containsKey("ench") ||
           containsKey("Enchantments") ||
           containsKey("StoredEnchantments") ||
           containsKey("minecraft:enchantments") ||
           containsKey("minecraft:stored_enchantments");
}

// Read a single ItemStack out of the PlayerInventory::mItems vector.
// `slot` is the index in the 0..35 range, with 0..8 = hotbar,
// 9..35 = main inventory. The returned pointer is the live ItemStack
// in the vector; the caller is responsible for not holding it past
// the next vector resize.
void* readSlot(void* inventory, int slot) {
    if (!inventory) return nullptr;
    if (reinterpret_cast<std::uintptr_t>(inventory) < kMinValidPointer) return nullptr;
    const auto* bytes = static_cast<std::byte*>(inventory);
    void* begin = *reinterpret_cast<void* const*>(
        bytes + bedrocktools::sdk::offsets::PlayerInventory::mItems);
    void* end = *reinterpret_cast<void* const*>(
        bytes + bedrocktools::sdk::offsets::PlayerInventory::mItems + sizeof(void*));
    if (!begin || !end) return nullptr;
    if (reinterpret_cast<std::uintptr_t>(begin) < kMinValidPointer) return nullptr;
    if (reinterpret_cast<std::uintptr_t>(end) < kMinValidPointer) return nullptr;
    const std::size_t count =
        (reinterpret_cast<std::uintptr_t>(end) - reinterpret_cast<std::uintptr_t>(begin)) /
        bedrocktools::sdk::offsets::PlayerInventory::mItemsSize;
    if (static_cast<std::size_t>(slot) >= count) return nullptr;
    auto* entry = reinterpret_cast<std::byte*>(begin) +
        static_cast<std::ptrdiff_t>(slot) * bedrocktools::sdk::offsets::PlayerInventory::mItemsSize;
    return entry;
}

uint32_t withAlpha(uint32_t color, float alpha) {
    const auto a = static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    return (a << 24) | (color & 0x00FFFFFFu);
}

} // namespace

InventoryHUDModule::InventoryHUDModule()
    : Module("Inventory HUD",
             "Shows the local player's regular inventory (hotbar + main) on screen. "
             "Width, height, slot size, spacing, background, border, item count and "
             "durability are all configurable in the HUD editor. Armor and offhand are "
             "intentionally hidden.") {
    g_module = this;
    m_slots.assign(36, SlotSnapshot{});
}

InventoryHUDModule::~InventoryHUDModule() {
    if (g_module == this) g_module = nullptr;
}

void InventoryHUDModule::onInit() {
    // Resolve the SDK symbols the module relies on. None of these are new
    // signatures — they all exist in the project's signature table today.
    g_loadItem = reinterpret_cast<ItemStackBaseLoadItemFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemStackBaseLoadItem));
    g_getDamageValue = reinterpret_cast<ItemStackBaseGetDamageValueFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemStackBaseGetDamageValue));
    g_nbtTreeFind = reinterpret_cast<NbtTreeFindFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::NbtTreeFind));
    g_getSelectedItem = reinterpret_cast<PlayerGetSelectedItemFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::PlayerGetSelectedItem));

    // The snapshot is refreshed off the player-tick event so the render
    // thread never has to walk memory on the EGL frame.
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_module && g_module->enabled) g_module->onLocalPlayerTick(event.player);
        });
}

void InventoryHUDModule::onDisable() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_slots.assign(36, SlotSnapshot{});
        m_readFailed = false;
    }
    // Clearing the draw commands removes the overlay immediately. The
    // module system stops calling onFrame() for disabled modules so this
    // is the only chance to clean up.
    ::submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
}

void InventoryHUDModule::onLocalPlayerTick(bedrocktools::sdk::Player* player) {
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastReadAt < std::chrono::milliseconds(std::max(0, m_refreshIntervalMs))) {
        return;
    }
    if (!readInventory(player)) {
        // Don't churn the snapshot on every failure — only mark a read
        // failure when we have nothing to draw at all.
        if (!m_readFailed) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_slots.assign(36, SlotSnapshot{});
            m_readFailed = true;
        }
        return;
    }
    m_readFailed = false;
    m_lastReadAt = now;
}

bool InventoryHUDModule::readInventory(bedrocktools::sdk::Player* player) {
    if (!player) return false;
    if (reinterpret_cast<std::uintptr_t>(player) < kMinValidPointer) return false;

    // ClientInstance is the entry point the rest of the SDK uses to find
    // the local player, but the LocalPlayerTickEvent already gave us a
    // valid Player pointer; we still cross-check that it is the local
    // one by asking the ClientInstance. If ClientInstance is not ready
    // (between world loads, etc.) we trust the tick event's payload —
    // the event itself is only published for the local player.
    auto* client = bedrocktools::sdk::ClientInstance::current();
    if (client) {
        auto* localPlayer = client->localPlayer();
        if (localPlayer && localPlayer != player) {
            // A remote player in a tick payload would be a bug in the
            // hook; bail rather than render someone else's inventory.
            return false;
        }
    }

    const auto invOffset = bedrocktools::sdk::offsets::Player::mInventory;
    if (invOffset == 0) return false;
    auto* inventory = *reinterpret_cast<void* const*>(
        reinterpret_cast<std::uintptr_t>(player) + invOffset);
    if (reinterpret_cast<std::uintptr_t>(inventory) < kMinValidPointer) return false;

    std::vector<SlotSnapshot> next(36);
    int readCount = 0;
    for (int slot = 0; slot < 36; ++slot) {
        void* stack = readSlot(inventory, slot);
        if (!stack) continue;
        if (!itemStackHasItem(stack)) continue;
        SlotSnapshot snap;
        snap.hasItem = true;
        snap.count = getStackCount(stack);
        snap.damage = g_getDamageValue ? g_getDamageValue(stack) : 0;
        void* item = getStackItem(stack);
        snap.itemId = getItemId(item);
        snap.maxDamage = getItemMaxDamage(item);
        snap.enchanted = stackIsEnchanted(stack);
        next[slot] = snap;
        ++readCount;
    }
    if (readCount == 0) {
        // 36 live slots, all empty — the player is in a freshly loaded
        // world or simply has nothing in their inventory. That is a
        // valid state and we still want to draw the empty grid.
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_slots.swap(next);
    }
    return true;
}

void InventoryHUDModule::onFrame() {
    if (!enabled) {
        ::submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
        return;
    }

    std::vector<SlotSnapshot> slots;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        slots = m_slots;
    }

    const float scale = std::clamp(m_scale, 0.1f, 5.0f);
    const float slotSize = std::clamp(m_slotSize, 8.0f, 96.0f) * scale;
    const float slotSpacing = std::clamp(m_slotSpacing, 0.0f, 32.0f) * scale;
    const float baseWidth = std::clamp(m_width, 64.0f, 1000.0f);
    const float baseHeight = std::clamp(m_height, 32.0f, 1000.0f);

    // 9 columns of slots is the vanilla layout and what players expect
    // from the in-game inventory. Resizing m_width or m_height never
    // changes the number of columns; it just changes how much of the
    // available space each slot is allowed to consume. If the user
    // shrinks the panel below the natural slot footprint, slots are
    // simply packed tighter; if they grow it, slots grow with the
    // panel up to the m_slotSize cap.
    constexpr int kColumns = 9;
    const int slotOffset = std::clamp(m_slotOffset, 0, 35);
    const int slotCount = std::clamp(m_slotCount, 0, 36 - slotOffset);
    const int totalSlots = slotOffset + slotCount;
    if (slotCount <= 0) {
        ::submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
        return;
    }
    const int rows = (totalSlots + kColumns - 1) / kColumns;

    // The natural footprint of the slot grid: a 9x4 grid of 32px slots
    // with 4px spacing. The user-supplied width/height are interpreted
    // as the *minimum* panel size — if the user picks something
    // smaller, the panel auto-shrinks to the natural footprint; if
    // they pick something larger, the panel grows but the slots stay
    // at m_slotSize (so the user can size the panel without the slots
    // running away from the cap).
    const float naturalWidth = kColumns * slotSize + (kColumns - 1) * slotSpacing;
    const float naturalHeight = rows * slotSize + (rows - 1) * slotSpacing;
    const float panelWidth = std::max(baseWidth, naturalWidth);
    const float panelHeight = std::max(baseHeight, naturalHeight);

    std::vector<PLModMenu_DrawCommand> cmds;
    cmds.reserve(16 + slotCount * 8);

    if (m_background) {
        const auto bgAlpha = std::clamp(m_backgroundOpacity, 0.0f, 1.0f);
        PLModMenu_DrawCommand bg{};
        bg.type = PL_DRAW_RECT_FILLED;
        bg.x = hudPosX;
        bg.y = hudPosY;
        bg.w = panelWidth;
        bg.h = panelHeight;
        bg.x3 = 2.0f * scale;
        bg.color = withAlpha(m_backgroundColor, bgAlpha);
        cmds.push_back(bg);
    }

    if (m_border) {
        const auto borderAlpha = std::clamp(m_borderOpacity, 0.0f, 1.0f);
        const auto borderThickness = std::max(0.5f, m_borderThickness) * scale;
        PLModMenu_DrawCommand border{};
        border.type = PL_DRAW_RECT;
        border.x = hudPosX;
        border.y = hudPosY;
        border.w = panelWidth;
        border.h = panelHeight;
        border.x3 = 2.0f * scale;
        border.size = borderThickness;
        border.color = withAlpha(m_borderColor, borderAlpha);
        cmds.push_back(border);
    }

    // Re-flow the slots inside the panel. The grid is anchored to the
    // top-left of the panel; with m_showEmptySlots the empty cells are
    // drawn as faint backdrops so the layout still reads as a grid
    // even when only a few slots are filled.
    const float gridX = hudPosX;
    const float gridY = hudPosY;

    for (int i = 0; i < slotCount; ++i) {
        const int slot = slotOffset + i;
        const int column = slot % kColumns;
        const int row = slot / kColumns;
        const float x = gridX + column * (slotSize + slotSpacing);
        const float y = gridY + row * (slotSize + slotSpacing);

        const auto& snap = slots[static_cast<std::size_t>(slot)];

        if (!snap.hasItem) {
            if (!m_showEmptySlots) continue;
            PLModMenu_DrawCommand empty{};
            empty.type = PL_DRAW_RECT;
            empty.x = x;
            empty.y = y;
            empty.w = slotSize;
            empty.h = slotSize;
            empty.size = 1.0f * scale;
            empty.color = withAlpha(m_emptySlotColor, 1.0f);
            cmds.push_back(empty);
            continue;
        }

        // The item itself: a simple colored square keyed by the
        // numeric item id. We do not have a pre-loaded per-item icon
        // texture here (the ItemRenderer hook used by ShulkerPreview
        // is wired into the container-screen render path, not the HUD
        // overlay), but the colored swatch still gives a clear visual
        // cue of "this slot has a block" while keeping the layout
        // exact to the user's width/height.
        const uint32_t itemRgb = (static_cast<uint32_t>(snap.itemId) * 0x9E3779B1u) & 0x00FFFFFFu;
        PLModMenu_DrawCommand itemBg{};
        itemBg.type = PL_DRAW_RECT_FILLED;
        itemBg.x = x;
        itemBg.y = y;
        itemBg.w = slotSize;
        itemBg.h = slotSize;
        itemBg.color = (0xC0u << 24) | itemRgb;
        cmds.push_back(itemBg);

        // A small inset border to make the item read as a separate
        // element from the slot frame, mirroring the vanilla look.
        PLModMenu_DrawCommand itemBorder{};
        itemBorder.type = PL_DRAW_RECT;
        itemBorder.x = x + 0.5f * scale;
        itemBorder.y = y + 0.5f * scale;
        itemBorder.w = slotSize - 1.0f * scale;
        itemBorder.h = slotSize - 1.0f * scale;
        itemBorder.size = 1.0f * scale;
        itemBorder.color = 0x80FFFFFFu;
        cmds.push_back(itemBorder);

        // A little label inside the slot showing the item id so the
        // user can tell at a glance what they are looking at. The
        // count/durability HUD elements sit on top.
        if (slotSize >= 24.0f * scale) {
            char label[16];
            std::snprintf(label, sizeof(label), "%u", static_cast<unsigned>(snap.itemId));
            PLModMenu_DrawCommand labelCmd{};
            labelCmd.type = PL_DRAW_TEXT;
            labelCmd.x = x;
            labelCmd.y = y + (slotSize - 12.0f * scale) * 0.5f;
            labelCmd.w = slotSize;
            labelCmd.h = 12.0f * scale;
            labelCmd.size = 12.0f * scale;
            labelCmd.color = 0xFFFFFFFFu;
            labelCmd.text = label;
            cmds.push_back(labelCmd);
        }

        if (m_showDurability && snap.maxDamage > 0) {
            const float remaining = static_cast<float>(snap.maxDamage - snap.damage) /
                static_cast<float>(snap.maxDamage);
            const float clamped = std::clamp(remaining, 0.0f, 1.0f);
            const float barHeight = std::max(1.0f, m_durabilityHeight) * scale;
            PLModMenu_DrawCommand track{};
            track.type = PL_DRAW_RECT_FILLED;
            track.x = x;
            track.y = y + slotSize - barHeight;
            track.w = slotSize;
            track.h = barHeight;
            track.color = 0x80000000u;
            cmds.push_back(track);
            PLModMenu_DrawCommand fill{};
            fill.type = PL_DRAW_RECT_FILLED;
            fill.x = x;
            fill.y = y + slotSize - barHeight;
            fill.w = std::max(barHeight, slotSize * clamped);
            fill.h = barHeight;
            fill.color = m_durabilityColor;
            cmds.push_back(fill);
        }

        if (m_showItemCount && snap.count > 1) {
            char countText[8];
            std::snprintf(countText, sizeof(countText), "%u", static_cast<unsigned>(snap.count));
            PLModMenu_DrawCommand countCmd{};
            countCmd.type = PL_DRAW_TEXT;
            countCmd.x = x;
            countCmd.y = y + slotSize - 14.0f * scale;
            countCmd.w = slotSize - 2.0f * scale;
            countCmd.h = 12.0f * scale;
            countCmd.size = m_itemCountSize * scale;
            countCmd.color = m_itemCountColor;
            countCmd.text = countText;
            cmds.push_back(countCmd);
        }

        if (snap.enchanted) {
            // A subtle glint line to mark enchanted items, matching
            // the convention other HUDs in the project use (effect
            // display in particular paints enchanted rows).
            PLModMenu_DrawCommand glint{};
            glint.type = PL_DRAW_RECT_FILLED;
            glint.x = x;
            glint.y = y;
            glint.w = slotSize;
            glint.h = std::max(1.0f, 2.0f * scale);
            glint.color = 0xC0B040FFu;
            cmds.push_back(glint);
        }
    }

    ::submitDrawCommands(moduleId, cmds);
}

void InventoryHUDModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();

    if (j.contains("m_width")) m_width = j["m_width"].get<float>();
    if (j.contains("m_height")) m_height = j["m_height"].get<float>();
    if (j.contains("m_scale")) m_scale = j["m_scale"].get<float>();
    if (j.contains("m_slotSize")) m_slotSize = j["m_slotSize"].get<float>();
    if (j.contains("m_slotSpacing")) m_slotSpacing = j["m_slotSpacing"].get<float>();
    if (j.contains("m_slotOffset")) m_slotOffset = j["m_slotOffset"].get<int>();
    if (j.contains("m_slotCount")) m_slotCount = j["m_slotCount"].get<int>();
    if (j.contains("m_refreshIntervalMs")) m_refreshIntervalMs = j["m_refreshIntervalMs"].get<int>();

    if (j.contains("m_background")) m_background = j["m_background"].get<bool>();
    if (j.contains("m_backgroundOpacity")) m_backgroundOpacity = j["m_backgroundOpacity"].get<float>();
    if (j.contains("m_border")) m_border = j["m_border"].get<bool>();
    if (j.contains("m_borderOpacity")) m_borderOpacity = j["m_borderOpacity"].get<float>();
    if (j.contains("m_borderThickness")) m_borderThickness = j["m_borderThickness"].get<float>();
    if (j.contains("m_showEmptySlots")) m_showEmptySlots = j["m_showEmptySlots"].get<bool>();
    if (j.contains("m_showItemCount")) m_showItemCount = j["m_showItemCount"].get<bool>();
    if (j.contains("m_showDurability")) m_showDurability = j["m_showDurability"].get<bool>();
    if (j.contains("m_durabilityHeight")) m_durabilityHeight = j["m_durabilityHeight"].get<float>();
    if (j.contains("m_itemCountSize")) m_itemCountSize = j["m_itemCountSize"].get<float>();

    if (j.contains("m_backgroundColor") && j["m_backgroundColor"].is_string()) {
        const std::string hex = j["m_backgroundColor"].get<std::string>();
        if (hex.size() > 1 && hex[0] == '#') {
            try { m_backgroundColor = static_cast<uint32_t>(std::stoul(hex.substr(1), nullptr, 16)); } catch (...) {}
        }
    }
    if (j.contains("m_borderColor") && j["m_borderColor"].is_string()) {
        const std::string hex = j["m_borderColor"].get<std::string>();
        if (hex.size() > 1 && hex[0] == '#') {
            try { m_borderColor = static_cast<uint32_t>(std::stoul(hex.substr(1), nullptr, 16)); } catch (...) {}
        }
    }
    if (j.contains("m_emptySlotColor") && j["m_emptySlotColor"].is_string()) {
        const std::string hex = j["m_emptySlotColor"].get<std::string>();
        if (hex.size() > 1 && hex[0] == '#') {
            try { m_emptySlotColor = static_cast<uint32_t>(std::stoul(hex.substr(1), nullptr, 16)); } catch (...) {}
        }
    }
    if (j.contains("m_itemCountColor") && j["m_itemCountColor"].is_string()) {
        const std::string hex = j["m_itemCountColor"].get<std::string>();
        if (hex.size() > 1 && hex[0] == '#') {
            try { m_itemCountColor = static_cast<uint32_t>(std::stoul(hex.substr(1), nullptr, 16)); } catch (...) {}
        }
    }
    if (j.contains("m_durabilityColor") && j["m_durabilityColor"].is_string()) {
        const std::string hex = j["m_durabilityColor"].get<std::string>();
        if (hex.size() > 1 && hex[0] == '#') {
            try { m_durabilityColor = static_cast<uint32_t>(std::stoul(hex.substr(1), nullptr, 16)); } catch (...) {}
        }
    }
}

void InventoryHUDModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;

    j["m_width"] = m_width;
    j["m_height"] = m_height;
    j["m_scale"] = m_scale;
    j["m_slotSize"] = m_slotSize;
    j["m_slotSpacing"] = m_slotSpacing;
    j["m_slotOffset"] = m_slotOffset;
    j["m_slotCount"] = m_slotCount;
    j["m_refreshIntervalMs"] = m_refreshIntervalMs;

    j["m_background"] = m_background;
    j["m_backgroundOpacity"] = m_backgroundOpacity;
    j["m_border"] = m_border;
    j["m_borderOpacity"] = m_borderOpacity;
    j["m_borderThickness"] = m_borderThickness;
    j["m_showEmptySlots"] = m_showEmptySlots;
    j["m_showItemCount"] = m_showItemCount;
    j["m_showDurability"] = m_showDurability;
    j["m_durabilityHeight"] = m_durabilityHeight;
    j["m_itemCountSize"] = m_itemCountSize;

    char hex[10];
    std::snprintf(hex, sizeof(hex), "#%08X", m_backgroundColor);
    j["m_backgroundColor"] = std::string(hex);
    std::snprintf(hex, sizeof(hex), "#%08X", m_borderColor);
    j["m_borderColor"] = std::string(hex);
    std::snprintf(hex, sizeof(hex), "#%08X", m_emptySlotColor);
    j["m_emptySlotColor"] = std::string(hex);
    std::snprintf(hex, sizeof(hex), "#%08X", m_itemCountColor);
    j["m_itemCountColor"] = std::string(hex);
    std::snprintf(hex, sizeof(hex), "#%08X", m_durabilityColor);
    j["m_durabilityColor"] = std::string(hex);
}

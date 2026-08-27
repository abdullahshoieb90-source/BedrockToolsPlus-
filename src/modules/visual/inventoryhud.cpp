// InventoryHUD — on-screen view of the local Player's main inventory.
//
// See inventoryhud.hpp for the design notes. The original implementation
// reached the inventory through a chain of guessed offsets
// (Player::mInventory -> PlayerInventory::mItems vector -> ItemStack::mCount)
// that were not validated against a real libminecraftpe.so dump. On 1.26.44
// ARM64 the chain resolved to null pointers and the HUD rendered an empty
// grid even when the player carried items. The implementation below drops
// those constants entirely and replaces them with a runtime scan of the
// Player memory region: every pointer-sized word inside a conservative
// window of the Player object is treated as a candidate base address and
// tested, together with a small set of plausible ItemStack strides, for a
// contiguous run of kInventorySize (36) well-formed ItemStack buffers using
// the same `itemStackHasItem` predicate the rest of the codebase relies on.
// The winning (base, stride, countOffset) triple is cached for the rest of
// the session; the scan re-runs once per m_rescanIntervalMs only while the
// cache is empty, so a freshly joined world is picked up without a
// relaunch.

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
#include <array>
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

using ItemStackBaseGetDamageValueFn = short (*)(void*);
using PlayerGetSelectedItemFn = void* (*)(void*);
using NbtTreeFindFn = void* (*)(void*, const void*);

ItemStackBaseGetDamageValueFn g_getDamageValue = nullptr;
PlayerGetSelectedItemFn g_getSelectedItem = nullptr;
NbtTreeFindFn g_nbtTreeFind = nullptr;

InventoryHUDModule* g_module = nullptr;

// Minimum valid pointer for any dereference. The mmap-min address is
// roughly 0x10000 on Android and the game itself is mapped far above
// that; anything below 0x1000 is a NULL-with-low-bits sentinel we
// never want to chase.
constexpr std::uintptr_t kMinValidPointer = 0x1000;

// Vanilla inventory layout: 9 hotbar slots followed by 27 main
// inventory slots. These are not offsets - they are game invariants
// (survival player inventory size has been 36 since beta), so they
// stay as constants here instead of in the SDK offset headers.
constexpr int kHotbarSize = 9;
constexpr int kInventorySize = 36;

// Conservative window inside the Player object to scan for an
// embedded or pointed-to inventory buffer. Player is roughly 3 KB on
// 1.26 ARM64 but we scan up to 8 KB to leave headroom for future
// growth; walking pointer-aligned words at that range is cheap.
constexpr std::size_t kPlayerScanBytes = 0x2000;

// Candidate ItemStack strides. On Bedrock ARM64 the observed stride
// between adjacent slots in the PlayerInventory flat array has been
// 0x800 for many versions (it matches ShulkerPreview::ItemStackStorageSize)
// but we probe a small set of nearby aligned sizes so a size change in
// a future update doesn't immediately break the scan. Strides are
// probed in the order listed; the first matching stride wins.
constexpr std::array<std::size_t, 4> kCandidateStrides = {{
    0x800, // historically documented ItemStack size
    0x400, // observed on older 32-bit / trimmed builds
    0xA00, // seen when extra NBT/user-data padding is added
    0xC00, // defensive: double-aligned / instrumented builds
}};

// Candidate offsets for ItemStack::mCount within an ItemStack. The
// count is a uint8_t that is only valid in 1..127 when the stack is
// non-empty, so we probe each offset and keep the first sensible
// value. The list intentionally contains the previously hardcoded
// value (0x88) as well as the neighboring positions observed across
// the 1.20..1.26 releases.
constexpr std::array<std::size_t, 6> kCandidateCountOffsets = {{
    0x80, 0x84, 0x88, 0x8C, 0x90, 0xA0,
}};

// Safe reader wrappers. Every raw dereference of a game pointer goes
// through one of these so the minimum-address guard lives in exactly
// one place. They intentionally return zero / nullptr on garbage so
// callers can chain checks without re-asserting.
inline bool isValidPointer(const void* p) {
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    return v >= kMinValidPointer;
}

template <class T>
inline T safeRead(const void* addr, T fallback = T{}) {
    if (!isValidPointer(addr)) return fallback;
    // On Android the game is always mapped readable; we additionally
    // require word-aligned accesses for the pointer walks used by the
    // scan to avoid unaligned faults on strict ARMv8 CPUs.
    if ((reinterpret_cast<std::uintptr_t>(addr) & (sizeof(T) - 1)) != 0) return fallback;
    T value;
    std::memcpy(&value, addr, sizeof(T));
    return value;
}

inline void* readPtr(const void* addr) {
    const auto v = safeRead<std::uintptr_t>(addr, 0);
    return reinterpret_cast<void*>(v);
}

// Same predicate the rest of the codebase uses: a stack is "live" when
// its SharedCounter pointer is non-null and the counter points to a
// real Item.
bool itemStackHasItem(const void* itemStack) {
    if (!itemStack) return false;
    if (!isValidPointer(itemStack)) return false;
    const auto* bytes = static_cast<const std::byte*>(itemStack);
    auto* counter = readPtr(bytes + sp::ItemStackBaseItem);
    if (!counter) return false;
    if (!isValidPointer(counter)) return false;
    auto* item = readPtr(counter);
    return item != nullptr && isValidPointer(item);
}

// An ItemStack-shaped slot is "well formed" if either (a) it's empty
// (counter pointer is null / below min address) or (b) it passes
// itemStackHasItem. Slots that contain a non-null but garbage-looking
// pointer (e.g. a misaligned stride that happens to land in the middle
// of another field) disqualify the candidate base+stride.
bool slotLooksLikeItemStack(const void* slot) {
    if (!isValidPointer(slot)) return false;
    const auto* bytes = static_cast<const std::byte*>(slot);
    const auto counter = safeRead<std::uintptr_t>(bytes + sp::ItemStackBaseItem, 0);
    if (counter == 0) return true;
    if (!isValidPointer(reinterpret_cast<const void*>(counter))) return false;
    return readPtr(reinterpret_cast<const void*>(counter)) != nullptr;
}

void* getStackItem(const void* stack) {
    if (!itemStackHasItem(stack)) return nullptr;
    const auto* bytes = static_cast<const std::byte*>(stack);
    auto* counter = readPtr(bytes + sp::ItemStackBaseItem);
    if (!counter) return nullptr;
    return readPtr(counter);
}

uint16_t getItemId(const void* item) {
    if (!item) return 0;
    if (!isValidPointer(item)) return 0;
    return safeRead<uint16_t>(static_cast<std::byte*>(const_cast<void*>(item)) + sp::ItemId,
                              static_cast<uint16_t>(0));
}

short getItemMaxDamage(const void* item) {
    if (!item) return 0;
    if (!isValidPointer(item)) return 0;
    const auto vtable = safeRead<std::uintptr_t>(item, 0);
    if (vtable < kMinValidPointer) return 0;
    const auto slot = bedrocktools::sdk::offsets::VTable::ItemGetMaxDamage;
    const auto fn = safeRead<std::uintptr_t>(reinterpret_cast<const void*>(vtable + slot * sizeof(void*)), 0);
    if (fn < kMinValidPointer) return 0;
    using Fn = short (*)(const void*);
    return reinterpret_cast<Fn>(fn)(item);
}

// Probe the count byte at each candidate offset and return the first
// one that looks like a sensible stack size (1..127). Returns 0 when
// no candidate yields a sensible value or the stack is empty.
uint8_t probeStackCount(const void* stack) {
    if (!itemStackHasItem(stack)) return 0;
    const auto* bytes = static_cast<const std::byte*>(stack);
    for (auto off : kCandidateCountOffsets) {
        const uint8_t v = safeRead<uint8_t>(bytes + off, 0);
        if (v >= 1 && v <= 127) return v;
    }
    // Fallback: if no candidate looked right the stack still has an
    // item; report a count of 1 so the icon shows up without a label.
    return 1;
}

// Walks the stack's NBT tree to look for an `ench` / `Enchantments` /
// `StoredEnchantments` / `minecraft:enchantments` /
// `minecraft:stored_enchantments` key — the five vanilla names Bedrock
// has used for the enchant list over the years.
bool stackIsEnchanted(const void* stack) {
    if (!itemStackHasItem(stack)) return false;
    if (!g_nbtTreeFind) return false;
    const auto* bytes = static_cast<const std::byte*>(stack);
    void* userData = readPtr(bytes + sp::ItemStackBaseUserData);
    if (!userData || !isValidPointer(userData)) return false;
    const auto* udb = static_cast<const std::byte*>(userData);

    auto* treeRoot = readPtr(udb + sp::CompoundTagTreeRoot);
    auto* treeEnd  = readPtr(udb + sp::CompoundTagTreeEnd);
    if (!treeRoot || !treeEnd) return false;
    if (!isValidPointer(treeRoot) || !isValidPointer(treeEnd)) return false;

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

// Score a candidate (base, stride) pair against the selected hotbar
// ItemStack pointer returned by Player::getSelectedItem, if it
// resolves. Returns a higher number for better matches:
//   -1  -> disqualified: one of the slots we can see is ill-formed
//    0  -> 36 well-formed slots, selected item doesn't match or is unknown
//    1  -> 36 well-formed slots, selected item address lines up with a
//          hotbar slot (0..8) -> almost certainly the real inventory.
int scoreCandidate(std::uintptr_t base, std::size_t stride,
                   const void* selectedItem) {
    if (base < kMinValidPointer || stride == 0) return -1;
    if (stride < 0x100) return -1; // absurdly small stride -> reject fast
    int matched = 0;
    for (int slot = 0; slot < kInventorySize; ++slot) {
        const void* entry = reinterpret_cast<const void*>(base + static_cast<std::ptrdiff_t>(slot) * stride);
        if (!slotLooksLikeItemStack(entry)) return -1;
        if (selectedItem && itemStackHasItem(entry) && entry == selectedItem) {
            // Selected slot is in the hotbar (0..8). If we see the
            // selected ItemStack pointer at slot 9..35 that's not the
            // player inventory — likely a container buffer — so score
            // it worse.
            if (slot < kHotbarSize) ++matched;
            else return -1;
        }
    }
    if (matched > 0) return 1;
    return 0;
}

// Walk pointer-sized words inside the Player memory region and try each
// candidate as the base address of a flat kInventorySize-slot ItemStack
// array. Returns the winning (base, stride, countOffset) triple; the
// returned cache.valid is false if nothing usable was found.
InventoryHUDModule::InventoryCache scanForInventory(
    bedrocktools::sdk::Player* player, const void* selectedItem) {
    InventoryHUDModule::InventoryCache best;
    int bestScore = -2;

    if (!player || !isValidPointer(player)) return best;
    const auto playerBase = reinterpret_cast<std::uintptr_t>(player);

    // Phase 1: try every pointer-sized word inside the Player scan
    // window as a direct base pointer. This handles the case where
    // the inventory array is stored inline or is referenced through
    // a single indirection (unique_ptr / NonOwnerPointer).
    for (std::size_t off = 0; off + sizeof(void*) <= kPlayerScanBytes; off += sizeof(void*)) {
        const auto word = safeRead<std::uintptr_t>(
            reinterpret_cast<const void*>(playerBase + off), 0);
        if (word < kMinValidPointer) continue;
        for (auto stride : kCandidateStrides) {
            const int score = scoreCandidate(word, stride, selectedItem);
            if (score > bestScore) {
                bestScore = score;
                best.base = word;
                best.stride = stride;
                best.valid = true;
                // A perfect tie-break match is as good as it gets;
                // stop probing further strides for this word.
                if (score >= 1) break;
            }
        }
        if (bestScore >= 1) break;
    }

    // Phase 2: if no pointer word matched, also try the case where the
    // inventory is embedded inside PlayerInventory reached through one
    // extra indirection (Player -> PlayerInventory -> Items-vector begin
    // or inline array). We treat every pointer word as a possible
    // PlayerInventory* and then scan the first KB of that sub-object
    // for another pointer that leads to a good 36-slot buffer.
    if (bestScore < 0) {
        for (std::size_t off = 0; off + sizeof(void*) <= kPlayerScanBytes; off += sizeof(void*)) {
            const auto inv = safeRead<std::uintptr_t>(
                reinterpret_cast<const void*>(playerBase + off), 0);
            if (inv < kMinValidPointer) continue;
            for (std::size_t io = 0; io + sizeof(void*) <= 0x1000; io += sizeof(void*)) {
                const auto word = safeRead<std::uintptr_t>(
                    reinterpret_cast<const void*>(inv + io), 0);
                if (word < kMinValidPointer) continue;
                for (auto stride : kCandidateStrides) {
                    const int score = scoreCandidate(word, stride, selectedItem);
                    if (score > bestScore) {
                        bestScore = score;
                        best.base = word;
                        best.stride = stride;
                        best.valid = true;
                        if (score >= 1) break;
                    }
                }
                if (bestScore >= 1) break;
            }
            if (bestScore >= 1) break;
        }
    }

    // Probe the count offset. We only need one non-empty slot to learn
    // where mCount lives; try each live slot in order and keep the
    // first sensible value. If every slot is empty we leave the offset
    // at zero — getStackCount will return 0 for empty slots anyway.
    if (best.valid) {
        best.countOffset = 0;
        for (int slot = 0; slot < kInventorySize; ++slot) {
            const auto* entry = reinterpret_cast<const void*>(
                best.base + static_cast<std::ptrdiff_t>(slot) * best.stride);
            if (!itemStackHasItem(entry)) continue;
            const auto* bytes = static_cast<const std::byte*>(entry);
            for (auto coff : kCandidateCountOffsets) {
                const uint8_t v = safeRead<uint8_t>(bytes + coff, 0);
                if (v >= 1 && v <= 127) {
                    best.countOffset = coff;
                    return best;
                }
            }
        }
    }
    return best;
}

uint8_t readCachedCount(const void* stack, std::size_t countOffset) {
    if (!itemStackHasItem(stack)) return 0;
    const auto* bytes = static_cast<const std::byte*>(stack);
    if (countOffset != 0) {
        const uint8_t v = safeRead<uint8_t>(bytes + countOffset, 0);
        if (v >= 1 && v <= 127) return v;
    }
    // Cache miss or invalid value -> fall back to the probe.
    return probeStackCount(stack);
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
    m_slots.assign(kInventorySize, SlotSnapshot{});
}

InventoryHUDModule::~InventoryHUDModule() {
    if (g_module == this) g_module = nullptr;
}

void InventoryHUDModule::onInit() {
    // Resolve the SDK symbols the module relies on. None of these are new
    // signatures — they all exist in the project's signature table today.
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
        m_slots.assign(kInventorySize, SlotSnapshot{});
        m_readFailed = false;
        m_cache = InventoryCache{};
    }
    ::submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
}

void InventoryHUDModule::onLocalPlayerTick(bedrocktools::sdk::Player* player) {
    const auto now = std::chrono::steady_clock::now();

    // Throttle the full snapshot refresh just like the old code did.
    if (now - m_lastReadAt < std::chrono::milliseconds(std::max(0, m_refreshIntervalMs))) {
        return;
    }

    // If we don't have a cache yet, try the scan. The scan is much more
    // expensive than a simple snapshot read (it walks thousands of
    // words) so it is throttled independently through m_rescanIntervalMs.
    bool cacheOk = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cacheOk = m_cache.valid;
    }
    if (!cacheOk) {
        if (now - m_lastScanAt < std::chrono::milliseconds(std::max(0, m_rescanIntervalMs))) {
            return;
        }
        m_lastScanAt = now;
        const void* selected = nullptr;
        if (g_getSelectedItem && player && isValidPointer(player)) {
            // getSelectedItem returns the ItemStack* for the currently
            // selected hotbar slot. We only use it as a tie-break hint
            // during the scan; if the signature isn't resolved the scan
            // still runs (it just can't discriminate the winner on the
            // selected-slot check).
            selected = g_getSelectedItem(player);
            if (!isValidPointer(selected)) selected = nullptr;
        }
        auto found = scanForInventory(player, selected);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cache = found;
        cacheOk = found.valid;
    }

    if (!readInventory(player)) {
        // If the read failed but we previously had a cache, the cache
        // is likely stale (world unload / Player swapped). Drop it so
        // the next tick re-runs the scan instead of chasing a dead
        // pointer.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_cache.valid) m_cache = InventoryCache{};
        }
        if (!m_readFailed) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_slots.assign(kInventorySize, SlotSnapshot{});
            m_readFailed = true;
        }
        return;
    }
    m_readFailed = false;
    m_lastReadAt = now;
}

bool InventoryHUDModule::readInventory(bedrocktools::sdk::Player* player) {
    if (!player) return false;
    if (!isValidPointer(player)) return false;

    // Cross-check that the tick event's Player matches the local
    // player we can reach through ClientInstance (the tick event only
    // fires for the local player but a bug in the hook could hand us
    // a remote actor).
    auto* client = bedrocktools::sdk::ClientInstance::current();
    if (client) {
        auto* localPlayer = client->localPlayer();
        if (localPlayer && localPlayer != player) {
            return false;
        }
    }

    InventoryCache cache;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cache = m_cache;
    }
    if (!cache.valid || cache.base < kMinValidPointer || cache.stride == 0) return false;

    // Verify the buffer still looks well-formed before reading — if
    // the game swapped out the inventory (world transition) the old
    // pointer may still be mapped but no longer hold ItemStacks.
    // A single bad slot is enough to invalidate the cache.
    for (int slot = 0; slot < kInventorySize; ++slot) {
        const auto* entry = reinterpret_cast<const void*>(
            cache.base + static_cast<std::ptrdiff_t>(slot) * cache.stride);
        if (!slotLooksLikeItemStack(entry)) return false;
    }

    std::vector<SlotSnapshot> next(kInventorySize);
    int readCount = 0;
    for (int slot = 0; slot < kInventorySize; ++slot) {
        const auto* stack = reinterpret_cast<const void*>(
            cache.base + static_cast<std::ptrdiff_t>(slot) * cache.stride);
        if (!itemStackHasItem(stack)) continue;
        SlotSnapshot snap;
        snap.hasItem = true;
        snap.count = readCachedCount(stack, cache.countOffset);
        snap.damage = g_getDamageValue ? g_getDamageValue(const_cast<void*>(stack)) : 0;
        void* item = getStackItem(stack);
        snap.itemId = getItemId(item);
        snap.maxDamage = getItemMaxDamage(item);
        snap.enchanted = stackIsEnchanted(stack);
        next[slot] = snap;
        ++readCount;
    }
    (void)readCount; // 0 is acceptable (empty inventory in a fresh world).

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

    // 9 columns of slots is the vanilla layout.
    constexpr int kColumns = 9;
    const int slotOffset = std::clamp(m_slotOffset, 0, kInventorySize - 1);
    const int slotCount = std::clamp(m_slotCount, 0, kInventorySize - slotOffset);
    const int totalSlots = slotOffset + slotCount;
    if (slotCount <= 0) {
        ::submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
        return;
    }
    const int rows = (totalSlots + kColumns - 1) / kColumns;

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

        const uint32_t itemRgb = (static_cast<uint32_t>(snap.itemId) * 0x9E3779B1u) & 0x00FFFFFFu;
        PLModMenu_DrawCommand itemBg{};
        itemBg.type = PL_DRAW_RECT_FILLED;
        itemBg.x = x;
        itemBg.y = y;
        itemBg.w = slotSize;
        itemBg.h = slotSize;
        itemBg.color = (0xC0u << 24) | itemRgb;
        cmds.push_back(itemBg);

        PLModMenu_DrawCommand itemBorder{};
        itemBorder.type = PL_DRAW_RECT;
        itemBorder.x = x + 0.5f * scale;
        itemBorder.y = y + 0.5f * scale;
        itemBorder.w = slotSize - 1.0f * scale;
        itemBorder.h = slotSize - 1.0f * scale;
        itemBorder.size = 1.0f * scale;
        itemBorder.color = 0x80FFFFFFu;
        cmds.push_back(itemBorder);

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
    if (j.contains("m_rescanIntervalMs")) m_rescanIntervalMs = j["m_rescanIntervalMs"].get<int>();

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
    j["m_rescanIntervalMs"] = m_rescanIntervalMs;

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

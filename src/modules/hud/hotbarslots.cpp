#include "hotbarslots.hpp"

#include "core/memory/Hooks.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <pl/ModMenu.hpp>
#include <pl/ModMenuConfig.hpp>
#include <pl/memory/Vtable.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using bedrocktools::hotbar::SlotRect;
using bedrocktools::hotbar::StripLayout;

constexpr std::size_t SlotCount = HotbarSlotsModule::SlotCount;
constexpr std::size_t FillingContainerItemsOffset = bedrocktools::sdk::offsets::Inventory::FillingContainerItems;
constexpr std::size_t ItemStackSize = bedrocktools::sdk::offsets::Inventory::ItemStackSize;
constexpr std::size_t MaxContainerSlots = 64;
constexpr float VanillaItemSize = 16.0f;
constexpr const char* MinecraftLibrary = "libminecraftpe.so";

// android.view.KeyEvent.KEYCODE_1. The launcher forwards the key code of an
// overlay button straight to the game, which maps 1-9 to the hotbar slots.
constexpr int AndroidKeyCode1 = 8;

// Prefix of the overlay button ids registered below ("<prefix>1".."9"). The
// JNI geometry query reports plain button ids, and this prefix maps them back
// to slot indices (see slotIndexFromButtonId).
constexpr std::string_view ButtonIdPrefix = "bedrocktoolsplus.HotbarSlots.Button";

struct RectangleArea {
    float x0;
    float x1;
    float y0;
    float y1;
};

struct Color {
    float r;
    float g;
    float b;
    float a;
};

class HashedString {
public:
    std::uint64_t hash;
    std::string value;
    mutable const HashedString* lastMatch;

    explicit HashedString(const char* text)
        : hash(computeHash(text ? std::string_view(text) : std::string_view())),
          value(text ? text : ""),
          lastMatch(nullptr) {}

private:
    static std::uint64_t computeHash(std::string_view text) {
        if (text.empty()) return 0;
        constexpr std::uint64_t offset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t prime = 0x100000001B3ULL;
        std::uint64_t result = offset;
        for (char character : text) {
            result = static_cast<std::uint64_t>(static_cast<unsigned char>(character)) ^ (prime * result);
        }
        return result;
    }
};

using HudCameraRendererFn = void (*)(void*, void*, void*, void*, int);
using BaseActorRenderContextCtorFn = void (*)(void*, void*, void*, void*);
using ItemRendererRenderGuiItemNewFn = std::uint64_t (*)(
    void*, void*, void*, unsigned int, unsigned char, std::uint64_t,
    float, float, float, float, float);

HudCameraRendererFn hudCameraRendererOriginal = nullptr;
BaseActorRenderContextCtorFn baseActorRenderContextCtor = nullptr;
ItemRendererRenderGuiItemNewFn itemRendererRenderGuiItemNew = nullptr;
HotbarSlotsModule* moduleInstance = nullptr;
bedrocktools::hooks::Handle hudRendererHook = nullptr;

constexpr const char* HudElementId = "bedrocktools.hotbarslots.strip";

void** getVtable(void* object) {
    return object ? *reinterpret_cast<void***>(object) : nullptr;
}

void* getLocalPlayer(void* client) {
    void** vtable = getVtable(client);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer]) return nullptr;
    return reinterpret_cast<void* (*)(void*)>(
        vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer])(client);
}

void* getCarriedItem(void* player) {
    void** vtable = getVtable(player);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::PlayerGetCarriedItem]) return nullptr;
    return reinterpret_cast<void* (*)(void*)>(
        vtable[bedrocktools::sdk::offsets::VTable::PlayerGetCarriedItem])(player);
}

void* getMinecraftGame(void* client) {
    if (!client) return nullptr;
    void** vtable = getVtable(client);
    if (vtable && vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetMinecraftGame]) {
        void* game = reinterpret_cast<void* (*)(void*)>(
            vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetMinecraftGame])(client);
        if (game) return game;
    }
    return *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(client) +
        bedrocktools::sdk::offsets::ShulkerPreview::ClientInstanceMinecraftGame);
}

void* getStackItem(void* stack) {
    if (!stack) return nullptr;
    void* counter = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(stack) +
        bedrocktools::sdk::offsets::ShulkerPreview::ItemStackBaseItem);
    if (!counter) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(counter) +
        bedrocktools::sdk::offsets::ShulkerPreview::SharedCounterPointer);
}

unsigned int getItemAnimationFrame(void* item, void* localPlayer, void* stack) {
    if (!item || !localPlayer || !stack) return 0;
    void** vtable = getVtable(item);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::ItemGetAnimationFrameFor]) return 0;
    using Fn = unsigned int (*)(void*, void*, int, void*, int);
    return reinterpret_cast<Fn>(
        vtable[bedrocktools::sdk::offsets::VTable::ItemGetAnimationFrameFor])(item, localPlayer, 0, stack, 1);
}

RectangleArea getFullClippingRectangle(void* context) {
    RectangleArea result{};
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetFullClippingRectangle])
        return result;
    using Fn = RectangleArea (*)(void*);
    return reinterpret_cast<Fn>(
        vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetFullClippingRectangle])(context);
}

void flushImages(void* context) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFlushImages]) return;
    using Fn = void (*)(void*, const Color&, float, const HashedString&);
    static const HashedString material("ui_flush");
    static constexpr Color color{1.0f, 1.0f, 1.0f, 1.0f};
    reinterpret_cast<Fn>(
        vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFlushImages])(context, color, 1.0f, material);
}

bool validRectangle(const RectangleArea& area) {
    return std::isfinite(area.x0) && std::isfinite(area.x1) &&
           std::isfinite(area.y0) && std::isfinite(area.y1) &&
           area.x1 > area.x0 && area.y1 > area.y0;
}

void destroyBaseActorRenderContext(void* context) {
    void** vtable = getVtable(context);
    if (vtable && vtable[0]) reinterpret_cast<void (*)(void*)>(vtable[0])(context);
}

// The first nine slots of the player inventory are the hotbar.
std::uintptr_t inventoryItems(void* player, std::size_t& count) {
    count = 0;
    if (!player) return 0;
    auto* proxy = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(player) +
        bedrocktools::sdk::offsets::Inventory::PlayerInventory);
    if (!proxy) return 0;
    auto* container = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(proxy) +
        bedrocktools::sdk::offsets::Inventory::PlayerInventoryContainer);
    if (!container) return 0;
    auto* bytes = reinterpret_cast<std::byte*>(container);
    const auto begin = *reinterpret_cast<std::uintptr_t*>(bytes + FillingContainerItemsOffset);
    const auto end = *reinterpret_cast<std::uintptr_t*>(bytes + FillingContainerItemsOffset + sizeof(void*));
    if (!begin || end < begin) return 0;
    const auto span = end - begin;
    if (span % ItemStackSize != 0) return 0;
    const auto slots = span / ItemStackSize;
    if (slots > MaxContainerSlots) return 0;
    count = static_cast<std::size_t>(slots);
    return begin;
}

std::array<void*, SlotCount> getHotbarStacks(void* player) {
    std::array<void*, SlotCount> stacks{};
    std::size_t count = 0;
    const std::uintptr_t begin = inventoryItems(player, count);
    if (!begin) return stacks;
    for (std::size_t i = 0; i < SlotCount && i < count; ++i) {
        stacks[i] = reinterpret_cast<void*>(begin + i * ItemStackSize);
    }
    return stacks;
}

// The selected slot index is derived by matching the carried item stack
// against the hotbar stacks. That avoids depending on an Inventory field
// offset that changes between Minecraft versions.
int selectedSlotFor(void* player, const std::array<void*, SlotCount>& stacks) {
    void* carried = getCarriedItem(player);
    if (!carried) return -1;
    for (std::size_t i = 0; i < SlotCount; ++i) {
        if (stacks[i] && stacks[i] == carried) return static_cast<int>(i);
    }
    return -1;
}

std::uint32_t parseColor(const std::string& value, std::uint32_t fallback) {
    if (value.empty()) return fallback;
    const std::string hex = value[0] == '#' ? value.substr(1) : value;
    try {
        if (hex.size() == 6) return 0xFF000000u | static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
        if (hex.size() == 8) return static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
    } catch (...) {
    }
    return fallback;
}

std::uint32_t withOpacity(std::uint32_t color, float opacity) {
    const auto alpha = static_cast<std::uint32_t>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f);
    return (alpha << 24) | (color & 0x00FFFFFFu);
}

void hudCameraRendererDetour(void* self, void* context, void* client, void* value, int pass) {
    if (hudCameraRendererOriginal) hudCameraRendererOriginal(self, context, client, value, pass);
    if (moduleInstance && moduleInstance->enabled) moduleInstance->renderNative(context, client);
}

// Minecraft-style slot frame, matching the look of the launcher's hotbar
// buttons (a light bevel around a darker face).
constexpr const char* slotButtonSvg = R"svg(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">
    <path fill="#C6C6C6" stroke="#373737" stroke-width="2" d="M2,2 L62,2 L62,62 L2,62 Z M4,4 L60,4 L60,60 L4,60 Z"/>
    <path fill="#8B8B8B" stroke="#5B5B5B" stroke-width="2" d="M6,6 L58,6 L58,58 L6,58 Z M8,8 L56,8 L56,56 L8,56 Z"/>
</svg>)svg";

constexpr const char* slotButtonActiveSvg = R"svg(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">
    <path fill="#C6C6C6" stroke="#373737" stroke-width="2" d="M2,2 L62,2 L62,62 L2,62 Z M4,4 L60,4 L60,60 L4,60 Z"/>
    <g transform="translate(32, 32) scale(0.85) translate(-32, -32)">
        <path fill="#8B8B8B" stroke="#5B5B5B" stroke-width="2" d="M6,6 L58,6 L58,58 L6,58 Z M8,8 L56,8 L56,56 L8,56 Z"/>
    </g>
</svg>)svg";

// Hollow frame around an on-screen button, drawn as four Line commands (Line
// is used across the existing modules, while an outline Rect type is not
// available on every launcher build).
void pushHollowFrame(std::vector<pl::modmenu::DrawCommand>& commands, float x, float y, float w,
                     float h, std::uint32_t color, float thickness) {
    struct Edge {
        float x1;
        float y1;
        float x2;
        float y2;
    };
    const Edge edges[4] = {
        {x, y, x + w, y},
        {x + w, y, x + w, y + h},
        {x + w, y + h, x, y + h},
        {x, y + h, x, y},
    };
    for (const Edge& edge : edges) {
        pl::modmenu::DrawCommand line;
        line.type = pl::modmenu::DrawCommandType::Line;
        line.x = edge.x1;
        line.y = edge.y1;
        line.w = edge.x2 - edge.x1; // the launcher treats w/h as the end-point delta
        line.h = edge.y2 - edge.y1;
        line.color = color;
        line.size = thickness;
        commands.push_back(std::move(line));
    }
}

} // namespace

HotbarSlotsModule::HotbarSlotsModule()
    : Module("Hotbar Slots",
             "Adds separate 1-9 buttons for selecting hotbar slots, with optional item icons on the HUD or inside the buttons.") {
    moduleInstance = this;
    m_slotEnabled.fill(true);
}

HotbarSlotsModule::~HotbarSlotsModule() {
    unregisterOverlayButtons();
    if (hudRendererHook) {
        bedrocktools::hooks::remove(hudRendererHook);
        hudRendererHook = nullptr;
        hudCameraRendererOriginal = nullptr;
    }
    if (moduleInstance == this) moduleInstance = nullptr;
}

std::string HotbarSlotsModule::buttonId(std::size_t index) {
    return std::string(ButtonIdPrefix) + std::to_string(index + 1);
}

void HotbarSlotsModule::onInit() {
    baseActorRenderContextCtor = reinterpret_cast<BaseActorRenderContextCtorFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BaseActorRenderContextCtor));
    itemRendererRenderGuiItemNew = reinterpret_cast<ItemRendererRenderGuiItemNewFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemRendererRenderGuiItemNew));

    const std::uintptr_t hudRenderer = pl::memory::resolveVtableFunction(
        "17HudCameraRenderer",
        bedrocktools::sdk::offsets::VTable::HudCameraRendererRender,
        MinecraftLibrary);
    if (hudRenderer && !hudRendererHook) {
        hudRendererHook = bedrocktools::hooks::install(
            reinterpret_cast<void*>(hudRenderer),
            reinterpret_cast<void*>(hudCameraRendererDetour),
            reinterpret_cast<void**>(&hudCameraRendererOriginal));
    }

    syncOverlayButtons();
}

void HotbarSlotsModule::onEnable() {
    syncOverlayButtons();
}

void HotbarSlotsModule::onDisable() {
    clearRuntime();
    unregisterOverlayButtons();
    pl::modmenu::submitDrawCommands(moduleId, std::span<const pl::modmenu::DrawCommand>{});
    pl::modmenu::submitHudEditorElements(moduleId, std::span<const pl::modmenu::HudEditorElement>{});
}

void HotbarSlotsModule::unregisterOverlayButtons() {
    for (std::size_t i = 0; i < SlotCount; ++i) pl::modmenu::unregisterButton(buttonId(i));
}

void HotbarSlotsModule::syncOverlayButtons() {
    unregisterOverlayButtons();
    if (!m_buttons) return;

    const float scale = std::clamp(m_buttonScale, 0.5f, 2.0f);
    for (std::size_t i = 0; i < SlotCount; ++i) {
        if (!m_slotEnabled[i]) continue;
        const std::string label = std::to_string(i + 1);
        pl::modmenu::ButtonBuilder builder(buttonId(i), "Hotbar Slot " + label);
        builder.moduleId(moduleId)
            .label(label)
            // The launcher delivers this key code to Minecraft, which selects
            // the matching hotbar slot exactly like a hardware keyboard would.
            .androidKeyCode(AndroidKeyCode1 + static_cast<int>(i))
            .behavior(pl::modmenu::ButtonBehavior::Hold)
            .defaultVisible(true)
            .stylePreset(pl::modmenu::ButtonStylePreset::Accent)
            .styleColors(0x00000001, 0x00000001, 0x00000001)
            .svgIcon(slotButtonSvg, false)
            .activeSvgIcon(slotButtonActiveSvg)
            .textColor(0xFF373737u)
            .activeTextColor(0xFF1F1F1Fu)
            .sizeScale(scale, scale);
        (void)builder.registerButton();
    }
}

HotbarSlotsModule::ConfigSnapshot HotbarSlotsModule::snapshotConfig() const {
    std::lock_guard lock(m_configMutex);
    ConfigSnapshot config;
    config.slots = m_slotEnabled;
    config.buttons = m_buttons;
    config.itemIcons = m_itemIcons;
    config.iconsOnButtons = m_iconsOnButtons;
    config.slotNumbers = m_slotNumbers;
    config.highlightSelected = m_highlightSelected;
    config.buttonGeometry = m_buttonGeometry;
    config.buttonGeometryValid = m_buttonGeometryValid;
    config.layout.x = hudPosX;
    config.layout.y = hudPosY;
    config.layout.slotSize = m_slotSize;
    config.layout.gap = m_slotGap;
    config.layout.vertical = m_vertical;
    config.buttonScale = m_buttonScale;
    config.numberTextSize = m_numberTextSize;
    config.numberColor = parseColor(m_numberColor, 0xFFFFFFFFu);
    config.highlightColor = withOpacity(parseColor(m_highlightColor, 0xFFFFFFFFu), m_highlightOpacity);
    config.gridSize = m_gridSize;
    config.gridGap = m_gridGap;
    config.snapThreshold = m_snapThreshold;
    config.snapFlags =
        (m_snapToGrid ? pl::modmenu::HudSnapGrid : pl::modmenu::HudSnapNone) |
        (m_snapToElements ? pl::modmenu::HudSnapElements : pl::modmenu::HudSnapNone) |
        (m_snapToScreenCenter ? pl::modmenu::HudSnapScreenCenter : pl::modmenu::HudSnapNone);
    return config;
}

void HotbarSlotsModule::clearRuntime() {
    for (auto& slot : m_hasItem) slot.store(false, std::memory_order_release);
    m_selectedSlot.store(-1, std::memory_order_release);
}

void HotbarSlotsModule::refreshButtonGeometryIfDue() {
    std::lock_guard lock(m_configMutex);
    if (!m_iconsOnButtons || !m_buttons || !m_itemIcons) {
        // The strip is the only destination: drop any cached rectangles so
        // both render paths fall back to it immediately.
        m_buttonGeometryValid.fill(false);
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < m_geometryRefreshAt) return;
    m_geometryRefreshAt = now + std::chrono::milliseconds(500);

    // The query walks several Java objects per button, so it runs here on the
    // frame thread a few times per second instead of inside the render hook.
    // Buttons only move while the user drags them, and any slot without a
    // fresh rectangle simply keeps using its strip position.
    m_buttonGeometryValid.fill(false);
    for (const auto& geometry : bedrocktools::launcher::queryButtonGeometry(moduleId)) {
        const int slot = bedrocktools::hotbar::slotIndexFromButtonId(geometry.buttonId, ButtonIdPrefix);
        if (slot < 0 || static_cast<std::size_t>(slot) >= SlotCount) continue;
        if (!m_slotEnabled[static_cast<std::size_t>(slot)]) continue;
        if (geometry.width <= 0.0f || geometry.height <= 0.0f) continue;
        m_buttonGeometry[static_cast<std::size_t>(slot)] = geometry;
        m_buttonGeometryValid[static_cast<std::size_t>(slot)] = true;
    }
}

void HotbarSlotsModule::renderNative(void* context, void* client) {
    if (!context || !client || !baseActorRenderContextCtor || !itemRendererRenderGuiItemNew) {
        clearRuntime();
        return;
    }

    void* localPlayer = getLocalPlayer(client);
    if (!localPlayer) {
        clearRuntime();
        return;
    }

    const ConfigSnapshot config = snapshotConfig();
    const auto stacks = getHotbarStacks(localPlayer);
    m_selectedSlot.store(selectedSlotFor(localPlayer, stacks), std::memory_order_release);

    const pl::modmenu::HudSurfaceSize surface = pl::modmenu::getHudSurfaceSize();
    const RectangleArea full = getFullClippingRectangle(context);
    const bool canRender = config.itemIcons && surface.width > 0.0f && surface.height > 0.0f && validRectangle(full);

    alignas(16) std::byte baseActorRenderContext[
        bedrocktools::sdk::offsets::ShulkerPreview::BaseActorRenderContextStorageSize]{};
    void* itemRenderer = nullptr;
    if (canRender) {
        void* screenContext = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(context) +
            bedrocktools::sdk::offsets::ShulkerPreview::MinecraftUIRenderContextScreenContext);
        void* game = getMinecraftGame(client);
        if (screenContext && game) {
            baseActorRenderContextCtor(baseActorRenderContext, screenContext, client, game);
            itemRenderer = *reinterpret_cast<void**>(baseActorRenderContext +
                bedrocktools::sdk::offsets::ShulkerPreview::BaseActorRenderContextItemRenderer);
        }
    }

    const float uiWidth = full.x1 - full.x0;
    const float uiHeight = full.y1 - full.y0;
    bool renderedAny = false;

    for (std::size_t i = 0; i < SlotCount; ++i) {
        void* stack = stacks[i];
        void* item = getStackItem(stack);
        m_hasItem[i].store(item != nullptr, std::memory_order_release);

        if (!config.slots[i] || !item || !itemRenderer || !canRender) continue;

        // Destination rectangle in HUD units: the on-screen button when its
        // geometry is known (screen pixels coincide with HUD units because
        // the launcher hosts overlay buttons in a fullscreen window),
        // otherwise the strip slot as before (automatic fallback).
        float dstX;
        float dstY;
        float dstW;
        float dstH;
        if (config.iconsOnButtons && config.buttonGeometryValid[i]) {
            const auto& geometry = config.buttonGeometry[i];
            const auto icon = bedrocktools::hotbar::iconRectInButton(
                geometry.x, geometry.y, geometry.width, geometry.height);
            dstX = icon.x;
            dstY = icon.y;
            dstW = icon.width;
            dstH = icon.height;
        } else {
            const SlotRect rect = bedrocktools::hotbar::slotRect(config.layout, i);
            if (rect.size <= 0.0f) continue;
            dstX = rect.x;
            dstY = rect.y;
            dstW = rect.size;
            dstH = rect.size;
        }
        if (dstW <= 0.0f || dstH <= 0.0f) continue;

        const float x = full.x0 + dstX * uiWidth / surface.width;
        const float y = full.y0 + dstY * uiHeight / surface.height;
        const float width = dstW * uiWidth / surface.width;
        const float height = dstH * uiHeight / surface.height;
        const float iconSize = std::max(1.0f, std::min(width, height));
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(iconSize)) continue;

        const unsigned int animationFrame = getItemAnimationFrame(item, localPlayer, stack);
        itemRendererRenderGuiItemNew(
            itemRenderer,
            baseActorRenderContext,
            stack,
            animationFrame,
            0,
            0,
            x,
            y,
            1.0f,
            1.0f,
            iconSize / VanillaItemSize);
        renderedAny = true;
    }

    if (itemRenderer) destroyBaseActorRenderContext(baseActorRenderContext);
    if (renderedAny) flushImages(context);
}

void HotbarSlotsModule::onFrame() {
    if (!enabled) return;

    refreshButtonGeometryIfDue();
    const ConfigSnapshot config = snapshotConfig();

    std::size_t visible = 0;
    for (bool slot : config.slots) visible += slot ? 1 : 0;

    std::vector<pl::modmenu::HudEditorElement> elements;
    if (visible > 0) {
        pl::modmenu::HudEditorElement element;
        element.elementId = HudElementId;
        element.displayName = "Hotbar Slots";
        element.positionKeyX = "hudPosX";
        element.positionKeyY = "hudPosY";
        element.x = config.layout.x;
        element.y = config.layout.y;
        element.width = std::max(1.0f, bedrocktools::hotbar::stripWidth(config.layout, SlotCount));
        element.height = std::max(1.0f, bedrocktools::hotbar::stripHeight(config.layout, SlotCount));
        element.gridSize = config.gridSize;
        element.snapThreshold = config.snapThreshold;
        element.gridGap = config.gridGap;
        element.snapFlags = config.snapFlags;
        elements.push_back(std::move(element));
    }
    pl::modmenu::submitHudEditorElements(moduleId, elements);

    std::vector<pl::modmenu::DrawCommand> commands;
    const int selected = m_selectedSlot.load(std::memory_order_acquire);

    for (std::size_t i = 0; i < SlotCount; ++i) {
        if (!config.slots[i]) continue;

        // Slots whose button geometry is known get a hollow frame around the
        // button (where the native icon is painted); the rest keep the
        // classic strip highlight/number so partially-known states never lose
        // content.
        if (config.iconsOnButtons && config.buttonGeometryValid[i]) {
            const auto& geometry = config.buttonGeometry[i];
            const bool isSelected = config.highlightSelected && selected == static_cast<int>(i);
            if (isSelected) {
                pl::modmenu::DrawCommand highlight;
                highlight.type = pl::modmenu::DrawCommandType::RectFilled;
                highlight.x = geometry.x;
                highlight.y = geometry.y;
                highlight.w = geometry.width;
                highlight.h = geometry.height;
                highlight.color = config.highlightColor;
                commands.push_back(std::move(highlight));
            }
            pushHollowFrame(commands, geometry.x, geometry.y, geometry.width, geometry.height,
                            isSelected ? config.highlightColor : config.numberColor, 2.0f);
            if (config.slotNumbers) {
                pl::modmenu::DrawCommand number;
                number.type = pl::modmenu::DrawCommandType::Text;
                number.x = geometry.x + 2.0f;
                number.y = geometry.y + config.numberTextSize;
                number.color = config.numberColor;
                number.size = config.numberTextSize;
                number.text = std::to_string(i + 1);
                commands.push_back(std::move(number));
            }
            continue;
        }

        const SlotRect rect = bedrocktools::hotbar::slotRect(config.layout, i);

        if (config.highlightSelected && selected == static_cast<int>(i)) {
            pl::modmenu::DrawCommand highlight;
            highlight.type = pl::modmenu::DrawCommandType::RectFilled;
            highlight.x = rect.x;
            highlight.y = rect.y;
            highlight.w = rect.size;
            highlight.h = rect.size;
            highlight.color = config.highlightColor;
            commands.push_back(std::move(highlight));
        }

        if (config.slotNumbers) {
            pl::modmenu::DrawCommand number;
            number.type = pl::modmenu::DrawCommandType::Text;
            number.x = rect.x + 2.0f;
            number.y = rect.y + config.numberTextSize;
            number.color = config.numberColor;
            number.size = config.numberTextSize;
            number.text = std::to_string(i + 1);
            commands.push_back(std::move(number));
        }
    }

    pl::modmenu::submitDrawCommands(moduleId, commands);
}

void HotbarSlotsModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("slots")
        .category("slots", "Slots", "Choose which hotbar slots are shown")
        .category("appearance", "Appearance", "Size, spacing and colors of the slot strip")
        .category("editor", "HUD Editor", "Placement and snapping while editing the HUD");

    auto node = [](std::string key, std::string title, std::string category, ConfigControlTypeV2 type) {
        ConfigNodeV2 value;
        value.id = key;
        value.key = std::move(key);
        value.title = std::move(title);
        value.category = std::move(category);
        value.type = type;
        return value;
    };
    auto section = [&](const char* id, const char* title, const char* category) {
        auto value = node(id, title, category, ConfigControlTypeV2::Section);
        value.key.clear();
        schema.node(std::move(value));
    };
    auto slider = [&](const char* key, std::string title, const char* category,
                      const char* sectionId, const char* min, const char* max,
                      const char* unit = " px") {
        auto value = node(key, std::move(title), category, ConfigControlTypeV2::SliderFloat);
        value.section = sectionId;
        value.minValue = min;
        value.maxValue = max;
        value.step = "1";
        value.unit = unit;
        schema.node(std::move(value));
    };

    section("slot_toggles", "Visible Slots", "slots");
    auto slots = node("slot_visibility", "Slots", "slots", ConfigControlTypeV2::ToggleGroup);
    slots.key.clear();
    slots.section = "slot_toggles";
    slots.choiceStyle = ConfigChoiceStyleV2::Chips;
    for (std::size_t i = 0; i < SlotCount; ++i) {
        const std::string key = "m_slot" + std::to_string(i + 1);
        slots.options.push_back({key, std::to_string(i + 1), {}, key});
    }
    schema.node(std::move(slots));

    section("slot_features", "Features", "slots");
    auto features = node("slot_features_group", "Show", "slots", ConfigControlTypeV2::ToggleGroup);
    features.key.clear();
    features.section = "slot_features";
    features.description = "On-screen buttons select the slot; the HUD strip shows what each slot holds. Draw Icons On Buttons paints each icon inside its button instead, falling back to the strip while a button is hidden.";
    features.choiceStyle = ConfigChoiceStyleV2::Checklist;
    features.options = {
        {"buttons", "On-Screen Buttons", {}, "m_buttons"},
        {"icons", "Item Icons", {}, "m_itemIcons"},
        {"buttonicons", "Draw Icons On Buttons", {}, "m_iconsOnButtons"},
        {"numbers", "Slot Numbers", {}, "m_slotNumbers"},
        {"highlight", "Highlight Selected Slot", {}, "m_highlightSelected"}
    };
    schema.node(std::move(features));

    section("strip", "Slot Strip", "appearance");
    auto orientation = node("m_vertical", "Vertical Strip", "appearance", ConfigControlTypeV2::Toggle);
    orientation.section = "strip";
    schema.node(std::move(orientation));
    slider("m_slotSize", "Slot Size", "appearance", "strip", "8", "100");
    slider("m_slotGap", "Gap Between Slots", "appearance", "strip", "0", "50");
    slider("m_buttonScale", "Button Scale", "appearance", "strip", "0.5", "2", "x");

    section("strip_text", "Text & Highlight", "appearance");
    slider("m_numberTextSize", "Number Size", "appearance", "strip_text", "6", "40");
    auto numberColor = node("m_numberColor", "Number Color", "appearance", ConfigControlTypeV2::Color);
    numberColor.section = "strip_text";
    numberColor.defaultValue = "#FFFFFF";
    schema.node(std::move(numberColor));
    auto highlightColor = node("m_highlightColor", "Highlight Color", "appearance", ConfigControlTypeV2::Color);
    highlightColor.section = "strip_text";
    highlightColor.defaultValue = "#FFFFFF";
    schema.node(std::move(highlightColor));
    slider("m_highlightOpacity", "Highlight Opacity", "appearance", "strip_text", "0", "1", "");

    auto help = node("editor_help", "Move The Strip In The HUD Editor", "editor", ConfigControlTypeV2::Info);
    help.key.clear();
    help.description = "Open the HUD Editor to drag the slot strip. The on-screen buttons keep their own positions.";
    schema.node(std::move(help));
    section("snapping", "Snapping", "editor");
    auto snapping = node("snap_targets", "Snap To", "editor", ConfigControlTypeV2::ToggleGroup);
    snapping.key.clear();
    snapping.section = "snapping";
    snapping.choiceStyle = ConfigChoiceStyleV2::Chips;
    snapping.options = {
        {"grid", "Grid", {}, "m_snapToGrid"},
        {"items", "Other Elements", {}, "m_snapToElements"},
        {"center", "Screen Center", {}, "m_snapToScreenCenter"}
    };
    schema.node(std::move(snapping));
    slider("m_gridSize", "Grid Size", "editor", "snapping", "1", "100");
    slider("m_gridGap", "Gap Between Elements", "editor", "snapping", "0", "100");
    slider("m_snapThreshold", "Snap Distance", "editor", "snapping", "1", "100");

    pl::modmenu::setConfigSchemaJson(moduleId, schema.toJson());
}

void HotbarSlotsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    const auto previousSlots = m_slotEnabled;
    const bool previousButtons = m_buttons;
    const float previousScale = m_buttonScale;

    {
        std::lock_guard lock(m_configMutex);
        for (std::size_t i = 0; i < SlotCount; ++i) {
            const std::string key = "m_slot" + std::to_string(i + 1);
            if (j.contains(key) && j[key].is_boolean()) m_slotEnabled[i] = j[key].get<bool>();
        }
        if (j.contains("m_buttons")) m_buttons = j["m_buttons"].get<bool>();
        if (j.contains("m_itemIcons")) m_itemIcons = j["m_itemIcons"].get<bool>();
        if (j.contains("m_iconsOnButtons")) m_iconsOnButtons = j["m_iconsOnButtons"].get<bool>();
        if (j.contains("m_slotNumbers")) m_slotNumbers = j["m_slotNumbers"].get<bool>();
        if (j.contains("m_highlightSelected")) m_highlightSelected = j["m_highlightSelected"].get<bool>();
        if (j.contains("m_vertical")) m_vertical = j["m_vertical"].get<bool>();
        if (j.contains("hudPosX")) hudPosX = std::clamp(j["hudPosX"].get<float>(), 0.0f, 4000.0f);
        if (j.contains("hudPosY")) hudPosY = std::clamp(j["hudPosY"].get<float>(), 0.0f, 4000.0f);
        if (j.contains("m_slotSize")) m_slotSize = std::clamp(j["m_slotSize"].get<float>(), 8.0f, 100.0f);
        if (j.contains("m_slotGap")) m_slotGap = std::clamp(j["m_slotGap"].get<float>(), 0.0f, 50.0f);
        if (j.contains("m_buttonScale")) m_buttonScale = std::clamp(j["m_buttonScale"].get<float>(), 0.5f, 2.0f);
        if (j.contains("m_numberTextSize"))
            m_numberTextSize = std::clamp(j["m_numberTextSize"].get<float>(), 6.0f, 40.0f);
        if (j.contains("m_numberColor")) m_numberColor = j["m_numberColor"].get<std::string>();
        if (j.contains("m_highlightColor")) m_highlightColor = j["m_highlightColor"].get<std::string>();
        if (j.contains("m_highlightOpacity"))
            m_highlightOpacity = std::clamp(j["m_highlightOpacity"].get<float>(), 0.0f, 1.0f);
        if (j.contains("m_gridSize")) m_gridSize = std::clamp(j["m_gridSize"].get<float>(), 1.0f, 100.0f);
        if (j.contains("m_gridGap")) m_gridGap = std::clamp(j["m_gridGap"].get<float>(), 0.0f, 100.0f);
        if (j.contains("m_snapThreshold")) m_snapThreshold = std::clamp(j["m_snapThreshold"].get<float>(), 1.0f, 100.0f);
        if (j.contains("m_snapToGrid")) m_snapToGrid = j["m_snapToGrid"].get<bool>();
        if (j.contains("m_snapToElements")) m_snapToElements = j["m_snapToElements"].get<bool>();
        if (j.contains("m_snapToScreenCenter")) m_snapToScreenCenter = j["m_snapToScreenCenter"].get<bool>();
    }

    // Registering the launcher buttons again is only needed when something the
    // launcher itself renders changed; unrelated edits must not churn the
    // button registry (and reset the user's button placement).
    bool overlayChanged = previousButtons != m_buttons || previousScale != m_buttonScale;
    for (std::size_t i = 0; i < SlotCount && !overlayChanged; ++i)
        overlayChanged = previousSlots[i] != m_slotEnabled[i];
    if (overlayChanged) syncOverlayButtons();
}

void HotbarSlotsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard lock(m_configMutex);

    for (std::size_t i = 0; i < SlotCount; ++i) j["m_slot" + std::to_string(i + 1)] = m_slotEnabled[i];
    j["m_buttons"] = m_buttons;
    j["m_itemIcons"] = m_itemIcons;
    j["m_iconsOnButtons"] = m_iconsOnButtons;
    j["m_slotNumbers"] = m_slotNumbers;
    j["m_highlightSelected"] = m_highlightSelected;
    j["m_vertical"] = m_vertical;
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["m_slotSize"] = m_slotSize;
    j["m_slotGap"] = m_slotGap;
    j["m_buttonScale"] = m_buttonScale;
    j["m_numberTextSize"] = m_numberTextSize;
    j["m_numberColor"] = m_numberColor;
    j["m_highlightColor"] = m_highlightColor;
    j["m_highlightOpacity"] = m_highlightOpacity;
    j["m_gridSize"] = m_gridSize;
    j["m_gridGap"] = m_gridGap;
    j["m_snapThreshold"] = m_snapThreshold;
    j["m_snapToGrid"] = m_snapToGrid;
    j["m_snapToElements"] = m_snapToElements;
    j["m_snapToScreenCenter"] = m_snapToScreenCenter;
}

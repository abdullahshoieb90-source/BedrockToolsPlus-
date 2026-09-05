#include "hotbarslots.hpp"

#include "huditems.hpp"
#include "launcher/ExternalButtonGeometry.hpp"
#include "modules/ModuleRegistry.hpp"

#include <pl/ModMenu.hpp>
#include <pl/ModMenuConfig.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

namespace huditems = bedrocktools::huditems;
using bedrocktools::hotbar::ButtonRect;
using bedrocktools::hotbar::SlotRect;
using bedrocktools::hotbar::StripLayout;
using bedrocktools::hotbar::SurfaceMapping;

constexpr std::size_t SlotCount = HotbarSlotsModule::SlotCount;

// android.view.KeyEvent.KEYCODE_1. The launcher forwards the key code of an
// overlay button straight to the game, which maps 1-9 to the hotbar slots.
constexpr int AndroidKeyCode1 = 8;

constexpr const char* HudElementId = "bedrocktools.hotbarslots.strip";

// The first nine slots of the player inventory are the hotbar.
std::array<void*, SlotCount> getHotbarStacks(void* player) {
    std::array<void*, SlotCount> stacks{};
    const huditems::ContainerSlots inventory = huditems::playerInventory(player);
    for (std::size_t i = 0; i < SlotCount; ++i) stacks[i] = inventory.stack(i);
    return stacks;
}

// The selected slot index is derived by matching the carried item stack
// against the hotbar stacks. That avoids depending on an Inventory field
// offset that changes between Minecraft versions.
int selectedSlotFor(void* player, const std::array<void*, SlotCount>& stacks) {
    void* carried = huditems::getCarriedItem(player);
    if (!carried) return -1;
    for (std::size_t i = 0; i < SlotCount; ++i) {
        if (stacks[i] && stacks[i] == carried) return static_cast<int>(i);
    }
    return -1;
}

void renderListener(void* context, void* client, void* user) {
    auto* module = static_cast<HotbarSlotsModule*>(user);
    if (module && module->enabled) module->renderNative(context, client);
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

} // namespace

HotbarSlotsModule::HotbarSlotsModule()
    : Module("Hotbar Slots",
             "Adds separate 1-9 buttons for selecting hotbar slots, with optional item icons on the HUD.") {
    m_slotEnabled.fill(true);
}

HotbarSlotsModule::~HotbarSlotsModule() {
    unregisterOverlayButtons();
    huditems::removeRenderListener(renderListener, this);
}

std::string HotbarSlotsModule::buttonId(std::size_t index) {
    return "bedrocktoolsplus.HotbarSlots.Button" + std::to_string(index + 1);
}

void HotbarSlotsModule::onInit() {
    huditems::initialize();
    huditems::addRenderListener(renderListener, this);
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
    config.placement = m_iconPlacement == static_cast<int>(IconPlacement::Buttons)
                           ? IconPlacement::Buttons
                           : IconPlacement::Strip;
    config.iconScale = m_iconScale;
    config.slotNumbers = m_slotNumbers;
    config.highlightSelected = m_highlightSelected;
    config.layout.x = hudPosX;
    config.layout.y = hudPosY;
    config.layout.slotSize = m_slotSize;
    config.layout.gap = m_slotGap;
    config.layout.vertical = m_vertical;
    config.buttonScale = m_buttonScale;
    config.numberTextSize = m_numberTextSize;
    config.numberColor = huditems::parseColor(m_numberColor, 0xFFFFFFFFu);
    config.highlightColor = huditems::withOpacity(huditems::parseColor(m_highlightColor, 0xFFFFFFFFu), m_highlightOpacity);
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
    std::lock_guard lock(m_geometryMutex);
    m_buttonRects.fill(ButtonRect{});
    m_surface = SurfaceMapping{};
}

// Asks the launcher where each slot button currently is. Only needed while
// the icons are painted on the buttons; the query touches JNI, so it runs
// once per frame from onFrame() and never from the render thread.
void HotbarSlotsModule::refreshButtonGeometry(const ConfigSnapshot& config) {
    if (config.placement != IconPlacement::Buttons || !config.itemIcons || !config.buttons) {
        std::lock_guard lock(m_geometryMutex);
        m_buttonRects.fill(ButtonRect{});
        m_surface = SurfaceMapping{};
        return;
    }

    std::array<ButtonRect, SlotCount> rects{};
    SurfaceMapping surface{};
    const pl::modmenu::HudSurfaceSize hud = pl::modmenu::getHudSurfaceSize();
    surface.hudWidth = hud.width;
    surface.hudHeight = hud.height;

    for (std::size_t i = 0; i < SlotCount; ++i) {
        if (!config.slots[i]) continue;
        bedrocktools::launcher::ExternalButtonGeometry geometry;
        if (!bedrocktools::launcher::queryExternalButtonGeometry(buttonId(i), geometry)) continue;
        rects[i].x = geometry.x;
        rects[i].y = geometry.y;
        rects[i].width = geometry.width;
        rects[i].height = geometry.height;
        rects[i].visible = geometry.visible;
        surface.screenWidth = geometry.screenWidth;
        surface.screenHeight = geometry.screenHeight;
    }

    std::lock_guard lock(m_geometryMutex);
    m_buttonRects = rects;
    m_surface = surface;
}

void HotbarSlotsModule::renderNative(void* context, void* client) {
    const ConfigSnapshot config = snapshotConfig();

    huditems::IconPainter painter(context, client, config.itemIcons);
    void* localPlayer = painter.player();
    if (!localPlayer) {
        clearRuntime();
        return;
    }

    const auto stacks = getHotbarStacks(localPlayer);
    m_selectedSlot.store(selectedSlotFor(localPlayer, stacks), std::memory_order_release);

    std::array<ButtonRect, SlotCount> buttonRects{};
    SurfaceMapping surface{};
    if (config.placement == IconPlacement::Buttons) {
        std::lock_guard lock(m_geometryMutex);
        buttonRects = m_buttonRects;
        surface = m_surface;
    }

    for (std::size_t i = 0; i < SlotCount; ++i) {
        void* stack = stacks[i];
        void* item = huditems::stackItem(stack);
        m_hasItem[i].store(item != nullptr, std::memory_order_release);

        if (!config.slots[i] || !item || !painter.ready()) continue;

        SlotRect rect;
        if (config.placement == IconPlacement::Buttons) {
            // Paint the icon inside the slot button's inner window, the same
            // way the launcher's own "Use item icons from hotbar" option does.
            if (!buttonRects[i].visible) continue;
            const float inset =
                std::clamp((bedrocktools::hotbar::IconWindowEnd - bedrocktools::hotbar::IconWindowStart) *
                               config.iconScale,
                           0.05f, 1.0f);
            rect = bedrocktools::hotbar::buttonIconRect(buttonRects[i], surface, inset);
        } else {
            rect = bedrocktools::hotbar::slotRect(config.layout, i);
        }
        if (rect.size <= 0.0f) continue;
        painter.draw(stack, item, rect.x, rect.y, rect.size);
    }
}

void HotbarSlotsModule::onFrame() {
    if (!enabled) return;

    const ConfigSnapshot config = snapshotConfig();
    refreshButtonGeometry(config);
    const bool onButtons = config.placement == IconPlacement::Buttons;

    std::array<ButtonRect, SlotCount> buttonRects{};
    SurfaceMapping surface{};
    if (onButtons) {
        std::lock_guard lock(m_geometryMutex);
        buttonRects = m_buttonRects;
        surface = m_surface;
    }

    std::size_t visible = 0;
    for (bool slot : config.slots) visible += slot ? 1 : 0;

    // With the icons living on the buttons there is no separate strip to drag,
    // so the HUD editor element is withdrawn (the buttons keep their own
    // launcher-managed positions).
    std::vector<pl::modmenu::HudEditorElement> elements;
    if (visible > 0 && !onButtons) {
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
        SlotRect rect;
        if (onButtons) {
            // Highlight/number follow the button, so they stay on the slot the
            // player actually taps instead of on a now-hidden strip.
            if (!buttonRects[i].visible) continue;
            rect = bedrocktools::hotbar::buttonIconRect(buttonRects[i], surface, 1.0f);
            if (rect.size <= 0.0f) continue;
        } else {
            rect = bedrocktools::hotbar::slotRect(config.layout, i);
        }

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
    features.description = "On-screen buttons select the slot; the HUD strip shows what each slot holds.";
    features.choiceStyle = ConfigChoiceStyleV2::Checklist;
    features.options = {
        {"buttons", "On-Screen Buttons", {}, "m_buttons"},
        {"icons", "Item Icons", {}, "m_itemIcons"},
        {"numbers", "Slot Numbers", {}, "m_slotNumbers"},
        {"highlight", "Highlight Selected Slot", {}, "m_highlightSelected"}
    };
    schema.node(std::move(features));

    section("icon_placement", "Item Icons", "slots");
    auto placement = node("m_iconPlacement", "Icon Placement", "slots", ConfigControlTypeV2::Choice);
    placement.section = "icon_placement";
    placement.choiceStyle = ConfigChoiceStyleV2::Segmented;
    placement.description =
        "Draw the item of each hotbar slot on the on-screen slot button (like the launcher's "
        "\"Use item icons from hotbar\"), or on a separate HUD strip you can move in the HUD Editor.";
    placement.defaultValue = "1";
    placement.options = {
        {"0", "HUD Strip", "A separate row/column placed with the HUD Editor.", {}},
        {"1", "On Slot Buttons", "Paint the icon inside the on-screen slot button.", {}}
    };
    schema.node(std::move(placement));
    slider("m_iconScale", "Icon Size On Button", "slots", "icon_placement", "0.2", "1.5", "x");

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
        if (j.contains("m_iconPlacement")) {
            // Choice values arrive as "index,Label,Label" strings.
            try {
                if (j["m_iconPlacement"].is_string()) {
                    std::string value = j["m_iconPlacement"].get<std::string>();
                    const std::size_t separator = value.find(',');
                    if (separator != std::string::npos) value.resize(separator);
                    m_iconPlacement = std::clamp(std::stoi(value), 0, 1);
                } else {
                    m_iconPlacement = std::clamp(j["m_iconPlacement"].get<int>(), 0, 1);
                }
            } catch (...) {
            }
        }
        if (j.contains("m_iconScale")) m_iconScale = std::clamp(j["m_iconScale"].get<float>(), 0.2f, 1.5f);
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
    j["m_iconPlacement"] = std::to_string(m_iconPlacement) + ",HUD Strip,On Slot Buttons";
    j["m_iconScale"] = m_iconScale;
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

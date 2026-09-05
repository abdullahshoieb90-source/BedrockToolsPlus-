#include "hotbarslots.hpp"

#include "huditems.hpp"
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
using bedrocktools::hotbar::SlotRect;
using bedrocktools::hotbar::StripLayout;

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

// Durability bar helpers (same proportions as Inventory HUD / vanilla)
inline float durabilityRatio(int damage, int maxDamage) {
    if (maxDamage <= 0) return 1.0f;
    const int safeDamage = std::clamp(damage, 0, maxDamage);
    return static_cast<float>(maxDamage - safeDamage) / static_cast<float>(maxDamage);
}

inline std::uint32_t durabilityColor(float ratio) {
    const float r = std::clamp(ratio, 0.0f, 1.0f);
    const auto red = static_cast<std::uint32_t>((1.0f - r) * 255.0f + 0.5f);
    const auto green = static_cast<std::uint32_t>(r * 255.0f + 0.5f);
    return 0xFF000000u | (red << 16) | (green << 8);
}

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
    config.slotNumbers = m_slotNumbers;
    config.highlightSelected = m_highlightSelected;
    config.showCount = m_showCount;
    config.showDurability = m_showDurability;
    config.showSlotBackground = m_showSlotBackground;
    config.layout.x = hudPosX;
    config.layout.y = hudPosY;
    config.layout.slotSize = m_slotSize;
    config.layout.gap = m_slotGap;
    config.layout.vertical = m_vertical;
    config.buttonScale = m_buttonScale;
    config.numberTextSize = m_numberTextSize;
    config.numberColor = huditems::parseColor(m_numberColor, 0xFFFFFFFFu);
    config.highlightColor = huditems::withOpacity(huditems::parseColor(m_highlightColor, 0xFFFFFFFFu), m_highlightOpacity);
    config.countTextSize = m_countTextSize;
    config.countColor = huditems::parseColor(m_countColor, 0xFFFFFFFFu);
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
    for (auto& slot : m_stackCount) slot.store(0, std::memory_order_release);
    for (auto& slot : m_damage) slot.store(0, std::memory_order_release);
    for (auto& slot : m_maxDamage) slot.store(0, std::memory_order_release);
    m_selectedSlot.store(-1, std::memory_order_release);
}

void HotbarSlotsModule::storeRuntime(std::size_t index, void* stack, void* item) {
    if (index >= SlotCount) return;
    const bool has = item != nullptr;
    m_hasItem[index].store(has, std::memory_order_release);
    if (!has) {
        m_stackCount[index].store(0, std::memory_order_release);
        m_damage[index].store(0, std::memory_order_release);
        m_maxDamage[index].store(0, std::memory_order_release);
        return;
    }
    m_stackCount[index].store(static_cast<int>(huditems::stackCount(stack)), std::memory_order_release);
    const int maxDmg = huditems::itemMaxDamage(item);
    m_maxDamage[index].store(maxDmg, std::memory_order_release);
    if (maxDmg > 0) {
        m_damage[index].store(huditems::stackDamage(stack), std::memory_order_release);
    } else {
        m_damage[index].store(0, std::memory_order_release);
    }
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

    // Always publish runtime so onFrame can show count/durability even if icons are disabled
    for (std::size_t i = 0; i < SlotCount; ++i) {
        void* stack = stacks[i];
        void* item = huditems::stackItem(stack);
        storeRuntime(i, stack, item);
    }

    if (!config.itemIcons || !painter.ready()) return;

    // First pass: dyed leather and other tinted items need HUD opacity fix
    if (painter.supportsOpacityFix()) {
        painter.beginOpacityFixPass();
        for (std::size_t i = 0; i < SlotCount; ++i) {
            if (!config.slots[i]) continue;
            void* stack = stacks[i];
            if (!stack || !huditems::needsTextureOpacityPass(stack)) continue;
            void* item = huditems::stackItem(stack);
            if (!item) continue;
            const SlotRect rect = bedrocktools::hotbar::slotRect(config.layout, i);
            if (rect.size <= 0.0f) continue;
            const float unit = rect.size / 16.0f;
            const float pad = unit; // 1px at vanilla 16px scale keeps slot border visible
            const float iconSize = std::max(1.0f, rect.size - 2.0f * pad);
            painter.drawOpacityFix(stack, item, rect.x + pad, rect.y + pad, iconSize);
        }
        painter.endOpacityFixPass();
    }

    // Main item icons - inset inside the slot frame so the border stays visible
    for (std::size_t i = 0; i < SlotCount; ++i) {
        if (!config.slots[i]) continue;
        void* stack = stacks[i];
        void* item = huditems::stackItem(stack);
        if (!item) continue;
        const SlotRect rect = bedrocktools::hotbar::slotRect(config.layout, i);
        if (rect.size <= 0.0f) continue;
        const float unit = rect.size / 16.0f;
        const float pad = unit;
        const float iconSize = std::max(1.0f, rect.size - 2.0f * pad);
        painter.draw(stack, item, rect.x + pad, rect.y + pad, iconSize);
    }
}

void HotbarSlotsModule::onFrame() {
    if (!enabled) return;

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
    commands.reserve(visible * 5);
    const int selected = m_selectedSlot.load(std::memory_order_acquire);

    for (std::size_t i = 0; i < SlotCount; ++i) {
        if (!config.slots[i]) continue;
        const SlotRect rect = bedrocktools::hotbar::slotRect(config.layout, i);
        if (rect.size <= 0.0f) continue;

        // Slot background / border - drawn as an outline so the interior stays
        // transparent and the native item icon shows through. Two thin rects
        // mimic the Minecraft bevel (dark outer, light inner) without obscuring
        // the icon center.
        if (config.showSlotBackground) {
            // Outer dark border
            {
                pl::modmenu::DrawCommand border;
                border.type = pl::modmenu::DrawCommandType::Rect;
                border.x = rect.x;
                border.y = rect.y;
                border.w = rect.size;
                border.h = rect.size;
                border.color = 0xFF373737u;
                border.size = 1.2f;
                commands.push_back(std::move(border));
            }
            // Inner light bevel (slightly inset) - also outline only
            {
                pl::modmenu::DrawCommand bevel;
                bevel.type = pl::modmenu::DrawCommandType::Rect;
                bevel.x = rect.x + 1.0f;
                bevel.y = rect.y + 1.0f;
                bevel.w = std::max(1.0f, rect.size - 2.0f);
                bevel.h = std::max(1.0f, rect.size - 2.0f);
                bevel.color = 0xFF8B8B8Bu;
                bevel.size = 0.8f;
                commands.push_back(std::move(bevel));
            }
            // Subtle slot fill - very translucent so icon remains dominant but
            // empty slots are still visible as a faint square
            const bool hasItem = m_hasItem[i].load(std::memory_order_acquire);
            if (!hasItem) {
                pl::modmenu::DrawCommand fill;
                fill.type = pl::modmenu::DrawCommandType::RectFilled;
                fill.x = rect.x + 1.0f;
                fill.y = rect.y + 1.0f;
                fill.w = std::max(1.0f, rect.size - 2.0f);
                fill.h = std::max(1.0f, rect.size - 2.0f);
                fill.color = 0x33000000u;
                commands.push_back(std::move(fill));
            }
        }

        // Highlight for selected slot - semi-transparent fill on top of the icon
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

        const bool hasItem = m_hasItem[i].load(std::memory_order_acquire);
        if (hasItem) {
            // Durability bar - drawn over the icon like vanilla hotbar
            if (config.showDurability) {
                const int dmg = m_damage[i].load(std::memory_order_acquire);
                const int maxDmg = m_maxDamage[i].load(std::memory_order_acquire);
                if (maxDmg > 0 && dmg >= 0) {
                    const float ratio = durabilityRatio(dmg, maxDmg);
                    if (ratio < 1.0f) {
                        const float unit = rect.size / 16.0f;
                        const float barW = 13.0f * unit;
                        const float barH = std::max(1.0f, 2.0f * unit);
                        const float fillH = std::max(1.0f, unit);
                        const float barX = rect.x + 2.0f * unit;
                        const float barY = rect.y + 13.0f * unit;
                        // background
                        {
                            pl::modmenu::DrawCommand bg;
                            bg.type = pl::modmenu::DrawCommandType::RectFilled;
                            bg.x = barX;
                            bg.y = barY;
                            bg.w = barW;
                            bg.h = barH;
                            bg.color = 0xFF000000u;
                            commands.push_back(std::move(bg));
                        }
                        const float fillW = barW * std::clamp(ratio, 0.0f, 1.0f);
                        if (fillW > 0.5f) {
                            pl::modmenu::DrawCommand fill;
                            fill.type = pl::modmenu::DrawCommandType::RectFilled;
                            fill.x = barX;
                            fill.y = barY;
                            fill.w = fillW;
                            fill.h = fillH;
                            fill.color = durabilityColor(ratio);
                            commands.push_back(std::move(fill));
                        }
                    }
                }
            }

            // Stack count - bottom-right corner of the slot
            if (config.showCount) {
                const int count = m_stackCount[i].load(std::memory_order_acquire);
                if (count > 1) {
                    const float unit = rect.size / 16.0f;
                    pl::modmenu::DrawCommand text;
                    text.type = pl::modmenu::DrawCommandType::Text;
                    // right-aligned at bottom-right, small shadow is handled by launcher
                    text.x = rect.x + rect.size - unit;
                    text.y = rect.y + rect.size - unit;
                    text.w = -1.0f; // right-aligned
                    text.color = config.countColor;
                    text.size = config.countTextSize;
                    text.text = std::to_string(count);
                    commands.push_back(std::move(text));
                }
            }
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
    features.description = "On-screen buttons select the slot; the HUD strip shows what each slot holds directly on the slot.";
    features.choiceStyle = ConfigChoiceStyleV2::Checklist;
    features.options = {
        {"buttons", "On-Screen Buttons", {}, "m_buttons"},
        {"icons", "Item Icons on Slots", {}, "m_itemIcons"},
        {"numbers", "Slot Numbers", {}, "m_slotNumbers"},
        {"highlight", "Highlight Selected Slot", {}, "m_highlightSelected"}
    };
    schema.node(std::move(features));

    section("slot_details", "Slot Details", "slots");
    auto details = node("slot_details_group", "Icon Details", "slots", ConfigControlTypeV2::ToggleGroup);
    details.key.clear();
    details.section = "slot_details";
    details.description = "Stack count and durability are drawn on top of the item icon inside the slot.";
    details.choiceStyle = ConfigChoiceStyleV2::Checklist;
    details.options = {
        {"count", "Stack Count", {}, "m_showCount"},
        {"durability", "Durability Bar", {}, "m_showDurability"},
        {"background", "Slot Frame", {}, "m_showSlotBackground"}
    };
    schema.node(std::move(details));

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
    slider("m_countTextSize", "Stack Count Size", "appearance", "strip_text", "6", "20");
    auto countColor = node("m_countColor", "Stack Count Color", "appearance", ConfigControlTypeV2::Color);
    countColor.section = "strip_text";
    countColor.defaultValue = "#FFFFFF";
    countColor.visibleWhen = {{"m_showCount", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(countColor));

    auto help = node("editor_help", "Move The Strip In The HUD Editor", "editor", ConfigControlTypeV2::Info);
    help.key.clear();
    help.description = "Open the HUD Editor to drag the slot strip. The item icon now renders inset inside each slot frame. On-screen buttons keep their own positions.";
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
        if (j.contains("m_slotNumbers")) m_slotNumbers = j["m_slotNumbers"].get<bool>();
        if (j.contains("m_highlightSelected")) m_highlightSelected = j["m_highlightSelected"].get<bool>();
        if (j.contains("m_showCount")) m_showCount = j["m_showCount"].get<bool>();
        if (j.contains("m_showDurability")) m_showDurability = j["m_showDurability"].get<bool>();
        if (j.contains("m_showSlotBackground")) m_showSlotBackground = j["m_showSlotBackground"].get<bool>();
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
        if (j.contains("m_countTextSize"))
            m_countTextSize = std::clamp(j["m_countTextSize"].get<float>(), 6.0f, 20.0f);
        if (j.contains("m_countColor")) m_countColor = j["m_countColor"].get<std::string>();
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
    j["m_slotNumbers"] = m_slotNumbers;
    j["m_highlightSelected"] = m_highlightSelected;
    j["m_showCount"] = m_showCount;
    j["m_showDurability"] = m_showDurability;
    j["m_showSlotBackground"] = m_showSlotBackground;
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
    j["m_countTextSize"] = m_countTextSize;
    j["m_countColor"] = m_countColor;
    j["m_gridSize"] = m_gridSize;
    j["m_gridGap"] = m_gridGap;
    j["m_snapThreshold"] = m_snapThreshold;
    j["m_snapToGrid"] = m_snapToGrid;
    j["m_snapToElements"] = m_snapToElements;
    j["m_snapToScreenCenter"] = m_snapToScreenCenter;
}

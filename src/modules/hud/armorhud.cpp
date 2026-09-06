#include "armorhud.hpp"

#include "huditems.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <pl/ModMenu.hpp>
#include <pl/ModMenuConfig.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace huditems = bedrocktools::huditems;
namespace layout = bedrocktools::armorhud;
namespace decor = bedrocktools::slotdecor;
using layout::ArmorLayout;
using decor::SlotRect;

constexpr std::size_t SlotCount = ArmorModule::SlotCount;
constexpr const char* ArmorElementId = "bedrocktools.armorhud.column";

struct EquipmentStacks {
    std::array<void*, SlotCount> stacks{};
};

// Helmet, chestplate, leggings, boots, offhand.
EquipmentStacks getEquipmentColumn(void* player) {
    EquipmentStacks column;
    const huditems::EquipmentStacks equipment = huditems::getEquipmentStacks(player);
    for (std::size_t i = 0; i < layout::ArmorSlotCount; ++i) column.stacks[i] = equipment.armor[i];
    column.stacks[layout::OffhandIndex] = equipment.offhand;
    return column;
}

void renderListener(void* context, void* client, void* user) {
    auto* module = static_cast<ArmorModule*>(user);
    if (module && module->enabled) module->renderNative(context, client);
}

} // namespace

ArmorModule::ArmorModule()
    : Module("Armor",
             "Shows your armor pieces and offhand item on the HUD, with durability bars and numbers.") {
}

ArmorModule::~ArmorModule() {
    huditems::removeRenderListener(renderListener, this);
}

void ArmorModule::onInit() {
    huditems::initialize();
    huditems::addRenderListener(renderListener, this);

    // The inventory (and any other container UI) draws the same items at full
    // size, so the HUD copy is hidden while one is open. The counter tracks
    // nested opens the same way the runtime's keybind blocker does.
    bedrocktools::events::bus().subscribe<bedrocktools::events::ScreenStateEvent>([this](auto& event) {
        if (event.screen != bedrocktools::events::ScreenKind::Container) return;
        if (event.phase == bedrocktools::events::ScreenPhase::Opened) {
            m_containerDepth.fetch_add(1, std::memory_order_acq_rel);
        } else {
            int depth = m_containerDepth.load(std::memory_order_acquire);
            while (depth > 0 &&
                   !m_containerDepth.compare_exchange_weak(depth, depth - 1, std::memory_order_acq_rel)) {
            }
        }
    });
}

void ArmorModule::onDisable() {
    clearRuntime();
    pl::modmenu::submitDrawCommands(moduleId, std::span<const pl::modmenu::DrawCommand>{});
    pl::modmenu::submitHudEditorElements(moduleId, std::span<const pl::modmenu::HudEditorElement>{});
}

bool ArmorModule::hiddenByScreen() const {
    return m_containerDepth.load(std::memory_order_acquire) > 0;
}

bedrocktools::armorhud::ArmorLayout ArmorModule::armorLayout() const {
    ArmorLayout value;
    value.x = hudPosX;
    value.y = hudPosY;
    value.slotSize = m_slotSize;
    value.gap = m_slotGap;
    value.armorTextSize = m_showArmorDurability ? m_countTextSize : 0.0f;
    value.horizontal = m_horizontal;
    return value;
}

ArmorModule::ConfigSnapshot ArmorModule::snapshotConfig() const {
    std::lock_guard lock(m_configMutex);
    ConfigSnapshot config;
    config.layout = armorLayout();
    config.showOffhand = m_showOffhand;
    config.stackCount = m_showStackCount;
    config.durability = m_showDurability;
    config.armorDurability = m_showArmorDurability;
    config.hideInContainer = m_hideInContainer;
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

void ArmorModule::clearRuntime() {
    for (auto& slot : m_slots) storeRuntime(slot, nullptr, nullptr, false);
}

// Publishes what the render thread saw for one slot to onFrame(); a null
// `item` clears the slot.
void ArmorModule::storeRuntime(SlotRuntime& runtime, void* stack, void* item, bool wantDurability) {
    const bool hasItem = item != nullptr;
    runtime.hasItem.store(hasItem, std::memory_order_release);
    runtime.count.store(hasItem ? huditems::stackCount(stack) : 0, std::memory_order_release);
    int damage = 0;
    int maxDamage = 0;
    if (hasItem && wantDurability) {
        maxDamage = huditems::itemMaxDamage(item);
        if (maxDamage > 0) damage = huditems::stackDamage(stack);
    }
    runtime.damage.store(damage, std::memory_order_release);
    runtime.maxDamage.store(maxDamage, std::memory_order_release);
}

void ArmorModule::renderNative(void* context, void* client) {
    const ConfigSnapshot config = snapshotConfig();
    const bool hidden = config.hideInContainer && hiddenByScreen();

    // Even while hidden the player is still needed to keep the overlay
    // bookkeeping (counts / durability) current, so only the render setup
    // is skipped.
    huditems::IconPainter painter(context, client, !hidden);
    void* localPlayer = painter.player();
    if (!localPlayer) {
        clearRuntime();
        return;
    }

    EquipmentStacks equipment = getEquipmentColumn(localPlayer);
    if (!config.showOffhand) equipment.stacks[layout::OffhandIndex] = nullptr;

    std::array<void*, SlotCount> items{};
    for (std::size_t i = 0; i < SlotCount; ++i) {
        items[i] = huditems::stackItem(equipment.stacks[i]);
        const bool wantDurability =
            config.durability || (config.armorDurability && i < layout::OffhandIndex);
        storeRuntime(m_slots[i], equipment.stacks[i], items[i], wantDurability);
    }

    if (!painter.ready()) return;

    // Dyed leather armor (and a few other tinted items) need the HUD opacity
    // fix pass first, otherwise their tinted pixels come out transparent.
    if (painter.supportsOpacityFix()) {
        painter.beginOpacityFixPass();
        for (std::size_t i = 0; i < SlotCount; ++i) {
            if (!items[i] || !huditems::needsTextureOpacityPass(equipment.stacks[i])) continue;
            const SlotRect rect = layout::slotRect(config.layout, i);
            painter.drawOpacityFix(equipment.stacks[i], items[i], rect.x, rect.y, rect.size);
        }
        painter.endOpacityFixPass();
    }

    // Only occupied slots are submitted to the ItemRenderer; empty ones cost
    // nothing.
    for (std::size_t i = 0; i < SlotCount; ++i) {
        if (!items[i]) continue;
        const SlotRect rect = layout::slotRect(config.layout, i);
        painter.draw(equipment.stacks[i], items[i], rect.x, rect.y, rect.size);
    }
}

void ArmorModule::onFrame() {
    if (!enabled) return;

    const ConfigSnapshot config = snapshotConfig();

    // The editor box always covers the full column so the element can be
    // placed even while every slot is empty.
    std::vector<pl::modmenu::HudEditorElement> elements;
    {
        pl::modmenu::HudEditorElement element;
        element.elementId = ArmorElementId;
        element.displayName = "Armor & Offhand";
        element.positionKeyX = "hudPosX";
        element.positionKeyY = "hudPosY";
        element.x = config.layout.x;
        element.y = config.layout.y;
        element.width = std::max(1.0f, layout::columnWidth(config.layout));
        element.height = std::max(1.0f, layout::columnHeight(config.layout));
        element.gridSize = config.gridSize;
        element.snapThreshold = config.snapThreshold;
        element.gridGap = config.gridGap;
        element.snapFlags = config.snapFlags;
        elements.push_back(std::move(element));
    }
    pl::modmenu::submitHudEditorElements(moduleId, elements);

    std::vector<pl::modmenu::DrawCommand> commands;
    const bool hidden = config.hideInContainer && hiddenByScreen();
    if (!hidden && (config.stackCount || config.durability || config.armorDurability)) {
        auto decorate = [&](const SlotRuntime& runtime, const SlotRect& rect, bool armorSlot) {
            if (!runtime.hasItem.load(std::memory_order_acquire) || rect.size <= 0.0f) return;

            const int maxDamage = runtime.maxDamage.load(std::memory_order_acquire);
            const int damage = runtime.damage.load(std::memory_order_acquire);
            if (config.durability && maxDamage > 0 && damage > 0) {
                const float ratio = decor::durabilityRatio(damage, maxDamage);
                const decor::DurabilityBar bar = decor::durabilityBar(rect, ratio);

                pl::modmenu::DrawCommand background;
                background.type = pl::modmenu::DrawCommandType::RectFilled;
                background.x = bar.x;
                background.y = bar.y;
                background.w = bar.width;
                background.h = bar.height;
                background.color = 0xFF000000u;
                commands.push_back(std::move(background));

                if (bar.fillWidth > 0.0f) {
                    pl::modmenu::DrawCommand fill;
                    fill.type = pl::modmenu::DrawCommandType::RectFilled;
                    fill.x = bar.x;
                    fill.y = bar.y;
                    fill.w = bar.fillWidth;
                    fill.h = bar.fillHeight;
                    fill.color = decor::durabilityColor(ratio);
                    commands.push_back(std::move(fill));
                }
            }

            // Armor numbers are independent of stack counts and durability
            // bars, and remain visible for undamaged armor as well.
            if (armorSlot && config.armorDurability && maxDamage > 0) {
                pl::modmenu::DrawCommand text;
                text.type = pl::modmenu::DrawCommandType::Text;
                text.x = rect.x + rect.size + layout::armorLabelGap(config.layout);
                text.y = rect.y;
                text.w = layout::armorLabelWidth(config.layout);
                text.h = rect.size; // center the label beside this armor icon
                text.color = config.countColor;
                text.size = layout::armorLabelTextSize(config.layout);
                text.text = decor::durabilityText(damage, maxDamage);
                commands.push_back(std::move(text));
            }

            const std::uint8_t count = runtime.count.load(std::memory_order_acquire);
            if (config.stackCount && count > 1) {
                const decor::TextAnchor anchor = decor::countTextAnchor(rect);
                pl::modmenu::DrawCommand text;
                text.type = pl::modmenu::DrawCommandType::Text;
                text.x = anchor.x;
                text.y = anchor.y;
                text.w = -1.0f; // right-aligned at x
                text.color = config.countColor;
                text.size = config.countTextSize;
                text.text = std::to_string(static_cast<unsigned>(count));
                commands.push_back(std::move(text));
            }
        };

        for (std::size_t i = 0; i < SlotCount; ++i) {
            decorate(m_slots[i], layout::slotRect(config.layout, i), i < layout::OffhandIndex);
        }
    }
    pl::modmenu::submitDrawCommands(moduleId, commands);
}

void ArmorModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("column")
        .category("column", "Column", "Shape and size of the armor element")
        .category("details", "Details", "Extra information drawn on each slot")
        .category("visibility", "Visibility", "When the armor element is shown")
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
                      const char* unit = " px", const char* enabledKey = nullptr) {
        auto value = node(key, std::move(title), category, ConfigControlTypeV2::SliderFloat);
        value.section = sectionId;
        value.minValue = min;
        value.maxValue = max;
        value.step = "1";
        value.unit = unit;
        if (enabledKey) value.visibleWhen = {{enabledKey, ConfigConditionOpV2::Truthy, {}}};
        schema.node(std::move(value));
    };

    section("column_shape", "Layout", "column");
    {
        auto horizontal = node("m_horizontal", "Horizontal Layout", "column", ConfigControlTypeV2::Toggle);
        horizontal.section = "column_shape";
        horizontal.description = "Lays the armor and offhand out in a row instead of a column.";
        schema.node(std::move(horizontal));
    }
    slider("m_slotSize", "Slot Size", "column", "column_shape", "8", "100");
    slider("m_slotGap", "Gap Between Slots", "column", "column_shape", "0", "50");
    section("activation", "Shortcut", "column");
    {
        auto toggleKey = node("keybind", "Toggle Keybind", "column", ConfigControlTypeV2::Keybind);
        toggleKey.section = "activation";
        schema.node(std::move(toggleKey));
    }

    section("slot_details", "Slot Details", "details");
    {
        auto features = node("slot_features", "Show", "details", ConfigControlTypeV2::ToggleGroup);
        features.key.clear();
        features.section = "slot_details";
        features.description = "This module is independent of Inventory HUD: enable only what you want to see.";
        features.choiceStyle = ConfigChoiceStyleV2::Checklist;
        features.options = {
            {"offhand", "Offhand Slot", {}, "m_showOffhand"},
            {"count", "Stack Count", {}, "m_showStackCount"},
            {"durability", "Durability Bar", {}, "m_showDurability"}
        };
        schema.node(std::move(features));
    }
    {
        auto armorNumbers = node("m_showArmorDurability", "Armor Durability Numbers", "details", ConfigControlTypeV2::Toggle);
        armorNumbers.section = "slot_details";
        armorNumbers.description = "Shows remaining/maximum durability beside each armor piece, independently of durability bars.";
        schema.node(std::move(armorNumbers));
    }
    section("count_text", "Number Text", "details");
    slider("m_countTextSize", "Text Size", "details", "count_text", "6", "40");
    {
        auto color = node("m_countColor", "Text Color", "details", ConfigControlTypeV2::Color);
        color.section = "count_text";
        color.defaultValue = "#FFFFFF";
        color.description = "Used for stack counts and armor durability numbers.";
        schema.node(std::move(color));
    }

    section("auto_hide", "Automatic Hiding", "visibility");
    {
        auto hide = node("m_hideInContainer", "Hide While Inventory Is Open", "visibility", ConfigControlTypeV2::Toggle);
        hide.section = "auto_hide";
        hide.description = "Hides the armor element while the inventory, a chest or any other container screen is open.";
        schema.node(std::move(hide));
    }

    auto help = node("editor_help", "An Element Of Its Own", "editor", ConfigControlTypeV2::Info);
    help.key.clear();
    help.description = "Armor & Offhand is its own module now: the HUD Editor shows one element for it, which you can place anywhere, with or without the Inventory HUD module enabled.";
    schema.node(std::move(help));
    section("snapping", "Snapping", "editor");
    {
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
    }
    slider("m_gridSize", "Grid Size", "editor", "snapping", "1", "100", " px", "m_snapToGrid");
    slider("m_gridGap", "Gap Between Elements", "editor", "snapping", "0", "100", " px", "m_snapToElements");
    slider("m_snapThreshold", "Snap Distance", "editor", "snapping", "1", "100");

    pl::modmenu::setConfigSchemaJson(moduleId, schema.toJson());
}

nlohmann::json ArmorModule::migratedFromInventoryHud(const nlohmann::json& inventoryHud) {
    nlohmann::json migrated;

    auto readBool = [&](const char* key, bool fallback) {
        return inventoryHud.contains(key) ? inventoryHud[key].get<bool>() : fallback;
    };
    auto readFloat = [&](const char* key, float fallback) {
        return inventoryHud.contains(key) ? inventoryHud[key].get<float>() : fallback;
    };

    // The column was only drawn when the Inventory HUD option was on; keep the
    // module off otherwise so nothing appears out of nowhere.
    const bool showEquipment = readBool("m_showEquipment", false);
    migrated["masterEnabled"] = showEquipment && readBool("masterEnabled", false);
    migrated["keybindActive"] = readBool("keybindActive", true);

    // Position: the column's own anchor when it had one, otherwise the shared
    // legacy anchor of the grid.
    float x = readFloat("hudEquipmentPosX", -1.0f);
    float y = readFloat("hudEquipmentPosY", -1.0f);
    if (x < 0.0f || y < 0.0f) {
        x = readFloat("hudPosX", 24.0f);
        y = readFloat("hudPosY", 200.0f);
    }
    migrated["hudPosX"] = x;
    migrated["hudPosY"] = y;

    for (const char* key : {"m_slotSize", "m_slotGap", "m_countTextSize", "m_gridSize", "m_gridGap",
                            "m_snapThreshold", "m_countColor", "m_showStackCount", "m_showDurability",
                            "m_showArmorDurability", "m_hideInContainer", "m_snapToGrid",
                            "m_snapToElements", "m_snapToScreenCenter"}) {
        if (inventoryHud.contains(key)) migrated[key] = inventoryHud[key];
    }
    return migrated;
}

void ArmorModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard lock(m_configMutex);

    if (j.contains("hudPosX")) hudPosX = std::clamp(j["hudPosX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("hudPosY")) hudPosY = std::clamp(j["hudPosY"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("m_slotSize")) m_slotSize = std::clamp(j["m_slotSize"].get<float>(), 8.0f, 100.0f);
    if (j.contains("m_slotGap")) m_slotGap = std::clamp(j["m_slotGap"].get<float>(), 0.0f, 50.0f);
    if (j.contains("m_horizontal")) m_horizontal = j["m_horizontal"].get<bool>();
    if (j.contains("m_showOffhand")) m_showOffhand = j["m_showOffhand"].get<bool>();
    if (j.contains("m_showStackCount")) m_showStackCount = j["m_showStackCount"].get<bool>();
    if (j.contains("m_showDurability")) m_showDurability = j["m_showDurability"].get<bool>();
    if (j.contains("m_showArmorDurability")) m_showArmorDurability = j["m_showArmorDurability"].get<bool>();
    if (j.contains("m_hideInContainer")) m_hideInContainer = j["m_hideInContainer"].get<bool>();
    if (j.contains("m_countTextSize")) m_countTextSize = std::clamp(j["m_countTextSize"].get<float>(), 6.0f, 40.0f);
    if (j.contains("m_countColor")) m_countColor = j["m_countColor"].get<std::string>();
    if (j.contains("m_gridSize")) m_gridSize = std::clamp(j["m_gridSize"].get<float>(), 1.0f, 100.0f);
    if (j.contains("m_gridGap")) m_gridGap = std::clamp(j["m_gridGap"].get<float>(), 0.0f, 100.0f);
    if (j.contains("m_snapThreshold")) m_snapThreshold = std::clamp(j["m_snapThreshold"].get<float>(), 1.0f, 100.0f);
    if (j.contains("m_snapToGrid")) m_snapToGrid = j["m_snapToGrid"].get<bool>();
    if (j.contains("m_snapToElements")) m_snapToElements = j["m_snapToElements"].get<bool>();
    if (j.contains("m_snapToScreenCenter")) m_snapToScreenCenter = j["m_snapToScreenCenter"].get<bool>();
}

void ArmorModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard lock(m_configMutex);

    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["m_slotSize"] = m_slotSize;
    j["m_slotGap"] = m_slotGap;
    j["m_horizontal"] = m_horizontal;
    j["m_showOffhand"] = m_showOffhand;
    j["m_showStackCount"] = m_showStackCount;
    j["m_showDurability"] = m_showDurability;
    j["m_showArmorDurability"] = m_showArmorDurability;
    j["m_hideInContainer"] = m_hideInContainer;
    j["m_countTextSize"] = m_countTextSize;
    j["m_countColor"] = m_countColor;
    j["m_gridSize"] = m_gridSize;
    j["m_gridGap"] = m_gridGap;
    j["m_snapThreshold"] = m_snapThreshold;
    j["m_snapToGrid"] = m_snapToGrid;
    j["m_snapToElements"] = m_snapToElements;
    j["m_snapToScreenCenter"] = m_snapToScreenCenter;
}

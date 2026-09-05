#include "armorhud.hpp"

#include "huditems.hpp"
#include "modules/ModuleRegistry.hpp"

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

constexpr std::size_t SlotCount = 6;

constexpr std::array<const char*, SlotCount> HudElementIds{
    "bedrocktools.armorhud.helmet",
    "bedrocktools.armorhud.chestplate",
    "bedrocktools.armorhud.leggings",
    "bedrocktools.armorhud.boots",
    "bedrocktools.armorhud.offhand",
    "bedrocktools.armorhud.mainhand"
};

constexpr std::array<const char*, SlotCount> HudElementNames{
    "Helmet", "Chestplate", "Leggings", "Boots", "Offhand", "Main Hand"
};

constexpr std::array<const char*, SlotCount> HudXKeys{
    "hudHelmetPosX", "hudChestplatePosX", "hudLeggingsPosX", "hudBootsPosX", "hudOffhandPosX", "hudMainhandPosX"
};

constexpr std::array<const char*, SlotCount> HudYKeys{
    "hudHelmetPosY", "hudChestplatePosY", "hudLeggingsPosY", "hudBootsPosY", "hudOffhandPosY", "hudMainhandPosY"
};

std::array<void*, SlotCount> getHudStacks(void* player) {
    const huditems::EquipmentStacks equipment = huditems::getEquipmentStacks(player);
    return {
        equipment.armor[0], equipment.armor[1], equipment.armor[2], equipment.armor[3],
        equipment.offhand, equipment.mainhand
    };
}

void renderListener(void* context, void* client, void* user) {
    auto* module = static_cast<ArmorHudModule*>(user);
    if (module && module->enabled) module->renderNative(context, client);
}

}

ArmorHudModule::ArmorHudModule()
    : Module("ArmorHUD", "Displays armor, offhand, and main-hand items with configurable durability information.") {
}

ArmorHudModule::~ArmorHudModule() {
    huditems::removeRenderListener(renderListener, this);
}

void ArmorHudModule::onInit() {
    huditems::initialize();
    huditems::addRenderListener(renderListener, this);
}

void ArmorHudModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("equipment")
        .category("equipment", "Equipment", "Choose visible items and their sizes")
        .category("durability", "Durability", "Choose which items show durability and what it contains")
        .category("text", "Text", "Position and style of durability labels")
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
                      const char* enabledKey = nullptr) {
        auto value = node(key, std::move(title), category, ConfigControlTypeV2::SliderFloat);
        value.section = sectionId;
        value.minValue = min;
        value.maxValue = max;
        value.step = "1";
        value.unit = " px";
        if (enabledKey) value.visibleWhen = {{enabledKey, ConfigConditionOpV2::Truthy, {}}};
        schema.node(std::move(value));
    };

    struct SlotSetting { const char* key; const char* title; };
    static constexpr SlotSetting slots[] = {
        {"m_helmet", "Helmet"}, {"m_chestplate", "Chestplate"},
        {"m_leggings", "Leggings"}, {"m_boots", "Boots"},
        {"m_offhand", "Offhand"}, {"m_mainhand", "Main Hand"}
    };
    section("visible_items", "Visible Items", "equipment");
    auto equipment = node("equipment_slots", "Show Items", "equipment", ConfigControlTypeV2::ToggleGroup);
    equipment.key.clear();
    equipment.section = "visible_items";
    equipment.choiceStyle = ConfigChoiceStyleV2::Chips;
    for (const auto& slot : slots) equipment.options.push_back({slot.key, slot.title, {}, slot.key});
    schema.node(std::move(equipment));

    section("item_sizes", "Item Sizes", "equipment");
    for (const auto& slot : slots) {
        const std::string key = std::string(slot.key) + "Size";
        slider(key.c_str(), std::string(slot.title) + " Size", "equipment", "item_sizes", "8", "100", slot.key);
    }
    section("activation", "Shortcut", "equipment");
    auto keybind = node("keybind", "Toggle Keybind", "equipment", ConfigControlTypeV2::Keybind);
    keybind.section = "activation";
    schema.node(std::move(keybind));

    section("durability_items", "Item Durability", "durability");
    auto durability = node("durability_slots", "Show Durability For", "durability", ConfigControlTypeV2::ToggleGroup);
    durability.key.clear();
    durability.section = "durability_items";
    durability.description = "Labels appear for enabled items that have durability.";
    durability.choiceStyle = ConfigChoiceStyleV2::Chips;
    for (const auto& slot : slots) {
        const std::string key = std::string(slot.key) + "Durability";
        durability.options.push_back({key, slot.title, {}, key});
    }
    schema.node(std::move(durability));

    section("durability_contents", "Label Contents", "durability");
    auto contents = node("durability_fields", "Display Values", "durability", ConfigControlTypeV2::ToggleGroup);
    contents.key.clear();
    contents.section = "durability_contents";
    contents.description = "Remaining and maximum combine as remaining / maximum. Percentage shows durability remaining.";
    contents.choiceStyle = ConfigChoiceStyleV2::Checklist;
    contents.options = {
        {"damage", "Damage Taken", {}, "m_showDamage"},
        {"remaining", "Remaining Durability", {}, "m_showRemaining"},
        {"maximum", "Maximum Durability", {}, "m_showMaxDurability"},
        {"percentage", "Percentage Remaining", {}, "m_showPercentage"}
    };
    schema.node(std::move(contents));

    section("text_layout", "Label Placement", "text");
    auto position = node("m_durabilityTextPosition", "Text Position", "text", ConfigControlTypeV2::Choice);
    position.section = "text_layout";
    position.choiceStyle = ConfigChoiceStyleV2::Segmented;
    position.options = {{"0", "Right"}, {"1", "Left"}, {"2", "Below"}};
    position.defaultValue = "0";
    schema.node(std::move(position));
    slider("m_durabilityTextGap", "Distance From Item", "text", "text_layout", "0", "100");
    section("text_style", "Label Appearance", "text");
    slider("m_durabilityTextSize", "Text Size", "text", "text_style", "6", "100");
    auto color = node("m_textColor", "Text Color", "text", ConfigControlTypeV2::Color);
    color.section = "text_style";
    color.defaultValue = "#FFFFFF";
    schema.node(std::move(color));

    auto help = node("editor_help", "Move Items In The HUD Editor", "editor", ConfigControlTypeV2::Info);
    help.key.clear();
    help.description = "Open the HUD Editor to drag each item separately. Existing positions are preserved.";
    schema.node(std::move(help));
    section("snapping", "Snapping", "editor");
    auto snapping = node("snap_targets", "Snap To", "editor", ConfigControlTypeV2::ToggleGroup);
    snapping.key.clear();
    snapping.section = "snapping";
    snapping.choiceStyle = ConfigChoiceStyleV2::Chips;
    snapping.options = {
        {"grid", "Grid", {}, "m_snapToGrid"},
        {"items", "Other Items", {}, "m_snapToElements"},
        {"center", "Screen Center", {}, "m_snapToScreenCenter"}
    };
    schema.node(std::move(snapping));
    slider("m_gridSize", "Grid Size", "editor", "snapping", "1", "100", "m_snapToGrid");
    slider("m_gridGap", "Gap Between Items", "editor", "snapping", "0", "100", "m_snapToElements");
    slider("m_snapThreshold", "Snap Distance", "editor", "snapping", "1", "100");

    pl::modmenu::setConfigSchemaJson(moduleId, schema.toJson());
}

void ArmorHudModule::onDisable() {
    clearRuntime();
    pl::modmenu::submitDrawCommands(moduleId, std::span<const pl::modmenu::DrawCommand>{});
    pl::modmenu::submitHudEditorElements(moduleId, std::span<const pl::modmenu::HudEditorElement>{});
}

ArmorHudModule::ConfigSnapshot ArmorHudModule::snapshotConfig() const {
    std::lock_guard lock(m_configMutex);
    return {
        std::array<SlotConfig, SlotCount>{
            SlotConfig{m_helmet, m_helmetDurability, hudHelmetPosX, hudHelmetPosY, m_helmetSize},
            SlotConfig{m_chestplate, m_chestplateDurability, hudChestplatePosX, hudChestplatePosY, m_chestplateSize},
            SlotConfig{m_leggings, m_leggingsDurability, hudLeggingsPosX, hudLeggingsPosY, m_leggingsSize},
            SlotConfig{m_boots, m_bootsDurability, hudBootsPosX, hudBootsPosY, m_bootsSize},
            SlotConfig{m_offhand, m_offhandDurability, hudOffhandPosX, hudOffhandPosY, m_offhandSize},
            SlotConfig{m_mainhand, m_mainhandDurability, hudMainhandPosX, hudMainhandPosY, m_mainhandSize}
        },
        m_showDamage,
        m_showRemaining,
        m_showMaxDurability,
        m_showPercentage,
        m_durabilityTextSize,
        m_durabilityTextPosition,
        m_durabilityTextGap,
        huditems::parseColor(m_textColor),
        m_gridSize,
        m_gridGap,
        m_snapThreshold,
        (m_snapToGrid ? pl::modmenu::HudSnapGrid : pl::modmenu::HudSnapNone) |
            (m_snapToElements ? pl::modmenu::HudSnapElements : pl::modmenu::HudSnapNone) |
            (m_snapToScreenCenter ? pl::modmenu::HudSnapScreenCenter : pl::modmenu::HudSnapNone)
    };
}

void ArmorHudModule::clearRuntime() {
    for (auto& slot : m_runtime) {
        slot.hasItem.store(false, std::memory_order_release);
        slot.damage.store(0, std::memory_order_release);
        slot.maxDamage.store(0, std::memory_order_release);
    }
}

void ArmorHudModule::submitEditorElements(const ConfigSnapshot& config) {
    std::vector<pl::modmenu::HudEditorElement> elements;
    elements.reserve(SlotCount);
    for (std::size_t i = 0; i < SlotCount; ++i) {
        const SlotConfig& slot = config.slots[i];
        if (!slot.enabled) continue;
        pl::modmenu::HudEditorElement element;
        element.elementId = HudElementIds[i];
        element.displayName = HudElementNames[i];
        element.positionKeyX = HudXKeys[i];
        element.positionKeyY = HudYKeys[i];
        element.x = slot.x;
        element.y = slot.y;
        element.width = std::max(1.0f, slot.size);
        element.height = std::max(1.0f, slot.size);
        element.gridSize = config.gridSize;
        element.snapThreshold = config.snapThreshold;
        element.gridGap = config.gridGap;
        element.snapFlags = config.snapFlags;
        elements.push_back(std::move(element));
    }
    pl::modmenu::submitHudEditorElements(moduleId, elements);
}

void ArmorHudModule::renderNative(void* context, void* client) {
    huditems::IconPainter painter(context, client);
    void* localPlayer = painter.player();
    if (!localPlayer) {
        clearRuntime();
        return;
    }

    const ConfigSnapshot config = snapshotConfig();
    const auto stacks = getHudStacks(localPlayer);

    // Dyed leather needs the opacity fix pass before the regular icons.
    if (painter.supportsOpacityFix()) {
        painter.beginOpacityFixPass();
        for (std::size_t i = 0; i < SlotCount; ++i) {
            const SlotConfig& slot = config.slots[i];
            void* stack = stacks[i];
            void* item = huditems::stackItem(stack);
            if (!slot.enabled || !item || slot.size <= 0.0f || !huditems::needsTextureOpacityPass(stack)) continue;
            painter.drawOpacityFix(stack, item, slot.x, slot.y, slot.size);
        }
        painter.endOpacityFixPass();
    }

    for (std::size_t i = 0; i < SlotCount; ++i) {
        void* stack = stacks[i];
        void* item = huditems::stackItem(stack);
        const bool hasItem = item != nullptr;
        int damage = 0;
        int maxDamage = 0;
        if (hasItem) {
            damage = huditems::stackDamage(stack);
            maxDamage = huditems::itemMaxDamage(item);
        }
        m_runtime[i].hasItem.store(hasItem, std::memory_order_release);
        m_runtime[i].damage.store(damage, std::memory_order_release);
        m_runtime[i].maxDamage.store(maxDamage, std::memory_order_release);

        const SlotConfig& slot = config.slots[i];
        if (!slot.enabled || !hasItem || slot.size <= 0.0f) continue;
        painter.draw(stack, item, slot.x, slot.y, slot.size);
    }
}

void ArmorHudModule::onFrame() {
    if (!enabled) return;

    const ConfigSnapshot config = snapshotConfig();
    submitEditorElements(config);

    std::vector<pl::modmenu::DrawCommand> commands;
    commands.reserve(SlotCount);

    for (std::size_t i = 0; i < SlotCount; ++i) {
        const SlotConfig& slot = config.slots[i];
        if (!slot.enabled) continue;

        const bool hasItem = m_runtime[i].hasItem.load(std::memory_order_acquire);
        const int damage = m_runtime[i].damage.load(std::memory_order_acquire);
        const int maxDamage = m_runtime[i].maxDamage.load(std::memory_order_acquire);
        if (!slot.durability || !hasItem || maxDamage <= 0) continue;

        const int safeDamage = std::clamp(damage, 0, maxDamage);
        const int remaining = maxDamage - safeDamage;
        const int percentage = static_cast<int>(std::lround(remaining * 100.0 / maxDamage));
        std::string text;
        auto append = [&](const std::string& part) {
            if (!text.empty()) text += "  ";
            text += part;
        };
        if (config.showDamage) append(std::to_string(safeDamage) + " dmg");
        if (config.showRemaining && config.showMaxDurability) {
            append(std::to_string(remaining) + "/" + std::to_string(maxDamage));
        } else if (config.showRemaining) {
            append(std::to_string(remaining));
        } else if (config.showMaxDurability) {
            append(std::to_string(maxDamage) + " max");
        }
        if (config.showPercentage) append(std::to_string(percentage) + "%");
        if (text.empty()) continue;

        pl::modmenu::DrawCommand durability;
        durability.type = pl::modmenu::DrawCommandType::Text;
        const float verticalCenterBaseline = slot.y + slot.size * 0.5f + config.durabilityTextSize * 0.35f;
        if (config.durabilityTextPosition == 1) {
            durability.x = slot.x - config.durabilityTextGap;
            durability.y = verticalCenterBaseline;
            durability.w = -1.0f;
        } else if (config.durabilityTextPosition == 2) {
            durability.x = slot.x + slot.size * 0.5f;
            durability.y = slot.y + slot.size + config.durabilityTextGap + config.durabilityTextSize;
            durability.w = -2.0f;
        } else {
            durability.x = slot.x + slot.size + config.durabilityTextGap;
            durability.y = verticalCenterBaseline;
            durability.w = 0.0f;
        }
        durability.color = config.textColor;
        durability.size = config.durabilityTextSize;
        durability.text = std::move(text);
        commands.push_back(std::move(durability));
    }

    pl::modmenu::submitDrawCommands(moduleId, commands);
}

void ArmorHudModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard lock(m_configMutex);

    if (j.contains("m_helmet")) m_helmet = j["m_helmet"].get<bool>();
    if (j.contains("m_helmetDurability")) m_helmetDurability = j["m_helmetDurability"].get<bool>();
    if (j.contains("hudHelmetPosX")) hudHelmetPosX = std::clamp(j["hudHelmetPosX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("hudHelmetPosY")) hudHelmetPosY = std::clamp(j["hudHelmetPosY"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("m_helmetSize")) m_helmetSize = std::clamp(j["m_helmetSize"].get<float>(), 8.0f, 100.0f);

    if (j.contains("m_chestplate")) m_chestplate = j["m_chestplate"].get<bool>();
    if (j.contains("m_chestplateDurability")) m_chestplateDurability = j["m_chestplateDurability"].get<bool>();
    if (j.contains("hudChestplatePosX")) hudChestplatePosX = std::clamp(j["hudChestplatePosX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("hudChestplatePosY")) hudChestplatePosY = std::clamp(j["hudChestplatePosY"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("m_chestplateSize")) m_chestplateSize = std::clamp(j["m_chestplateSize"].get<float>(), 8.0f, 100.0f);

    if (j.contains("m_leggings")) m_leggings = j["m_leggings"].get<bool>();
    if (j.contains("m_leggingsDurability")) m_leggingsDurability = j["m_leggingsDurability"].get<bool>();
    if (j.contains("hudLeggingsPosX")) hudLeggingsPosX = std::clamp(j["hudLeggingsPosX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("hudLeggingsPosY")) hudLeggingsPosY = std::clamp(j["hudLeggingsPosY"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("m_leggingsSize")) m_leggingsSize = std::clamp(j["m_leggingsSize"].get<float>(), 8.0f, 100.0f);

    if (j.contains("m_boots")) m_boots = j["m_boots"].get<bool>();
    if (j.contains("m_bootsDurability")) m_bootsDurability = j["m_bootsDurability"].get<bool>();
    if (j.contains("hudBootsPosX")) hudBootsPosX = std::clamp(j["hudBootsPosX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("hudBootsPosY")) hudBootsPosY = std::clamp(j["hudBootsPosY"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("m_bootsSize")) m_bootsSize = std::clamp(j["m_bootsSize"].get<float>(), 8.0f, 100.0f);

    if (j.contains("m_offhand")) m_offhand = j["m_offhand"].get<bool>();
    if (j.contains("m_offhandDurability")) m_offhandDurability = j["m_offhandDurability"].get<bool>();
    if (j.contains("hudOffhandPosX")) hudOffhandPosX = std::clamp(j["hudOffhandPosX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("hudOffhandPosY")) hudOffhandPosY = std::clamp(j["hudOffhandPosY"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("m_offhandSize")) m_offhandSize = std::clamp(j["m_offhandSize"].get<float>(), 8.0f, 100.0f);

    if (j.contains("m_mainhand")) m_mainhand = j["m_mainhand"].get<bool>();
    if (j.contains("m_mainhandDurability")) m_mainhandDurability = j["m_mainhandDurability"].get<bool>();
    if (j.contains("hudMainhandPosX")) hudMainhandPosX = std::clamp(j["hudMainhandPosX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("hudMainhandPosY")) hudMainhandPosY = std::clamp(j["hudMainhandPosY"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("m_mainhandSize")) m_mainhandSize = std::clamp(j["m_mainhandSize"].get<float>(), 8.0f, 100.0f);

    if (j.contains("m_showDamage")) m_showDamage = j["m_showDamage"].get<bool>();
    if (j.contains("m_showRemaining")) m_showRemaining = j["m_showRemaining"].get<bool>();
    if (j.contains("m_showMaxDurability")) m_showMaxDurability = j["m_showMaxDurability"].get<bool>();
    if (j.contains("m_showPercentage")) m_showPercentage = j["m_showPercentage"].get<bool>();
    if (j.contains("m_durabilityTextSize")) m_durabilityTextSize = std::clamp(j["m_durabilityTextSize"].get<float>(), 6.0f, 100.0f);
    if (j.contains("m_durabilityTextPosition")) {
        try {
            std::string value = j["m_durabilityTextPosition"].get<std::string>();
            const std::size_t separator = value.find(',');
            if (separator != std::string::npos) value.resize(separator);
            m_durabilityTextPosition = std::clamp(std::stoi(value), 0, 2);
        } catch (...) {
        }
    }
    if (j.contains("m_durabilityTextGap")) m_durabilityTextGap = std::clamp(j["m_durabilityTextGap"].get<float>(), 0.0f, 100.0f);
    if (j.contains("m_textColor")) m_textColor = j["m_textColor"].get<std::string>();
    if (j.contains("m_gridSize")) m_gridSize = std::clamp(j["m_gridSize"].get<float>(), 1.0f, 100.0f);
    if (j.contains("m_gridGap")) m_gridGap = std::clamp(j["m_gridGap"].get<float>(), 0.0f, 100.0f);
    if (j.contains("m_snapThreshold")) m_snapThreshold = std::clamp(j["m_snapThreshold"].get<float>(), 1.0f, 100.0f);
    if (j.contains("m_snapToGrid")) m_snapToGrid = j["m_snapToGrid"].get<bool>();
    if (j.contains("m_snapToElements")) m_snapToElements = j["m_snapToElements"].get<bool>();
    if (j.contains("m_snapToScreenCenter")) m_snapToScreenCenter = j["m_snapToScreenCenter"].get<bool>();
}

void ArmorHudModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard lock(m_configMutex);

    j["m_helmet"] = m_helmet;
    j["m_helmetDurability"] = m_helmetDurability;
    j["hudHelmetPosX"] = hudHelmetPosX;
    j["hudHelmetPosY"] = hudHelmetPosY;
    j["m_helmetSize"] = m_helmetSize;

    j["m_chestplate"] = m_chestplate;
    j["m_chestplateDurability"] = m_chestplateDurability;
    j["hudChestplatePosX"] = hudChestplatePosX;
    j["hudChestplatePosY"] = hudChestplatePosY;
    j["m_chestplateSize"] = m_chestplateSize;

    j["m_leggings"] = m_leggings;
    j["m_leggingsDurability"] = m_leggingsDurability;
    j["hudLeggingsPosX"] = hudLeggingsPosX;
    j["hudLeggingsPosY"] = hudLeggingsPosY;
    j["m_leggingsSize"] = m_leggingsSize;

    j["m_boots"] = m_boots;
    j["m_bootsDurability"] = m_bootsDurability;
    j["hudBootsPosX"] = hudBootsPosX;
    j["hudBootsPosY"] = hudBootsPosY;
    j["m_bootsSize"] = m_bootsSize;

    j["m_offhand"] = m_offhand;
    j["m_offhandDurability"] = m_offhandDurability;
    j["hudOffhandPosX"] = hudOffhandPosX;
    j["hudOffhandPosY"] = hudOffhandPosY;
    j["m_offhandSize"] = m_offhandSize;

    j["m_mainhand"] = m_mainhand;
    j["m_mainhandDurability"] = m_mainhandDurability;
    j["hudMainhandPosX"] = hudMainhandPosX;
    j["hudMainhandPosY"] = hudMainhandPosY;
    j["m_mainhandSize"] = m_mainhandSize;

    j["m_showDamage"] = m_showDamage;
    j["m_showRemaining"] = m_showRemaining;
    j["m_showMaxDurability"] = m_showMaxDurability;
    j["m_showPercentage"] = m_showPercentage;
    j["m_durabilityTextSize"] = m_durabilityTextSize;
    j["m_durabilityTextPosition"] = std::to_string(m_durabilityTextPosition) + ",Right,Left,Below";
    j["m_durabilityTextGap"] = m_durabilityTextGap;
    j["m_textColor"] = m_textColor;
    j["m_gridSize"] = m_gridSize;
    j["m_gridGap"] = m_gridGap;
    j["m_snapThreshold"] = m_snapThreshold;
    j["m_snapToGrid"] = m_snapToGrid;
    j["m_snapToElements"] = m_snapToElements;
    j["m_snapToScreenCenter"] = m_snapToScreenCenter;
}

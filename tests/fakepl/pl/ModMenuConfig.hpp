#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace pl::modmenu {

inline constexpr std::uint32_t HudSnapNone = 0;
inline constexpr std::uint32_t HudSnapGrid = 1 << 0;
inline constexpr std::uint32_t HudSnapElements = 1 << 1;
inline constexpr std::uint32_t HudSnapScreenCenter = 1 << 2;

struct HudSurfaceSize {
    float width = 1920.0f;
    float height = 1080.0f;
};

inline HudSurfaceSize getHudSurfaceSize() {
    return {1920.0f, 1080.0f};
}

struct HudEditorElement {
    std::string elementId;
    std::string displayName;
    std::string positionKeyX;
    std::string positionKeyY;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float gridSize = 16.0f;
    float snapThreshold = 12.0f;
    float gridGap = 4.0f;
    std::uint32_t snapFlags = 0;
};

inline void submitHudEditorElements(std::string_view, std::span<const HudEditorElement>) {}
inline void submitHudEditorElements(std::string_view moduleId, const std::vector<HudEditorElement>& elements) {
    submitHudEditorElements(moduleId, std::span<const HudEditorElement>(elements.data(), elements.size()));
}

enum class ConfigControlTypeV2 {
    Section,
    Toggle,
    ToggleGroup,
    SliderFloat,
    SliderInt,
    Choice,
    Color,
    Keybind,
    Info,
    Button,
    MultilineText
};

enum class ConfigChoiceStyleV2 {
    Chips,
    Checklist,
    Segmented
};

enum class ConfigConditionOpV2 {
    Truthy,
    Falsy
};

struct ConfigOptionV2 {
    std::string id;
    std::string title;
    std::string description;
    std::string key;
};

struct ConfigConditionV2 {
    std::string key;
    ConfigConditionOpV2 op;
    std::string value;
};

struct ConfigNodeV2 {
    std::string id;
    std::string key;
    std::string title;
    std::string description;
    std::string category;
    std::string section;
    ConfigControlTypeV2 type = ConfigControlTypeV2::Toggle;
    ConfigChoiceStyleV2 choiceStyle = ConfigChoiceStyleV2::Chips;
    std::vector<ConfigOptionV2> options;
    std::string minValue;
    std::string maxValue;
    std::string step;
    std::string unit;
    std::vector<ConfigConditionV2> visibleWhen;
    std::string defaultValue;
    std::string currentValue;
    bool advanced = false;
    std::string actionValue;
    bool colorAlpha = true;
};

struct ConfigCategoryV2 {
    std::string id;
    std::string title;
    std::string description;
};

class ConfigSchemaBuilder {
public:
    ConfigSchemaBuilder& defaultCategory(std::string cat) {
        m_defaultCategory = std::move(cat);
        return *this;
    }
    ConfigSchemaBuilder& category(std::string id, std::string title, std::string desc = "") {
        m_categories.push_back({std::move(id), std::move(title), std::move(desc)});
        return *this;
    }
    ConfigSchemaBuilder& node(ConfigNodeV2 node) {
        m_nodes.push_back(std::move(node));
        return *this;
    }
    std::string toJson() const {
        return "{}";
    }

private:
    std::string m_defaultCategory;
    std::vector<ConfigCategoryV2> m_categories;
    std::vector<ConfigNodeV2> m_nodes;
};

inline void setConfigSchemaJson(std::string_view, std::string_view) {}

}

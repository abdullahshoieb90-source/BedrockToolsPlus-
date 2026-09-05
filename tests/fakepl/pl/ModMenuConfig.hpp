// Host-side fake of the Preloader <pl/ModMenuConfig.hpp> header. Mirrors the
// config-schema API surface the HUD modules use (ConfigSchemaBuilder /
// ConfigNodeV2 / the control-type enums) so modules compile and run on the
// host against tests/fakepl. The builder records nothing structural; toJson
// only reports the node count, which is all the host tests need.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pl::modmenu {

enum class ConfigControlTypeV2 {
    Toggle,
    SliderInt,
    SliderFloat,
    Radio,
    Choice,
    Color,
    Keybind,
    Text,
    Button,
    Section,
    ToggleGroup,
    Info,
};

enum class ConfigChoiceStyleV2 {
    Default,
    Chips,
    Checklist,
    Segmented,
};

struct ConfigOptionV2 {
    std::string value;
    std::string label;
    std::string description;
    std::string key;
};

struct ConfigNodeV2 {
    std::string id;
    std::string key;
    std::string title;
    std::string category;
    std::string section;
    std::string description;
    std::string defaultValue;
    std::string minValue;
    std::string maxValue;
    std::string step;
    std::string unit;
    ConfigControlTypeV2 type = ConfigControlTypeV2::Toggle;
    ConfigChoiceStyleV2 choiceStyle = ConfigChoiceStyleV2::Default;
    std::vector<ConfigOptionV2> options;
};

class ConfigSchemaBuilder {
public:
    ConfigSchemaBuilder& defaultCategory(std::string category) {
        mDefaultCategory = std::move(category);
        return *this;
    }

    ConfigSchemaBuilder& category(std::string id, std::string title, std::string description) {
        mCategories.emplace_back(std::move(id), std::move(title));
        (void)description;
        return *this;
    }

    ConfigSchemaBuilder& node(ConfigNodeV2 value) {
        mNodes.push_back(std::move(value));
        return *this;
    }

    std::string toJson() const {
        return "{\"nodes\":" + std::to_string(mNodes.size()) + "}";
    }

private:
    std::string mDefaultCategory;
    std::vector<std::pair<std::string, std::string>> mCategories;
    std::vector<ConfigNodeV2> mNodes;
};

inline std::string& recordedConfigSchema() {
    static std::string schema;
    return schema;
}

inline void setConfigSchemaJson(std::string_view, std::string_view json) {
    recordedConfigSchema().assign(json.data(), json.size());
}

} // namespace pl::modmenu

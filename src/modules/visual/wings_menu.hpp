#pragma once

// The launcher menu page for the Wings module.
//
// Kept separate from the renderer and expressed as a pure function so the exact
// JSON the launcher receives can be built and asserted on the host: a node that
// serializes wrong is a node that silently does not appear in the menu, and that
// is not something the game side can tell you.

#include "wings_art.hpp"
#include "wings_design.hpp"

#include <pl/ModMenuConfig.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace bedrocktools::modules::wings {

// Builds the v2 config schema for the module: the style picker (each option
// carrying its one line shape description), the silhouette preview of
// `styleIndex`, and the flap speed slider.
inline std::string wingMenuSchema(int styleIndex, float flapSpeed) {
    using namespace pl::modmenu;

    if (styleIndex < 0 || styleIndex >= kWingStyleCount) styleIndex = 0;
    const WingDesign& style = kWingDesigns[styleIndex];

    ConfigSchemaBuilder schema;
    schema.defaultCategory("style")
        .category("style", "Style", "Which wings you wear")
        .category("preview", "Preview", "Shape of every style, drawn from the real geometry")
        .category("motion", "Motion", "How the wings move");

    auto node = [](std::string key, std::string title, std::string category,
                   ConfigControlTypeV2 type) {
        ConfigNodeV2 value;
        value.id = key;
        value.key = std::move(key);
        value.title = std::move(title);
        value.category = std::move(category);
        value.type = type;
        return value;
    };

    // --- Style picker -------------------------------------------------------
    // Every option carries a one line description of its shape as well as its
    // name, so the list is readable even if the silhouette below is collapsed.
    {
        auto picker = node("m_wingStyle", "Wings", "style", ConfigControlTypeV2::Choice);
        picker.choiceStyle = ConfigChoiceStyleV2::Dropdown;
        picker.currentValue = style.id;
        picker.hasCurrentValue = true;
        picker.description = "Pick a style, then read the shape straight off the preview below.";
        for (int i = 0; i < kWingStyleCount; ++i) {
            const WingDesign& design = kWingDesigns[i];
            ConfigOptionV2 option;
            option.value = design.id;
            option.label = design.label;
            option.description = design.shape;
            picker.options.push_back(std::move(option));
        }
        schema.node(std::move(picker));
    }

    // --- Live silhouette ----------------------------------------------------
    // Rasterized from buildWing(), so it is the same geometry the game draws.
    {
        auto art = node("wing_preview", std::string("Shape: ") + style.label, "preview",
                        ConfigControlTypeV2::Info);
        art.key.clear();
        art.description = wingStyleArt(styleIndex);
        schema.node(std::move(art));
    }

    // --- Overlay preview ----------------------------------------------------
    {
        auto preview = node("m_showStylePreview", "Draw The Shape On Screen", "preview",
                            ConfigControlTypeV2::Toggle);
        preview.defaultValue = "true";
        preview.currentValue = "true";
        preview.hasCurrentValue = true;
        preview.description =
            "Draws the selected style's silhouette over the screen, large enough to "
            "compare with the wings on your back. The dropdown above also describes "
            "each shape in words.";
        schema.node(std::move(preview));
    }

    // --- Flap speed ---------------------------------------------------------
    {
        auto speed = node("m_flapSpeed", "Flap Speed", "motion",
                          ConfigControlTypeV2::SliderFloat);
        speed.minValue = "0.1";
        speed.maxValue = "10";
        speed.step = "0.1";
        speed.unit = "x";
        speed.defaultValue = "1";
        speed.currentValue = std::to_string(flapSpeed);
        speed.hasCurrentValue = true;
        schema.node(std::move(speed));
    }

    return schema.toJson();
}

}  // namespace bedrocktools::modules::wings

#pragma once

// Shared helpers that let launcher overlay buttons (Comment Key / Command
// Hotkey) honor their module's "Button Opacity" setting. The launcher hosts
// these buttons, so a module cannot call View#setAlpha on them directly; the
// opacity has to travel inside the button definition itself:
//   * the Minecraft-style frame comes from an SVG icon, which the launcher
//     rasterizes with lunasvg. lunasvg honors the root element's opacity
//     attribute, so injecting `opacity="..."` there fades the whole frame.
//   * the label is drawn by a TextView whose color keeps an alpha byte, so
//     packing the opacity into the ARGB text colors fades the text too.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace overlaystyle {

// Opacity values below this are clamped so a button never becomes fully
// invisible (matching the module config clamp of 0.05..1.0).
constexpr float kMinOpacity = 0.05f;

inline float clampedOpacity(float opacity) {
    return std::clamp(opacity, kMinOpacity, 1.0f);
}

// Packs the opacity into an ARGB color's alpha byte. rgb keeps its channels;
// any incoming alpha is replaced.
inline std::uint32_t alphaColor(std::uint32_t rgb, float opacity) {
    const auto alpha = static_cast<std::uint32_t>(clampedOpacity(opacity) * 255.0f + 0.5f);
    return (alpha << 24) | (rgb & 0x00FFFFFFu);
}

// Returns a copy of `svg` with a root-level opacity attribute. Fully opaque
// requests return the SVG unchanged so the common case keeps byte-identical
// icons (the launcher caches rasterized icons per definition).
inline std::string opacitySvg(const char* svg, float opacity) {
    std::string result(svg ? svg : "");
    if (opacity >= 0.999f) return result;

    char attribute[48];
    std::snprintf(attribute, sizeof(attribute), " opacity=\"%.3f\"", clampedOpacity(opacity));
    const std::size_t tag = result.find("<svg");
    if (tag == std::string::npos) return result;
    const std::size_t tagEnd = result.find('>', tag);
    if (tagEnd == std::string::npos) return result;
    result.insert(tagEnd, attribute);
    return result;
}

} // namespace overlaystyle

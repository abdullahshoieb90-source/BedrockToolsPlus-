#pragma once

// SVG artwork for the on-screen slot buttons of the Hotbar Slots module.
//
// The launcher draws its overlay buttons into an Android window that sits
// *above* the Minecraft surface, while this module paints the item icons
// natively *inside* that surface (HudCameraRenderer hook). A button with a
// solid face therefore covers the icon completely, which is why "Icon
// Placement = On Slot Buttons" showed nothing but empty frames.
//
// The launcher's own HotbarSlotOverlay solves the same problem in
// createBitmap(): it draws the slot frame and then clears the item window
// (93..419 of a 512 px sprite) with PorterDuff.CLEAR. The artwork below does
// the equivalent declaratively, with `fill-rule="evenodd"` paths whose second
// subpath is the item window: a point covered by two subpaths is not filled,
// so the window stays transparent instead of being painted over.
//
// The window is derived from IconWindowStart/IconWindowEnd - the exact rect
// buttonIconRect() paints the icon into - so what the module draws always
// lands inside the hole, in the resting and in the pressed artwork.
//
// Header-only and free of Minecraft/launcher types so the host unit tests can
// parse and rasterize the very markup that is handed to the launcher.

#include "hotbarslots_layout.hpp"

#include <cstdio>
#include <string>

namespace bedrocktools::hotbar {

// The artwork is authored in a 64x64 viewBox, like every other button icon in
// the mod.
inline constexpr float ButtonArtSize = 64.0f;

// Item window of the artwork, in viewBox units: 93/512*64 = 11.625 and
// 419/512*64 = 52.375, the same proportions the launcher clears.
inline constexpr float IconWindowMin = IconWindowStart * ButtonArtSize;
inline constexpr float IconWindowMax = IconWindowEnd * ButtonArtSize;

// The pressed artwork pulls the slot face towards the centre, like the Zoom
// and Command Hotkey buttons do. The window itself must not move with it,
// otherwise holding a button would cover the icon again.
inline constexpr float ActiveFaceScale = 0.85f;

namespace detail {

inline std::string svgNumber(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
    return std::string(buffer);
}

// Closed clockwise square, the only path command the launcher icons use.
inline std::string svgSquare(float min, float max) {
    const std::string lo = svgNumber(min);
    const std::string hi = svgNumber(max);
    return "M" + lo + "," + lo + " L" + hi + "," + lo + " L" + hi + "," + hi + " L" + lo + "," + hi + " Z";
}

// Scales a coordinate towards the centre of the viewBox (the pressed look).
inline float activeCoord(float value) {
    const float centre = ButtonArtSize * 0.5f;
    return centre + (value - centre) * ActiveFaceScale;
}

// Hollowed Minecraft slot frame: light bevel, darker face, both with the item
// window cut out. The bevel lines are stroked on paths of their own - a stroke
// on a window subpath would paint straight back into the hole. `innerBevel`
// drops the second face outline, which the pressed artwork cannot use because
// it would land on the window edge.
inline std::string svgHollowFrame(float faceMin, float faceMax, bool innerBevel) {
    const std::string window = svgSquare(IconWindowMin, IconWindowMax);
    std::string svg;
    svg += "    <path fill=\"#C6C6C6\" fill-rule=\"evenodd\" d=\"" +
           svgSquare(2.0f, 62.0f) + " " + window + "\"/>\n";
    svg += "    <path fill=\"none\" stroke=\"#373737\" stroke-width=\"2\" d=\"" +
           svgSquare(2.0f, 62.0f) + " " + svgSquare(4.0f, 60.0f) + "\"/>\n";
    svg += "    <path fill=\"#8B8B8B\" fill-rule=\"evenodd\" d=\"" +
           svgSquare(faceMin, faceMax) + " " + window + "\"/>\n";
    svg += "    <path fill=\"none\" stroke=\"#5B5B5B\" stroke-width=\"2\" d=\"" +
           svgSquare(faceMin, faceMax) + (innerBevel ? " " + svgSquare(8.0f, 56.0f) : std::string{}) + "\"/>\n";
    return svg;
}

// The solid frame the buttons had before the window was cut out; still used by
// the HUD strip placement, where nothing is painted behind the button.
inline std::string svgSolidFrame(bool pressed) {
    std::string svg;
    svg += "    <path fill=\"#C6C6C6\" stroke=\"#373737\" stroke-width=\"2\" d=\"" +
           svgSquare(2.0f, 62.0f) + " " + svgSquare(4.0f, 60.0f) + "\"/>\n";
    if (pressed) svg += "    <g transform=\"translate(32, 32) scale(0.85) translate(-32, -32)\">\n";
    svg += std::string(pressed ? "        " : "    ") +
           "<path fill=\"#8B8B8B\" stroke=\"#5B5B5B\" stroke-width=\"2\" d=\"" +
           svgSquare(6.0f, 58.0f) + " " + svgSquare(8.0f, 56.0f) + "\"/>\n";
    if (pressed) svg += "    </g>\n";
    return svg;
}

inline std::string svgDocument(const std::string& body) {
    return "<svg viewBox=\"0 0 64 64\" xmlns=\"http://www.w3.org/2000/svg\">\n" + body + "</svg>";
}

} // namespace detail

// Resting slot button. `hollow` cuts the item window out of the frame so the
// natively painted item icon shows through; without it the button keeps the
// solid face used by the HUD strip placement.
inline std::string slotButtonSvg(bool hollow) {
    return detail::svgDocument(hollow ? detail::svgHollowFrame(6.0f, 58.0f, true)
                                      : detail::svgSolidFrame(false));
}

// Pressed slot button (the slot buttons use ButtonBehavior::Hold, so this is
// the artwork shown while a slot is being tapped). The window stays put even
// though the face shrinks, so the icon never gets covered while held.
inline std::string slotButtonActiveSvg(bool hollow) {
    if (!hollow) return detail::svgDocument(detail::svgSolidFrame(true));
    return detail::svgDocument(
        detail::svgHollowFrame(detail::activeCoord(6.0f), detail::activeCoord(58.0f), false));
}

} // namespace bedrocktools::hotbar

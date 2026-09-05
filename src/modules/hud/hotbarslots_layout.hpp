#pragma once

// Pure layout math for the Hotbar Slots module.
//
// The icon strip drawn by the module is positioned from a single HUD editor
// anchor plus a slot size / gap, exactly like the LeviLauncher "Hotbar Slot"
// buttons are laid out by its overlay manager. Keeping the math header-only
// and free of Minecraft types lets the host unit tests cover it.

#include <cstddef>

namespace bedrocktools::hotbar {

inline constexpr std::size_t SlotCount = 9;

struct StripLayout {
    float x = 0.0f;         // anchor of the first slot, in HUD units
    float y = 0.0f;
    float slotSize = 32.0f; // width and height of one slot
    float gap = 4.0f;       // space between two slots
    bool vertical = false;  // stack the slots downwards instead of sideways
};

struct SlotRect {
    float x = 0.0f;
    float y = 0.0f;
    float size = 0.0f;
};

// Position of a single slot inside the strip. Out-of-range indices are
// clamped to the strip so callers can never produce a rectangle outside it.
inline SlotRect slotRect(const StripLayout& layout, std::size_t index) {
    if (index >= SlotCount) index = SlotCount - 1;
    const float step = layout.slotSize + layout.gap;
    const float offset = step * static_cast<float>(index);
    SlotRect rect;
    rect.size = layout.slotSize;
    rect.x = layout.vertical ? layout.x : layout.x + offset;
    rect.y = layout.vertical ? layout.y + offset : layout.y;
    return rect;
}

// Total size of the strip, used for the HUD editor element box.
inline SlotRect stripBounds(const StripLayout& layout, std::size_t visibleSlots = SlotCount) {
    if (visibleSlots > SlotCount) visibleSlots = SlotCount;
    SlotRect rect;
    rect.x = layout.x;
    rect.y = layout.y;
    rect.size = layout.slotSize;
    if (visibleSlots == 0) rect.size = 0.0f;
    return rect;
}

inline float stripWidth(const StripLayout& layout, std::size_t visibleSlots = SlotCount) {
    if (visibleSlots == 0) return 0.0f;
    if (visibleSlots > SlotCount) visibleSlots = SlotCount;
    const float span = layout.slotSize * static_cast<float>(visibleSlots) +
                       layout.gap * static_cast<float>(visibleSlots - 1);
    return layout.vertical ? layout.slotSize : span;
}

inline float stripHeight(const StripLayout& layout, std::size_t visibleSlots = SlotCount) {
    if (visibleSlots == 0) return 0.0f;
    if (visibleSlots > SlotCount) visibleSlots = SlotCount;
    const float span = layout.slotSize * static_cast<float>(visibleSlots) +
                       layout.gap * static_cast<float>(visibleSlots - 1);
    return layout.vertical ? span : layout.slotSize;
}

// ---------------------------------------------------------------------------
// Mapping an on-screen launcher button onto the HUD surface
// ---------------------------------------------------------------------------
//
// When "Use item icons from hotbar" paints the icons on the slot *buttons*
// (like the LeviLauncher built-in mod does) the module needs the button
// rectangle, which the launcher reports in screen pixels, expressed in the
// HUD units the item painter draws in.

// Geometry of one launcher overlay button, in screen pixels.
struct ButtonRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    bool visible = false;

    bool valid() const { return width > 0.0f && height > 0.0f; }
};

// Size of the surface the button coordinates are relative to (the game's
// decor view) and of the launcher HUD overlay, both in their own units.
struct SurfaceMapping {
    float screenWidth = 0.0f;
    float screenHeight = 0.0f;
    float hudWidth = 0.0f;
    float hudHeight = 0.0f;

    bool valid() const {
        return screenWidth > 0.0f && screenHeight > 0.0f && hudWidth > 0.0f && hudHeight > 0.0f;
    }
};

// The button artwork is a Minecraft slot frame whose inner (item) window only
// covers the middle of the bitmap; these are the same proportions the
// launcher's HotbarSlotOverlay uses (93..419 of a 512 px sprite). The window
// is also the hole the button artwork is cut with (see hotbarslots_buttons.hpp),
// so the icon and the transparent area always line up.
inline constexpr float IconWindowStart = 93.0f / 512.0f;
inline constexpr float IconWindowEnd = 419.0f / 512.0f;

// Inset buttonIconRect() is called with for a given icon size setting: 1.0
// fills the whole button, IconWindowEnd - IconWindowStart fills exactly the
// artwork's window. Larger settings grow the icon past the window, where the
// button frame covers its edges again.
inline float iconInset(float iconScale) {
    const float inset = (IconWindowEnd - IconWindowStart) * iconScale;
    if (inset < 0.05f) return 0.05f;
    if (inset > 1.0f) return 1.0f;
    return inset;
}

// Square, centred icon rectangle for a button, in HUD units. `inset` shrinks
// the icon towards the middle of the button (1.0 = full button).
inline SlotRect buttonIconRect(const ButtonRect& button, const SurfaceMapping& surface,
                               float inset = IconWindowEnd - IconWindowStart) {
    SlotRect rect;
    if (!button.valid() || !surface.valid()) return rect;
    if (inset <= 0.0f) return rect;
    if (inset > 1.0f) inset = 1.0f;

    const float scaleX = surface.hudWidth / surface.screenWidth;
    const float scaleY = surface.hudHeight / surface.screenHeight;

    const float hudX = button.x * scaleX;
    const float hudY = button.y * scaleY;
    const float hudW = button.width * scaleX;
    const float hudH = button.height * scaleY;

    // A square keeps the item icon undistorted even when the launcher button
    // was scaled non-uniformly.
    const float size = (hudW < hudH ? hudW : hudH) * inset;
    rect.size = size;
    rect.x = hudX + (hudW - size) * 0.5f;
    rect.y = hudY + (hudH - size) * 0.5f;
    return rect;
}

} // namespace bedrocktools::hotbar

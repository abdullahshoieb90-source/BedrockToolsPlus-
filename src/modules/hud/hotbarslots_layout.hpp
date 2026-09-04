#pragma once

// Pure layout math for the Hotbar Slots module.
//
// The icon strip drawn by the module is positioned from a single HUD editor
// anchor plus a slot size / gap, exactly like the LeviLauncher "Hotbar Slot"
// buttons are laid out by its overlay manager. Keeping the math header-only
// and free of Minecraft types lets the host unit tests cover it.

#include <cstddef>
#include <string_view>

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

// Destination rectangle of an item icon painted inside an on-screen overlay
// button ("Draw Icons On Buttons" mode). The icon is inset by `padding`
// (a fraction of the button size) so it neither touches the button frame nor
// covers the button's own label. Unlike the square strip slots, buttons may
// be rectangular, hence the separate width/height.
struct ButtonIconRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

inline ButtonIconRect iconRectInButton(float x, float y, float width, float height,
                                       float padding = 0.15f) {
    if (padding < 0.0f) padding = 0.0f;
    if (padding > 0.49f) padding = 0.49f;
    const float insetX = width * padding;
    const float insetY = height * padding;
    ButtonIconRect rect;
    rect.x = x + insetX;
    rect.y = y + insetY;
    rect.width = width - 2.0f * insetX;
    rect.height = height - 2.0f * insetY;
    return rect;
}

// Maps a Hotbar Slots overlay button id ("<prefix>1" .. "<prefix>9") back to
// its zero-based slot index. Returns -1 when the id does not belong to this
// module, so foreign or malformed overlay entries can never address a slot.
inline int slotIndexFromButtonId(std::string_view buttonId, std::string_view prefix) {
    if (buttonId.size() != prefix.size() + 1) return -1;
    if (buttonId.compare(0, prefix.size(), prefix) != 0) return -1;
    const char digit = buttonId[prefix.size()];
    if (digit < '1' || digit > '9') return -1;
    const int index = digit - '1';
    return index >= 0 && static_cast<std::size_t>(index) < SlotCount ? index : -1;
}

} // namespace bedrocktools::hotbar

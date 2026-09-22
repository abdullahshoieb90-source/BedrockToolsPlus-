#pragma once

// Pure layout math for the Armor module.
//
// The module paints the player's four armor pieces plus the offhand slot as a
// column of item icons on the HUD, optionally with remaining/maximum
// durability numbers beside each armor icon. It is a HUD element of its own,
// completely independent of the Inventory HUD module (which only owns the 9x3
// inventory grid). Keeping the math header-only and free of Minecraft types
// lets the host unit tests cover it.

#include "slotdecor_layout.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

namespace bedrocktools::armorhud {

using slotdecor::SlotRect;

// Order of the column: helmet, chestplate, leggings, boots, offhand.
inline constexpr std::size_t ArmorSlotCount = 4;
inline constexpr std::size_t SlotCount = 5;
inline constexpr std::size_t OffhandIndex = 4;

// Anchor and shape of the armor + offhand element.
struct ArmorLayout {
    float x = 0.0f;             // anchor (top-left of the column), HUD units
    float y = 0.0f;
    float slotSize = 32.0f;     // width and height of one slot
    float gap = 4.0f;           // space between two slots
    float armorTextSize = 0.0f; // 0 disables the durability labels beside armor
    bool horizontal = false;    // lay the slots out in a row instead of a column
};

// Keep labels inside their equipment row, even with tiny icons / large text.
inline float armorLabelTextSize(const ArmorLayout& layout) {
    return std::clamp(layout.armorTextSize, 0.0f, std::max(0.0f, layout.slotSize));
}

inline float armorLabelWidth(const ArmorLayout& layout) {
    // Item::getMaxDamage returns a short. Reserve enough room for 32767/32767
    // in the default font, without resizing the HUD as equipment wears down.
    return 7.0f * armorLabelTextSize(layout);
}

inline float armorLabelGap(const ArmorLayout& layout) {
    return std::max(layout.gap, layout.slotSize / 8.0f);
}

// Horizontal space one slot occupies in a row layout: the icon plus, when the
// durability numbers are on, the label drawn to its right.
inline float slotAdvance(const ArmorLayout& layout) {
    const float labelWidth = armorLabelWidth(layout);
    if (!layout.horizontal || labelWidth <= 0.0f) return layout.slotSize;
    return layout.slotSize + armorLabelGap(layout) + labelWidth;
}

// Position of one slot in the element.
inline SlotRect slotRect(const ArmorLayout& layout, std::size_t index) {
    SlotRect rect;
    if (index >= SlotCount) index = SlotCount - 1;
    rect.size = layout.slotSize;
    const float offset = (slotAdvance(layout) + layout.gap) * static_cast<float>(index);
    if (layout.horizontal) {
        rect.x = layout.x + offset;
        rect.y = layout.y;
    } else {
        rect.x = layout.x;
        rect.y = layout.y + (layout.slotSize + layout.gap) * static_cast<float>(index);
    }
    return rect;
}

// Size of the element including the durability labels drawn to the right of
// the icons, so the HUD editor box covers everything it owns.
inline float columnWidth(const ArmorLayout& layout) {
    if (layout.horizontal) {
        return slotAdvance(layout) * static_cast<float>(SlotCount) +
               layout.gap * static_cast<float>(SlotCount - 1);
    }
    const float labelWidth = armorLabelWidth(layout);
    if (labelWidth <= 0.0f) return layout.slotSize;
    return layout.slotSize + armorLabelGap(layout) + labelWidth;
}

inline float columnHeight(const ArmorLayout& layout) {
    if (layout.horizontal) return layout.slotSize;
    return layout.slotSize * static_cast<float>(SlotCount) +
           layout.gap * static_cast<float>(SlotCount - 1);
}

// Optional background behind the whole element: one hotbar-style strip around
// all slots (dark fill with a lighter border), instead of the per-slot cells.
// Like the vanilla hotbar sprite, the frame keeps one GUI pixel (a sixteenth
// of the slot size) of breathing room between it and the icons.

// Size of one vanilla GUI pixel at the current slot size.
inline float hotbarUnit(const ArmorLayout& layout) {
    return std::max(1.0f, layout.slotSize / 16.0f);
}

// Space between the slots and the inner edge of the border.
inline float hotbarPadding(const ArmorLayout& layout) {
    return hotbarUnit(layout);
}

// Thickness of the border drawn around the strip.
inline float hotbarBorder(const ArmorLayout& layout) {
    return hotbarUnit(layout);
}

// A rectangle in HUD units (the hotbar strip and the editor box use it).
struct HotbarRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

// Outer rectangle of the strip around the first `visibleSlots` slots (the
// offhand is hidden while it is disabled, and the strip follows). Nothing to
// wrap collapses to the anchor with no extent.
inline HotbarRect hotbarRect(const ArmorLayout& layout, std::size_t visibleSlots = SlotCount) {
    if (visibleSlots == 0 || layout.slotSize <= 0.0f) return {layout.x, layout.y, 0.0f, 0.0f};
    if (visibleSlots > SlotCount) visibleSlots = SlotCount;

    const SlotRect first = slotRect(layout, 0);
    const SlotRect last = slotRect(layout, visibleSlots - 1);
    const float slotsWidth = layout.horizontal ? last.x + last.size - first.x : first.size;
    const float slotsHeight = layout.horizontal ? first.size : last.y + last.size - first.y;

    const float frame = hotbarPadding(layout) + hotbarBorder(layout);
    return {first.x - frame, first.y - frame, slotsWidth + 2.0f * frame, slotsHeight + 2.0f * frame};
}

// The five rectangles the strip is painted with: the inner fill plus the four
// border edges, in top, bottom, left, right order.
struct HotbarFrame {
    HotbarRect fill;
    std::array<HotbarRect, 4> edges;
};

inline HotbarFrame hotbarFrame(const ArmorLayout& layout, std::size_t visibleSlots = SlotCount) {
    const HotbarRect outer = hotbarRect(layout, visibleSlots);
    const float border = hotbarBorder(layout);
    HotbarFrame frame;
    frame.fill = {outer.x + border, outer.y + border,
                  std::max(0.0f, outer.width - 2.0f * border),
                  std::max(0.0f, outer.height - 2.0f * border)};
    frame.edges[0] = {outer.x, outer.y, outer.width, border};                            // top
    frame.edges[1] = {outer.x, outer.y + outer.height - border, outer.width, border};    // bottom
    frame.edges[2] = {outer.x, outer.y + border, border, frame.fill.height};             // left
    frame.edges[3] = {outer.x + outer.width - border, outer.y + border, border, frame.fill.height}; // right
    return frame;
}

// Outer box the HUD editor should cover: the slots and their durability
// labels, grown to include the strip when it is drawn.
inline HotbarRect elementBounds(const ArmorLayout& layout, bool hotbarStrip,
                                std::size_t visibleSlots = SlotCount) {
    HotbarRect bounds{layout.x, layout.y, columnWidth(layout), columnHeight(layout)};
    if (hotbarStrip) {
        const HotbarRect strip = hotbarRect(layout, visibleSlots);
        const float x2 = std::max(bounds.x + bounds.width, strip.x + strip.width);
        const float y2 = std::max(bounds.y + bounds.height, strip.y + strip.height);
        bounds.x = std::min(bounds.x, strip.x);
        bounds.y = std::min(bounds.y, strip.y);
        bounds.width = std::max(0.0f, x2 - bounds.x);
        bounds.height = std::max(0.0f, y2 - bounds.y);
    }
    return bounds;
}

} // namespace bedrocktools::armorhud

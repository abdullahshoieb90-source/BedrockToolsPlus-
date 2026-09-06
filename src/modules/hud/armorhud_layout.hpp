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

} // namespace bedrocktools::armorhud

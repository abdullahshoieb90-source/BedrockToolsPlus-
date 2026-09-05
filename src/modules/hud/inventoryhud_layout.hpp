#pragma once

// Pure layout math for the Inventory HUD module.
//
// The module paints the 27 slots of the player's main inventory (container
// slots 9-35, the 9x3 grid of the inventory screen) as a grid anchored at a
// single HUD editor position, optionally with the armor + offhand column on
// its left like the vanilla inventory screen. Keeping the math header-only and
// free of Minecraft types lets the host unit tests cover it.

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace bedrocktools::inventoryhud {

inline constexpr std::size_t HotbarSlotCount = 9;
inline constexpr std::size_t FirstGridSlot = HotbarSlotCount; // container slot 9
inline constexpr std::size_t GridSlotCount = 27;              // container slots 9-35
inline constexpr std::size_t LastGridSlot = FirstGridSlot + GridSlotCount - 1;
inline constexpr std::size_t MinColumns = 1;
inline constexpr std::size_t MaxColumns = GridSlotCount;
inline constexpr std::size_t DefaultColumns = 9;

// Order of the optional equipment column: helmet, chestplate, leggings,
// boots, offhand.
inline constexpr std::size_t EquipmentSlotCount = 5;
inline constexpr std::size_t OffhandEquipmentIndex = 4;

struct GridLayout {
    float x = 0.0f;                   // anchor (top-left of the whole element), HUD units
    float y = 0.0f;
    float slotSize = 32.0f;           // width and height of one slot
    float gap = 4.0f;                 // space between two slots
    std::size_t columns = DefaultColumns;
    bool equipment = false;           // armor + offhand column on the left
};

struct SlotRect {
    float x = 0.0f;
    float y = 0.0f;
    float size = 0.0f;
};

inline std::size_t clampColumns(std::size_t columns) {
    return std::clamp(columns, MinColumns, MaxColumns);
}

// Rows needed to show every grid slot with the configured column count.
inline std::size_t rowCount(const GridLayout& layout) {
    const std::size_t columns = clampColumns(layout.columns);
    return (GridSlotCount + columns - 1) / columns;
}

// Container slot index of a grid cell (0-26 -> 9-35).
inline std::size_t containerSlot(std::size_t gridIndex) {
    if (gridIndex >= GridSlotCount) gridIndex = GridSlotCount - 1;
    return FirstGridSlot + gridIndex;
}

// Horizontal space between the equipment column and the grid.
inline float equipmentSeparator(const GridLayout& layout) {
    return layout.gap + layout.slotSize * 0.5f;
}

// Left edge of the grid; shifts right when the equipment column is shown.
inline float gridOriginX(const GridLayout& layout) {
    if (!layout.equipment) return layout.x;
    return layout.x + layout.slotSize + equipmentSeparator(layout);
}

// Position of one grid cell. Out-of-range indices are clamped so callers can
// never produce a rectangle outside the element.
inline SlotRect gridSlotRect(const GridLayout& layout, std::size_t gridIndex) {
    if (gridIndex >= GridSlotCount) gridIndex = GridSlotCount - 1;
    const std::size_t columns = clampColumns(layout.columns);
    const float step = layout.slotSize + layout.gap;
    SlotRect rect;
    rect.size = layout.slotSize;
    rect.x = gridOriginX(layout) + step * static_cast<float>(gridIndex % columns);
    rect.y = layout.y + step * static_cast<float>(gridIndex / columns);
    return rect;
}

// Position of one equipment slot; a zero-size rectangle when the column is
// hidden.
inline SlotRect equipmentSlotRect(const GridLayout& layout, std::size_t index) {
    SlotRect rect;
    if (!layout.equipment) return rect;
    if (index >= EquipmentSlotCount) index = EquipmentSlotCount - 1;
    const float step = layout.slotSize + layout.gap;
    rect.size = layout.slotSize;
    rect.x = layout.x;
    rect.y = layout.y + step * static_cast<float>(index);
    return rect;
}

inline float gridWidth(const GridLayout& layout) {
    const std::size_t columns = clampColumns(layout.columns);
    return layout.slotSize * static_cast<float>(columns) + layout.gap * static_cast<float>(columns - 1);
}

inline float gridHeight(const GridLayout& layout) {
    const std::size_t rows = rowCount(layout);
    return layout.slotSize * static_cast<float>(rows) + layout.gap * static_cast<float>(rows - 1);
}

// Size of the whole element (grid plus equipment column), used for the HUD
// editor box.
inline float layoutWidth(const GridLayout& layout) {
    const float grid = gridWidth(layout);
    if (!layout.equipment) return grid;
    return layout.slotSize + equipmentSeparator(layout) + grid;
}

inline float layoutHeight(const GridLayout& layout) {
    const float grid = gridHeight(layout);
    if (!layout.equipment) return grid;
    const float column = layout.slotSize * static_cast<float>(EquipmentSlotCount) +
                         layout.gap * static_cast<float>(EquipmentSlotCount - 1);
    return std::max(grid, column);
}

// ---- Per-slot decorations ---------------------------------------------------

// Vanilla draws the durability bar 2px from the left, 13px from the top, 13px
// wide and 2px tall (with a 1px fill) inside a 16px icon; this scales those
// proportions to the slot size.
struct DurabilityBar {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float fillWidth = 0.0f;
    float fillHeight = 0.0f;
};

inline float durabilityRatio(int damage, int maxDamage) {
    if (maxDamage <= 0) return 1.0f;
    const int safeDamage = std::clamp(damage, 0, maxDamage);
    return static_cast<float>(maxDamage - safeDamage) / static_cast<float>(maxDamage);
}

inline DurabilityBar durabilityBar(const SlotRect& slot, float remainingRatio) {
    const float ratio = std::clamp(remainingRatio, 0.0f, 1.0f);
    const float unit = slot.size / 16.0f;
    DurabilityBar bar;
    bar.x = slot.x + 2.0f * unit;
    bar.y = slot.y + 13.0f * unit;
    bar.width = 13.0f * unit;
    bar.height = std::max(1.0f, 2.0f * unit);
    bar.fillWidth = bar.width * ratio;
    bar.fillHeight = std::max(1.0f, unit);
    return bar;
}

// Green at full durability fading to red when almost broken (ARGB).
inline std::uint32_t durabilityColor(float remainingRatio) {
    const float ratio = std::clamp(remainingRatio, 0.0f, 1.0f);
    const auto red = static_cast<std::uint32_t>((1.0f - ratio) * 255.0f + 0.5f);
    const auto green = static_cast<std::uint32_t>(ratio * 255.0f + 0.5f);
    return 0xFF000000u | (red << 16) | (green << 8);
}

// Baseline anchor of the right-aligned stack count in a slot's bottom-right
// corner.
struct TextAnchor {
    float x = 0.0f;
    float y = 0.0f;
};

inline TextAnchor countTextAnchor(const SlotRect& slot) {
    const float unit = slot.size / 16.0f;
    TextAnchor anchor;
    anchor.x = slot.x + slot.size - unit;
    anchor.y = slot.y + slot.size - unit;
    return anchor;
}

} // namespace bedrocktools::inventoryhud

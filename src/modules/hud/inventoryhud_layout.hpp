#pragma once

// Pure layout math for the Inventory HUD module.
//
// The module paints the 27 slots of the player's main inventory (container
// slots 9-35, the 9x3 grid of the inventory screen) as item icons on the HUD,
// optionally together with the armor + offhand column. The grid and that
// column are two independent HUD editor elements, each with its own anchor, so
// they can be dragged and placed separately. Keeping the math header-only and
// free of Minecraft types lets the host unit tests cover it.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace bedrocktools::inventoryhud {

inline constexpr std::size_t HotbarSlotCount = 9;
inline constexpr std::size_t FirstGridSlot = HotbarSlotCount; // container slot 9
inline constexpr std::size_t GridSlotCount = 27;              // container slots 9-35
inline constexpr std::size_t LastGridSlot = FirstGridSlot + GridSlotCount - 1;
inline constexpr std::size_t MinColumns = 1;
inline constexpr std::size_t MaxColumns = GridSlotCount;
inline constexpr std::size_t DefaultColumns = 9;

// Order of the equipment column: helmet, chestplate, leggings, boots, offhand.
inline constexpr std::size_t EquipmentSlotCount = 5;
inline constexpr std::size_t OffhandEquipmentIndex = 4;

// A position of -1 means "the user never placed this element"; the anchor is
// then derived from the inventory grid (see equipmentAnchorBesideGrid).
inline constexpr float UnplacedPosition = -1.0f;

// Anchor and shape of the inventory grid element.
struct GridLayout {
    float x = 0.0f;                   // anchor (top-left of the grid), HUD units
    float y = 0.0f;
    float slotSize = 32.0f;           // width and height of one slot
    float gap = 4.0f;                 // space between two slots
    std::size_t columns = DefaultColumns;
};

// Anchor and shape of the armor + offhand element. It shares the slot size and
// gap settings with the grid, but has its own position so both elements can be
// moved independently in the HUD editor.
struct EquipmentLayout {
    float x = 0.0f;                   // anchor (top-left of the column), HUD units
    float y = 0.0f;
    float slotSize = 32.0f;
    float gap = 4.0f;
    float armorTextSize = 0.0f;        // 0 disables the durability labels beside armor
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

// ---- Inventory grid --------------------------------------------------------

// Position of one grid cell. Out-of-range indices are clamped so callers can
// never produce a rectangle outside the element.
inline SlotRect gridSlotRect(const GridLayout& layout, std::size_t gridIndex) {
    if (gridIndex >= GridSlotCount) gridIndex = GridSlotCount - 1;
    const std::size_t columns = clampColumns(layout.columns);
    const float step = layout.slotSize + layout.gap;
    SlotRect rect;
    rect.size = layout.slotSize;
    rect.x = layout.x + step * static_cast<float>(gridIndex % columns);
    rect.y = layout.y + step * static_cast<float>(gridIndex / columns);
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

// ---- Armor + offhand column ------------------------------------------------

// Keep labels inside their equipment row, even with tiny icons / large text.
inline float armorLabelTextSize(const EquipmentLayout& layout) {
    return std::clamp(layout.armorTextSize, 0.0f, std::max(0.0f, layout.slotSize));
}

inline float armorLabelWidth(const EquipmentLayout& layout) {
    // Item::getMaxDamage returns a short. Reserve enough room for 32767/32767
    // in the default font, without resizing the HUD as equipment wears down.
    return 7.0f * armorLabelTextSize(layout);
}

inline float armorLabelGap(const EquipmentLayout& layout) {
    return std::max(layout.gap, layout.slotSize / 8.0f);
}

// Position of one equipment slot in its own element.
inline SlotRect equipmentSlotRect(const EquipmentLayout& layout, std::size_t index) {
    SlotRect rect;
    if (index >= EquipmentSlotCount) index = EquipmentSlotCount - 1;
    const float step = layout.slotSize + layout.gap;
    rect.size = layout.slotSize;
    rect.x = layout.x;
    rect.y = layout.y + step * static_cast<float>(index);
    return rect;
}

// Width of the armor element including the durability labels drawn to the
// right of the icons, so the HUD editor box covers everything it owns.
inline float equipmentColumnWidth(const EquipmentLayout& layout) {
    const float labelWidth = armorLabelWidth(layout);
    if (labelWidth <= 0.0f) return layout.slotSize;
    return layout.slotSize + armorLabelGap(layout) + labelWidth;
}

inline float equipmentColumnHeight(const EquipmentLayout& layout) {
    return layout.slotSize * static_cast<float>(EquipmentSlotCount) +
           layout.gap * static_cast<float>(EquipmentSlotCount - 1);
}

// Horizontal space the armor column reserved between itself and the grid back
// when both still shared a single HUD editor element. The room needed to the
// right of the icons (label plus a gap on either side), or half a slot when the
// durability labels are off.
inline float legacyEquipmentSeparator(const EquipmentLayout& layout) {
    const float labelWidth = armorLabelWidth(layout);
    if (labelWidth > 0.0f) return labelWidth + 2.0f * armorLabelGap(layout);
    return layout.gap + layout.slotSize * 0.5f;
}

// Room the armor column takes to the left of the grid: one slot for the icons
// plus the separator above. That is also exactly how far a legacy
// single-anchor layout pushed the grid to the right of the column.
inline float equipmentClearance(const EquipmentLayout& layout) {
    return layout.slotSize + legacyEquipmentSeparator(layout);
}

// Anchor used while the armor column has no position of its own: beside the
// grid, on the left when the whole column fits there and on the right
// otherwise. The module stores the result as the column's own position, so this
// runs once - from then on the two elements are moved independently.
inline EquipmentLayout equipmentAnchorBesideGrid(const GridLayout& grid, const EquipmentLayout& equipment) {
    EquipmentLayout placed = equipment;
    const float offset = equipmentClearance(equipment);
    const float spacing = std::max(grid.gap, equipment.gap);
    placed.x = grid.x >= offset ? grid.x - offset : grid.x + gridWidth(grid) + spacing;
    placed.y = grid.y;
    return placed;
}

inline bool isPlaced(float position) {
    return position >= 0.0f;
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

inline int remainingDurability(int damage, int maxDamage) {
    if (maxDamage <= 0) return 0;
    return maxDamage - std::clamp(damage, 0, maxDamage);
}

inline std::string durabilityText(int damage, int maxDamage) {
    if (maxDamage <= 0) return {}; // empty / non-damageable equipment has no label
    return std::to_string(remainingDurability(damage, maxDamage)) + "/" + std::to_string(maxDamage);
}

inline float durabilityRatio(int damage, int maxDamage) {
    if (maxDamage <= 0) return 1.0f;
    return static_cast<float>(remainingDurability(damage, maxDamage)) / static_cast<float>(maxDamage);
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

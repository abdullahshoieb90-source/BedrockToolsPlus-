#pragma once

// Decorations every item-slot HUD module draws on top of an icon: the vanilla
// durability bar, the remaining/maximum durability text and the anchor of the
// stack count in a slot's bottom-right corner.
//
// Inventory HUD and the Armor module both paint square item slots, so this
// math is shared instead of duplicated. It stays header-only and free of
// Minecraft types so the host unit tests can cover it.

#include <algorithm>
#include <cstdint>
#include <string>

namespace bedrocktools::slotdecor {

struct SlotRect {
    float x = 0.0f;
    float y = 0.0f;
    float size = 0.0f;
};

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

// Size and baseline for the right-aligned stack count. The launcher text size
// is in HUD pixels, not slot pixels: cap it for small slots and reserve room
// for up to three digits (ItemStack count is a byte). Otherwise a large Number
// Text setting can place the count entirely outside the icon.
inline float countTextSize(const SlotRect& slot, float requestedSize, unsigned count) {
    const float inset = slot.size / 16.0f;
    const float available = std::max(0.0f, slot.size - 2.0f * inset);
    const unsigned digits = count >= 100 ? 3 : (count >= 10 ? 2 : 1);
    // The launcher's default font's digits are narrower than 0.7 em; keep a
    // small margin without requiring a platform-specific font measurement.
    const float widthLimit = available / (0.7f * static_cast<float>(digits));
    return std::max(0.0f, std::min({requestedSize, available, widthLimit}));
}

struct TextAnchor {
    float x = 0.0f;
    float y = 0.0f;
};

inline TextAnchor countTextAnchor(const SlotRect& slot, float textSize) {
    const float unit = slot.size / 16.0f;
    TextAnchor anchor;
    anchor.x = slot.x + slot.size - unit;
    // Android Canvas positions text by its baseline. Leave room for the
    // font's descent below it so the count doesn't slip below the icon.
    anchor.y = slot.y + slot.size - unit - std::max(0.0f, textSize) * 0.25f;
    return anchor;
}

} // namespace bedrocktools::slotdecor

// Host-side tests for the Inventory HUD grid layout.
//
//     g++ -std=c++20 -I src -I include tests/inventoryhud_layout_test.cpp -o /tmp/t && /tmp/t

#include "modules/hud/inventoryhud_layout.hpp"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void expectNear(const char* what, float actual, float expected) {
    if (std::fabs(actual - expected) > 0.001f) {
        std::printf("  FAIL %s: expected %.3f, got %.3f\n", what, expected, actual);
        ++failures;
    }
}

void expectEqual(const char* what, std::size_t actual, std::size_t expected) {
    if (actual != expected) {
        std::printf("  FAIL %s: expected %zu, got %zu\n", what, expected, actual);
        ++failures;
    }
}

void expectHex(const char* what, std::uint32_t actual, std::uint32_t expected) {
    if (actual != expected) {
        std::printf("  FAIL %s: expected 0x%08X, got 0x%08X\n", what, expected, actual);
        ++failures;
    }
}

} // namespace

int main() {
    using namespace bedrocktools::inventoryhud;

    // Container slot mapping: grid cell 0 is container slot 9 (the first slot
    // after the hotbar) and cell 26 is slot 35.
    expectEqual("first grid slot", containerSlot(0), 9);
    expectEqual("last grid slot", containerSlot(26), 35);
    expectEqual("last grid slot constant", LastGridSlot, 35);
    expectEqual("clamped grid slot", containerSlot(99), 35);

    GridLayout grid;
    grid.x = 100.0f;
    grid.y = 50.0f;
    grid.slotSize = 32.0f;
    grid.gap = 4.0f;
    grid.columns = 9;

    // 9 columns -> 3 rows, exactly the inventory screen.
    expectEqual("rows for 9 columns", rowCount(grid), 3);
    expectNear("cell 0 x", gridSlotRect(grid, 0).x, 100.0f);
    expectNear("cell 0 y", gridSlotRect(grid, 0).y, 50.0f);
    expectNear("cell 0 size", gridSlotRect(grid, 0).size, 32.0f);
    expectNear("cell 8 x (end of first row)", gridSlotRect(grid, 8).x, 100.0f + 8 * 36.0f);
    expectNear("cell 8 y", gridSlotRect(grid, 8).y, 50.0f);
    expectNear("cell 9 x (wraps to second row)", gridSlotRect(grid, 9).x, 100.0f);
    expectNear("cell 9 y", gridSlotRect(grid, 9).y, 50.0f + 36.0f);
    expectNear("cell 26 x", gridSlotRect(grid, 26).x, 100.0f + 8 * 36.0f);
    expectNear("cell 26 y", gridSlotRect(grid, 26).y, 50.0f + 2 * 36.0f);
    // Out-of-range indices clamp to the final cell instead of escaping the grid.
    expectNear("clamped cell x", gridSlotRect(grid, 99).x, gridSlotRect(grid, 26).x);
    expectNear("clamped cell y", gridSlotRect(grid, 99).y, gridSlotRect(grid, 26).y);

    expectNear("grid width", gridWidth(grid), 9 * 32.0f + 8 * 4.0f);
    expectNear("grid height", gridHeight(grid), 3 * 32.0f + 2 * 4.0f);
    expectNear("layout width without equipment", layoutWidth(grid), gridWidth(grid));
    expectNear("layout height without equipment", layoutHeight(grid), gridHeight(grid));
    expectNear("hidden equipment slot size", equipmentSlotRect(grid, 0).size, 0.0f);

    // Fewer columns wrap the 27 cells into more rows; the last row may be
    // partial.
    GridLayout narrow = grid;
    narrow.columns = 4;
    expectEqual("rows for 4 columns", rowCount(narrow), 7);
    expectNear("narrow cell 4 x", gridSlotRect(narrow, 4).x, 100.0f);
    expectNear("narrow cell 4 y", gridSlotRect(narrow, 4).y, 50.0f + 36.0f);
    expectNear("narrow cell 26 x (3rd column of last row)", gridSlotRect(narrow, 26).x, 100.0f + 2 * 36.0f);
    expectNear("narrow cell 26 y", gridSlotRect(narrow, 26).y, 50.0f + 6 * 36.0f);
    expectNear("narrow width", gridWidth(narrow), 4 * 32.0f + 3 * 4.0f);
    expectNear("narrow height", gridHeight(narrow), 7 * 32.0f + 6 * 4.0f);

    // Column count is clamped to 1..27.
    GridLayout single = grid;
    single.columns = 0;
    expectEqual("zero columns clamp to one", rowCount(single), 27);
    expectNear("single column cell 5 x", gridSlotRect(single, 5).x, 100.0f);
    expectNear("single column cell 5 y", gridSlotRect(single, 5).y, 50.0f + 5 * 36.0f);
    GridLayout wide = grid;
    wide.columns = 1000;
    expectEqual("huge column count clamps to 27", rowCount(wide), 1);
    expectNear("single row width", gridWidth(wide), 27 * 32.0f + 26 * 4.0f);

    // Gap of zero packs the slots edge to edge.
    GridLayout packed = grid;
    packed.gap = 0.0f;
    expectNear("packed cell 1 x", gridSlotRect(packed, 1).x, 132.0f);
    expectNear("packed width", gridWidth(packed), 9 * 32.0f);

    // Equipment column: five slots on the left, the grid shifts right by a
    // slot plus separator.
    GridLayout equipped = grid;
    equipped.equipment = true;
    const float separator = 4.0f + 16.0f;
    expectNear("equipment separator", equipmentSeparator(equipped), separator);
    expectNear("helmet x", equipmentSlotRect(equipped, 0).x, 100.0f);
    expectNear("helmet y", equipmentSlotRect(equipped, 0).y, 50.0f);
    expectNear("offhand y", equipmentSlotRect(equipped, OffhandEquipmentIndex).y, 50.0f + 4 * 36.0f);
    expectNear("clamped equipment index", equipmentSlotRect(equipped, 42).y, equipmentSlotRect(equipped, 4).y);
    expectNear("grid origin with equipment", gridOriginX(equipped), 100.0f + 32.0f + separator);
    expectNear("cell 0 x with equipment", gridSlotRect(equipped, 0).x, 100.0f + 32.0f + separator);
    expectNear("cell 0 y with equipment", gridSlotRect(equipped, 0).y, 50.0f);
    expectNear("layout width with equipment", layoutWidth(equipped), 32.0f + separator + gridWidth(grid));
    // Five equipment rows are taller than the three grid rows.
    expectNear("layout height with equipment", layoutHeight(equipped), 5 * 32.0f + 4 * 4.0f);
    GridLayout tallGrid = equipped;
    tallGrid.columns = 3; // 9 rows of grid
    expectNear("layout height when grid is taller", layoutHeight(tallGrid), 9 * 32.0f + 8 * 4.0f);

    // Durability bar proportions scale with the slot (vanilla: 2/13/13/2 in 16px).
    SlotRect slot;
    slot.x = 10.0f;
    slot.y = 20.0f;
    slot.size = 32.0f;
    expectNear("full durability ratio", durabilityRatio(0, 100), 1.0f);
    expectNear("half durability ratio", durabilityRatio(50, 100), 0.5f);
    expectNear("broken durability ratio", durabilityRatio(250, 100), 0.0f);
    expectNear("unbreakable durability ratio", durabilityRatio(5, 0), 1.0f);
    const DurabilityBar bar = durabilityBar(slot, 0.5f);
    expectNear("bar x", bar.x, 10.0f + 4.0f);
    expectNear("bar y", bar.y, 20.0f + 26.0f);
    expectNear("bar width", bar.width, 26.0f);
    expectNear("bar height", bar.height, 4.0f);
    expectNear("bar fill width", bar.fillWidth, 13.0f);
    expectNear("bar fill height", bar.fillHeight, 2.0f);
    SlotRect tiny = slot;
    tiny.size = 8.0f;
    expectNear("tiny bar keeps 1px fill", durabilityBar(tiny, 1.0f).fillHeight, 1.0f);
    expectNear("tiny bar keeps 1px height", durabilityBar(tiny, 1.0f).height, 1.0f);
    expectHex("durability color full", durabilityColor(1.0f), 0xFF00FF00u);
    expectHex("durability color empty", durabilityColor(0.0f), 0xFFFF0000u);
    expectHex("durability color half", durabilityColor(0.5f), 0xFF808000u);

    // Stack count sits in the bottom-right corner, one vanilla pixel in.
    const TextAnchor anchor = countTextAnchor(slot);
    expectNear("count anchor x", anchor.x, 10.0f + 32.0f - 2.0f);
    expectNear("count anchor y", anchor.y, 20.0f + 32.0f - 2.0f);

    if (failures == 0) {
        std::printf("inventoryhud_layout_test: all checks passed\n");
        return 0;
    }
    std::printf("inventoryhud_layout_test: %d check(s) failed\n", failures);
    return 1;
}

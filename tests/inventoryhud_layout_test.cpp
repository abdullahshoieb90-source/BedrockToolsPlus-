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

void expectText(const char* what, const std::string& actual, const std::string& expected) {
    if (actual != expected) {
        std::printf("  FAIL %s: expected '%s', got '%s'\n", what, expected.c_str(), actual.c_str());
        ++failures;
    }
}

void expectBool(const char* what, bool actual, bool expected) {
    if (actual != expected) {
        std::printf("  FAIL %s: expected %s, got %s\n", what, expected ? "true" : "false",
                    actual ? "true" : "false");
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
    // The grid element is exactly the grid: nothing is reserved for the armor
    // column any more, because that column is an element of its own.
    expectNear("cell 0 sits at the grid anchor", gridSlotRect(grid, 0).x, grid.x);

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

    // Armor + offhand column: five slots in its own element with its own
    // anchor, independent of the grid.
    EquipmentLayout armor;
    armor.x = 100.0f;
    armor.y = 50.0f;
    armor.slotSize = 32.0f;
    armor.gap = 4.0f;
    expectNear("helmet x", equipmentSlotRect(armor, 0).x, 100.0f);
    expectNear("helmet y", equipmentSlotRect(armor, 0).y, 50.0f);
    expectNear("helmet size", equipmentSlotRect(armor, 0).size, 32.0f);
    expectNear("boots y", equipmentSlotRect(armor, 3).y, 50.0f + 3 * 36.0f);
    expectNear("offhand y", equipmentSlotRect(armor, OffhandEquipmentIndex).y, 50.0f + 4 * 36.0f);
    expectNear("clamped equipment index", equipmentSlotRect(armor, 42).y, equipmentSlotRect(armor, 4).y);
    expectNear("column width without labels", equipmentColumnWidth(armor), 32.0f);
    expectNear("column height", equipmentColumnHeight(armor), 5 * 32.0f + 4 * 4.0f);

    // Moving one element never moves the other.
    EquipmentLayout movedArmor = armor;
    movedArmor.x = 640.0f;
    movedArmor.y = 12.0f;
    expectNear("moved helmet x", equipmentSlotRect(movedArmor, 0).x, 640.0f);
    expectNear("moved offhand y", equipmentSlotRect(movedArmor, OffhandEquipmentIndex).y, 12.0f + 4 * 36.0f);
    expectNear("grid ignores the moved column", gridSlotRect(grid, 0).x, 100.0f);
    expectNear("grid height ignores the column", gridHeight(grid), 3 * 32.0f + 2 * 4.0f);
    GridLayout movedGrid = grid;
    movedGrid.x = 500.0f;
    movedGrid.y = 300.0f;
    expectNear("moved cell 0 x", gridSlotRect(movedGrid, 0).x, 500.0f);
    expectNear("moved cell 0 y", gridSlotRect(movedGrid, 0).y, 300.0f);
    expectNear("column ignores the moved grid", equipmentSlotRect(armor, 0).x, 100.0f);
    expectNear("column ignores the moved grid vertically", equipmentSlotRect(armor, 0).y, 50.0f);

    // Armor labels reserve stable space inside the column's own editor box, so
    // the numbers do not get clipped. No reflow as durability drops.
    EquipmentLayout labeled = armor;
    labeled.armorTextSize = 12.0f;
    expectNear("armor text size", armorLabelTextSize(labeled), 12.0f);
    expectNear("armor label width", armorLabelWidth(labeled), 84.0f);
    expectNear("armor label gap", armorLabelGap(labeled), 4.0f);
    expectNear("labeled column width", equipmentColumnWidth(labeled), 32.0f + 4.0f + 84.0f);
    expectNear("labels keep column height", equipmentColumnHeight(labeled), equipmentColumnHeight(armor));
    expectNear("labels keep offhand position", equipmentSlotRect(labeled, OffhandEquipmentIndex).y,
               equipmentSlotRect(armor, OffhandEquipmentIndex).y);
    // Size extremes must not let text escape its row, even with zero slot gap.
    labeled.slotSize = 8.0f;
    labeled.gap = 0.0f;
    labeled.armorTextSize = 40.0f;
    expectNear("armor text clamped to tiny slot", armorLabelTextSize(labeled), 8.0f);
    expectNear("zero gap still separates armor text", armorLabelGap(labeled), 1.0f);
    expectNear("tiny labeled column width", equipmentColumnWidth(labeled), 8.0f + 1.0f + 7.0f * 8.0f);
    labeled.armorTextSize = 0.0f;
    expectNear("disabled labels reclaim space", equipmentColumnWidth(labeled), 8.0f);
    expectNear("disabled labels have no width", armorLabelWidth(labeled), 0.0f);

    // Old single-anchor configs: the column sat at the shared anchor and the
    // grid was drawn one slot plus a separator to its right. Migrating such a
    // config needs exactly that offset to keep the old picture.
    expectNear("legacy separator without labels", legacyEquipmentSeparator(armor), 4.0f + 16.0f);
    expectNear("clearance without labels", equipmentClearance(armor), 32.0f + 20.0f);
    EquipmentLayout legacyLabeled = armor;
    legacyLabeled.armorTextSize = 12.0f;
    expectNear("legacy separator with labels", legacyEquipmentSeparator(legacyLabeled), 92.0f);
    expectNear("clearance with labels", equipmentClearance(legacyLabeled), 124.0f);

    // A column that was never placed goes beside the grid: to the left when the
    // whole column fits there, to the right when the grid hugs the screen edge.
    GridLayout roomy = grid;
    roomy.x = 400.0f;
    const EquipmentLayout left = equipmentAnchorBesideGrid(roomy, legacyLabeled);
    expectNear("derived anchor left of the grid", left.x, 400.0f - 124.0f);
    expectNear("derived anchor keeps the grid row", left.y, roomy.y);
    expectNear("derived anchor keeps the column shape", left.slotSize, armor.slotSize);
    expectNear("derived anchor keeps the label style", armorLabelWidth(left), 84.0f);
    expectNear("derived column stops before the grid",
               left.x + equipmentColumnWidth(left) + armorLabelGap(left), roomy.x);
    GridLayout atEdge = grid;
    atEdge.x = 24.0f;
    const EquipmentLayout right = equipmentAnchorBesideGrid(atEdge, armor);
    expectNear("derived anchor right of an edge grid", right.x, 24.0f + gridWidth(atEdge) + 4.0f);
    expectNear("derived right anchor keeps the grid row", right.y, atEdge.y);
    GridLayout tightGap = atEdge;
    tightGap.gap = 10.0f;
    EquipmentLayout wideGap = armor;
    wideGap.gap = 2.0f;
    expectNear("derived spacing uses the wider gap",
               equipmentAnchorBesideGrid(tightGap, wideGap).x, 24.0f + gridWidth(tightGap) + 10.0f);

    // The sentinel that marks "the user never placed this element".
    expectNear("unplaced sentinel", UnplacedPosition, -1.0f);
    expectBool("sentinel is not placed", isPlaced(UnplacedPosition), false);
    expectBool("zero is a real position", isPlaced(0.0f), true);
    expectBool("positive is a real position", isPlaced(240.0f), true);

    // Numeric armor durability includes fully repaired and fully broken armor,
    // clamps invalid damage, and skips non-damageable items such as pumpkins.
    expectText("damaged armor", durabilityText(143, 363), "220/363");
    expectText("full armor", durabilityText(0, 528), "528/528");
    expectText("broken armor", durabilityText(363, 363), "0/363");
    expectText("excess damage", durabilityText(500, 363), "0/363");
    expectText("negative damage", durabilityText(-5, 363), "363/363");
    expectText("non-damageable armor", durabilityText(0, 0), "");
    expectText("invalid max damage", durabilityText(4, -1), "");
    expectText("largest supported max damage", durabilityText(0, 32767), "32767/32767");

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

// Host-side tests for the Armor module layout.
//
//     g++ -std=c++20 -I src -I include tests/armorhud_layout_test.cpp -o /tmp/t && /tmp/t

#include "modules/hud/armorhud_layout.hpp"

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

} // namespace

int main() {
    using namespace bedrocktools::armorhud;

    // Helmet, chestplate, leggings, boots, offhand - five slots in one column.
    ArmorLayout armor;
    armor.x = 100.0f;
    armor.y = 50.0f;
    armor.slotSize = 32.0f;
    armor.gap = 4.0f;
    expectNear("helmet x", slotRect(armor, 0).x, 100.0f);
    expectNear("helmet y", slotRect(armor, 0).y, 50.0f);
    expectNear("helmet size", slotRect(armor, 0).size, 32.0f);
    expectNear("boots y", slotRect(armor, 3).y, 50.0f + 3 * 36.0f);
    expectNear("offhand y", slotRect(armor, OffhandIndex).y, 50.0f + 4 * 36.0f);
    expectNear("clamped index", slotRect(armor, 42).y, slotRect(armor, 4).y);
    expectNear("column width without labels", columnWidth(armor), 32.0f);
    expectNear("column height", columnHeight(armor), 5 * 32.0f + 4 * 4.0f);

    // The element carries its own anchor: moving it moves nothing else.
    ArmorLayout moved = armor;
    moved.x = 640.0f;
    moved.y = 12.0f;
    expectNear("moved helmet x", slotRect(moved, 0).x, 640.0f);
    expectNear("moved offhand y", slotRect(moved, OffhandIndex).y, 12.0f + 4 * 36.0f);
    expectNear("original column untouched", slotRect(armor, 0).x, 100.0f);

    // Labels reserve stable space inside the editor box, so the numbers do not
    // get clipped and there is no reflow as durability drops.
    ArmorLayout labeled = armor;
    labeled.armorTextSize = 12.0f;
    expectNear("armor text size", armorLabelTextSize(labeled), 12.0f);
    expectNear("armor label width", armorLabelWidth(labeled), 84.0f);
    expectNear("armor label gap", armorLabelGap(labeled), 4.0f);
    expectNear("labeled column width", columnWidth(labeled), 32.0f + 4.0f + 84.0f);
    expectNear("labels keep column height", columnHeight(labeled), columnHeight(armor));
    expectNear("labels keep offhand position", slotRect(labeled, OffhandIndex).y,
               slotRect(armor, OffhandIndex).y);

    // Size extremes must not let text escape its row, even with zero slot gap.
    ArmorLayout tiny = labeled;
    tiny.slotSize = 8.0f;
    tiny.gap = 0.0f;
    tiny.armorTextSize = 40.0f;
    expectNear("armor text clamped to tiny slot", armorLabelTextSize(tiny), 8.0f);
    expectNear("zero gap still separates armor text", armorLabelGap(tiny), 1.0f);
    expectNear("tiny labeled column width", columnWidth(tiny), 8.0f + 1.0f + 7.0f * 8.0f);
    tiny.armorTextSize = 0.0f;
    expectNear("disabled labels reclaim space", columnWidth(tiny), 8.0f);
    expectNear("disabled labels have no width", armorLabelWidth(tiny), 0.0f);

    // Horizontal layout: the slots run to the right, and each one reserves the
    // room its durability label needs so labels never overlap the next icon.
    ArmorLayout row = armor;
    row.horizontal = true;
    expectNear("row helmet x", slotRect(row, 0).x, 100.0f);
    expectNear("row helmet y", slotRect(row, 0).y, 50.0f);
    expectNear("row offhand y stays", slotRect(row, OffhandIndex).y, 50.0f);
    expectNear("row offhand x", slotRect(row, OffhandIndex).x, 100.0f + 4 * 36.0f);
    expectNear("row width", columnWidth(row), 5 * 32.0f + 4 * 4.0f);
    expectNear("row height", columnHeight(row), 32.0f);
    ArmorLayout labeledRow = row;
    labeledRow.armorTextSize = 12.0f;
    expectNear("labeled row advance", slotAdvance(labeledRow), 32.0f + 4.0f + 84.0f);
    expectNear("labeled row second slot x", slotRect(labeledRow, 1).x, 100.0f + 120.0f + 4.0f);
    expectNear("labeled row width", columnWidth(labeledRow), 5 * 120.0f + 4 * 4.0f);
    expectNear("labeled row height", columnHeight(labeledRow), 32.0f);

    if (failures == 0) {
        std::printf("armorhud_layout_test: all checks passed\n");
        return 0;
    }
    std::printf("armorhud_layout_test: %d check(s) failed\n", failures);
    return 1;
}

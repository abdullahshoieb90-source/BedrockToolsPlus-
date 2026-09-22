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

    // Hotbar background: one strip around all slots. With 32 px slots one
    // vanilla GUI pixel is 2 px, so the frame sits 4 px (padding + border)
    // outside the slots on every side.
    expectNear("hotbar unit", hotbarUnit(armor), 2.0f);
    expectNear("hotbar padding", hotbarPadding(armor), 2.0f);
    expectNear("hotbar border", hotbarBorder(armor), 2.0f);
    HotbarRect bar = hotbarRect(armor);
    expectNear("strip x", bar.x, 96.0f);
    expectNear("strip y", bar.y, 46.0f);
    expectNear("strip width", bar.width, 40.0f);
    expectNear("strip height", bar.height, 5 * 32.0f + 4 * 4.0f + 8.0f);
    HotbarFrame frame = hotbarFrame(armor);
    expectNear("strip fill x", frame.fill.x, 98.0f);
    expectNear("strip fill y", frame.fill.y, 48.0f);
    expectNear("strip fill width", frame.fill.width, 36.0f);
    expectNear("strip fill height", frame.fill.height, 180.0f);
    expectNear("strip top edge", frame.edges[0].y, 46.0f);
    expectNear("strip top edge height", frame.edges[0].height, 2.0f);
    expectNear("strip bottom edge y", frame.edges[1].y, 228.0f);
    expectNear("strip left edge width", frame.edges[2].width, 2.0f);
    expectNear("strip left edge height", frame.edges[2].height, 180.0f);
    expectNear("strip right edge x", frame.edges[3].x, 134.0f);

    // A disabled offhand slot is not part of the strip, which then only wraps
    // the four armor pieces.
    bar = hotbarRect(armor, ArmorSlotCount);
    expectNear("offhand-free strip height", bar.height, 4 * 32.0f + 3 * 4.0f + 8.0f);
    expectNear("offhand-free strip width", bar.width, 40.0f);

    // Tiny slots keep at least a 1 px frame so the strip stays visible.
    expectNear("tiny hotbar unit", hotbarUnit(tiny), 1.0f);
    bar = hotbarRect(tiny);
    expectNear("tiny strip x", bar.x, 98.0f);
    expectNear("tiny strip width", bar.width, 8.0f + 4.0f);

    // Horizontal layouts wrap the row, labels included (they sit between the
    // icons in that orientation).
    bar = hotbarRect(row);
    expectNear("row strip width", bar.width, 5 * 32.0f + 4 * 4.0f + 8.0f);
    expectNear("row strip height", bar.height, 40.0f);
    bar = hotbarRect(labeledRow);
    // The strip hugs the icons plus the labels between them; it does not
    // include the label reserve behind the last icon (nothing is drawn there).
    expectNear("labeled row strip width", bar.width, 4 * 124.0f + 32.0f + 8.0f);
    expectNear("labeled row strip height", bar.height, 40.0f);

    // The editor box covers slots, labels and — when drawn — the strip.
    HotbarRect bounds = elementBounds(labeled, false);
    expectNear("editor bounds without strip x", bounds.x, 100.0f);
    expectNear("editor bounds without strip width", bounds.width, 32.0f + 4.0f + 84.0f);
    expectNear("editor bounds without strip height", bounds.height, 5 * 32.0f + 4 * 4.0f);
    bounds = elementBounds(labeled, true);
    expectNear("editor bounds with strip x", bounds.x, 96.0f);
    expectNear("editor bounds with strip width", bounds.width, (32.0f + 4.0f + 84.0f) + 4.0f);
    expectNear("editor bounds with strip height", bounds.height, 5 * 32.0f + 4 * 4.0f + 8.0f);
    bounds = elementBounds(labeledRow, true);
    // Column width (with the trailing label reserve) and strip width union.
    expectNear("labeled row editor bounds width", bounds.width, 5 * 120.0f + 4 * 4.0f + 4.0f);
    expectNear("labeled row editor bounds height", bounds.height, 40.0f);

    if (failures == 0) {
        std::printf("armorhud_layout_test: all checks passed\n");
        return 0;
    }
    std::printf("armorhud_layout_test: %d check(s) failed\n", failures);
    return 1;
}

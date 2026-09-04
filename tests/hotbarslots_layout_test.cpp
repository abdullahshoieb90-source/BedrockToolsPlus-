// Host-side tests for the Hotbar Slots strip layout.
//
//     g++ -std=c++20 -I src -I include tests/hotbarslots_layout_test.cpp -o /tmp/t && /tmp/t

#include "modules/hud/hotbarslots_layout.hpp"

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
    using namespace bedrocktools::hotbar;

    StripLayout horizontal;
    horizontal.x = 100.0f;
    horizontal.y = 50.0f;
    horizontal.slotSize = 32.0f;
    horizontal.gap = 4.0f;

    expectNear("first slot x", slotRect(horizontal, 0).x, 100.0f);
    expectNear("first slot y", slotRect(horizontal, 0).y, 50.0f);
    expectNear("third slot x", slotRect(horizontal, 2).x, 100.0f + 2 * 36.0f);
    expectNear("third slot y", slotRect(horizontal, 2).y, 50.0f);
    expectNear("last slot x", slotRect(horizontal, SlotCount - 1).x, 100.0f + 8 * 36.0f);
    // Out-of-range indices clamp to the final slot instead of escaping the strip.
    expectNear("clamped slot x", slotRect(horizontal, 99).x, slotRect(horizontal, SlotCount - 1).x);

    expectNear("strip width", stripWidth(horizontal), 9 * 32.0f + 8 * 4.0f);
    expectNear("strip height", stripHeight(horizontal), 32.0f);
    expectNear("empty strip width", stripWidth(horizontal, 0), 0.0f);
    expectNear("single slot width", stripWidth(horizontal, 1), 32.0f);

    StripLayout vertical = horizontal;
    vertical.vertical = true;
    expectNear("vertical slot x", slotRect(vertical, 3).x, 100.0f);
    expectNear("vertical slot y", slotRect(vertical, 3).y, 50.0f + 3 * 36.0f);
    expectNear("vertical strip width", stripWidth(vertical), 32.0f);
    expectNear("vertical strip height", stripHeight(vertical), 9 * 32.0f + 8 * 4.0f);

    if (failures == 0) {
        std::printf("hotbarslots_layout_test: all checks passed\n");
        return 0;
    }
    std::printf("hotbarslots_layout_test: %d check(s) failed\n", failures);
    return 1;
}

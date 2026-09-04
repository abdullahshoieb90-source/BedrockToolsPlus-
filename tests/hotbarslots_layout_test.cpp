// Host-side tests for the Hotbar Slots strip layout and button-icon math.
//
//     g++ -std=c++20 -I src -I include tests/hotbarslots_layout_test.cpp -o /tmp/t && /tmp/t

#include "modules/hud/hotbarslots_layout.hpp"

#include <cmath>
#include <cstdio>
#include <string_view>

namespace {

int failures = 0;

void expectNear(const char* what, float actual, float expected) {
    if (std::fabs(actual - expected) > 0.001f) {
        std::printf("  FAIL %s: expected %.3f, got %.3f\n", what, expected, actual);
        ++failures;
    }
}

void expectIndex(const char* what, int actual, int expected) {
    if (actual != expected) {
        std::printf("  FAIL %s: expected %d, got %d\n", what, expected, actual);
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

    // Icons painted inside the on-screen buttons ("Draw Icons On Buttons").
    ButtonIconRect icon = iconRectInButton(100.0f, 200.0f, 96.0f, 96.0f);
    expectNear("button icon x", icon.x, 100.0f + 96.0f * 0.15f);
    expectNear("button icon y", icon.y, 200.0f + 96.0f * 0.15f);
    expectNear("button icon width", icon.width, 96.0f * 0.7f);
    expectNear("button icon height", icon.height, 96.0f * 0.7f);

    ButtonIconRect wide = iconRectInButton(0.0f, 0.0f, 120.0f, 60.0f, 0.1f);
    expectNear("wide button icon x", wide.x, 12.0f);
    expectNear("wide button icon y", wide.y, 6.0f);
    expectNear("wide button icon width", wide.width, 120.0f * 0.8f);
    expectNear("wide button icon height", wide.height, 60.0f * 0.8f);

    // Padding is clamped so the icon rectangle can never invert.
    ButtonIconRect clamped = iconRectInButton(0.0f, 0.0f, 100.0f, 100.0f, 0.9f);
    expectNear("overlarge padding clamps", clamped.width, 100.0f * 0.02f);
    ButtonIconRect negative = iconRectInButton(0.0f, 0.0f, 100.0f, 100.0f, -1.0f);
    expectNear("negative padding clamps", negative.width, 100.0f);

    // Overlay button ids map back to their slot index.
    constexpr std::string_view prefix = "bedrocktoolsplus.HotbarSlots.Button";
    expectIndex("first button id",
                slotIndexFromButtonId("bedrocktoolsplus.HotbarSlots.Button1", prefix), 0);
    expectIndex("last button id",
                slotIndexFromButtonId("bedrocktoolsplus.HotbarSlots.Button9", prefix), 8);
    expectIndex("slot zero rejected",
                slotIndexFromButtonId("bedrocktoolsplus.HotbarSlots.Button0", prefix), -1);
    expectIndex("two-digit id rejected",
                slotIndexFromButtonId("bedrocktoolsplus.HotbarSlots.Button10", prefix), -1);
    expectIndex("foreign module rejected",
                slotIndexFromButtonId("bedrocktoolsplus.Zoom.Button1", prefix), -1);
    expectIndex("empty id rejected", slotIndexFromButtonId("", prefix), -1);
    expectIndex("prefix alone rejected", slotIndexFromButtonId(prefix, prefix), -1);

    if (failures == 0) {
        std::printf("hotbarslots_layout_test: all checks passed\n");
        return 0;
    }
    std::printf("hotbarslots_layout_test: %d check(s) failed\n", failures);
    return 1;
}

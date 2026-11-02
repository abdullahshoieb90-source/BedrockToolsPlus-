// Regression test for Hitbox first-person hiding.
//
// Jumping in first person interpolates the camera above the tick AABB, so a
// tight inside-box test used to treat the player as third-person and flash
// their own hitbox. The helper in hitbox_camera.hpp must keep that case as
// first person while still recognizing a real third-person camera.
//
//     g++ -std=c++20 -I src tests/hitbox_camera_test.cpp -o /tmp/hitbox_camera_test
//     /tmp/hitbox_camera_test

#include "modules/visual/hitbox_camera.hpp"

#include <cstdio>
#include <string>

namespace {
int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}
} // namespace

int main() {
    std::printf("hitbox first-person camera detection\n");

    // Standing player collision box: 0.6 x 1.8 x 0.6, origin at the feet.
    const float x0 = 0.0f, y0 = 0.0f, z0 = 0.0f;
    const float x1 = 0.6f, y1 = 1.8f, z1 = 0.6f;

    auto third = [&](float cx, float cy, float cz) {
        return hitbox::isThirdPersonCamera(cx, cy, cz, x0, y0, z0, x1, y1, z1);
    };
    auto draw = [&](bool show, bool gameThird, bool camThird) {
        return hitbox::shouldDrawLocalHitbox(show, gameThird, camThird);
    };

    check(!third(0.3f, 1.62f, 0.3f), "standing first-person eye is inside the box");
    check(!third(0.3f, 1.27f, 0.3f), "sneaking first-person eye is inside the (shorter) box");
    check(!third(0.62f, 1.62f, 0.3f), "camera just past the XZ edge stays first-person");

    // The original 0.05 Y margin failed here: one tick of jump velocity
    // (0.42) plus view-bob puts the camera above aabb.max.y.
    check(!third(0.3f, 1.8f + 0.42f, 0.3f),
          "jump interpolation (camera 0.42 above AABB) stays first-person");
    check(!third(0.3f, 1.8f + 0.70f, 0.3f),
          "jump + view bob (camera 0.70 above AABB) stays first-person");
    check(!third(0.3f, 1.8f + 1.20f, 0.3f),
          "near the Y margin still counts as first-person");

    check(third(0.3f, 2.4f, 3.0f), "camera pulled back is third-person");
    check(third(2.0f, 3.0f, 0.3f), "camera off to the side is third-person");
    check(third(0.3f, 1.62f + 4.0f, 0.3f),
          "third-person looking straight down (camera far above) is third-person");

    std::printf("local hitbox draw gate\n");

    check(!draw(false, true, true), "menu off: never draw self");
    check(!draw(true, false, true), "first-person game mode: never draw self");
    check(!draw(true, true, false), "wall-clipped camera still inside the box: do not fill the view");
    check(draw(true, true, true), "third person with camera pulled away: draw self");
    check(!draw(true, false, third(0.3f, 1.8f + 0.42f, 0.3f)),
          "jumping in first person does not draw the local hitbox");
    check(!draw(true, true, third(0.3f, 1.8f + 0.42f, 0.3f)),
          "even if the game mode were unknown/third, jump geometry still hides self");

    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all hitbox camera checks passed\n");
    return 0;
}

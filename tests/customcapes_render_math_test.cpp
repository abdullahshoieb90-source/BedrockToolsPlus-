// Regression test for the Custom Capes self-rendered overlay geometry.
//
// Exercises the pure math in src/modules/player/customcapes_render_math.hpp
// (cape quad corners, UV mapping, hem sway, third-person camera test). The
// header is plain C++ with no Minecraft or Android dependencies, so this
// test compiles and runs on the host with any C++20 compiler.
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src -I include
//         tests/customcapes_render_math_test.cpp -o /tmp/customcapes_render_math_test
//     /tmp/customcapes_render_math_test

#include "modules/player/customcapes_render_math.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace render = customcapes::render;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

bool nearly(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

bool nearlyV(const render::Vec3& a, const render::Vec3& b, float eps = 1e-4f) {
    return nearly(a.x, b.x, eps) && nearly(a.y, b.y, eps) && nearly(a.z, b.z, eps);
}

// Cape center of the six corners (average), used to anchor the checks below.
render::Vec3 capeCenter(const render::CapeVertex corners[6]) {
    render::Vec3 c{0, 0, 0};
    for (int i = 0; i < 6; ++i) {
        c = c + corners[i].pos;
    }
    return c * (1.0f / 6.0f);
}

} // namespace

int main() {
    std::printf("camera test\n");
    // Camera inside the player's head (first person) -> hidden.
    check(!render::isThirdPersonCamera(0.0f, 1.62f, 0.0f,
                                       -0.3f, 0.0f, -0.3f, 0.3f, 1.8f, 0.3f),
          "camera inside the AABB (first person) -> no cape");
    // Camera pulled back behind the player -> visible.
    check(render::isThirdPersonCamera(0.0f, 1.62f, 5.0f,
                                      -0.3f, 0.0f, -0.3f, 0.3f, 1.8f, 0.3f),
          "camera outside the AABB (third person) -> cape shows");
    // Jitter at the box edge is absorbed by the margin.
    check(!render::isThirdPersonCamera(0.29f, 1.62f, 0.0f,
                                       -0.3f, 0.0f, -0.3f, 0.3f, 1.8f, 0.3f),
          "camera on the AABB edge stays hidden (margin)");

    std::printf("cape canvas UVs\n");
    check(render::kCapeU0 > 0.0f && render::kCapeU0 < 1.0f &&
              render::kCapeU1 > render::kCapeU0 && render::kCapeU1 <= 1.0f,
          "u range inside the canvas and ordered");
    check(render::kCapeV0 > 0.0f && render::kCapeV0 < 1.0f &&
              render::kCapeV1 > render::kCapeV0 && render::kCapeV1 <= 1.0f,
          "v range inside the canvas and ordered");
    // The visible face is x=1..11, y=1..17 of 64x32 with a half-texel inset.
    check(nearly(render::kCapeU0, 1.5f / 64.0f) && nearly(render::kCapeU1, 10.5f / 64.0f),
          "u maps the 10 px wide outer face (x=1..11)");
    check(nearly(render::kCapeV0, 1.5f / 32.0f) && nearly(render::kCapeV1, 16.5f / 32.0f),
          "v maps the 16 px tall outer face (y=1..17)");

    std::printf("cape quad geometry (no sway)\n");
    render::CapeVertex corners[6];
    render::buildCapeQuad(0.0f, 0.0f, 0.0f, 0.0f /*yaw*/, 0.0f /*phase*/, corners);

    // Yaw 0: right = (-1,0,0), back = (0,0,-1) -> the cape hangs on -z.
    check(nearly(corners[0].pos.y, render::kCapeTopHeight) &&
              nearly(corners[1].pos.y, render::kCapeTopHeight),
          "top corners at shoulder height");
    check(nearly(corners[4].pos.y, render::kCapeTopHeight - render::kCapeHeightBlocks) &&
              nearly(corners[5].pos.y, render::kCapeTopHeight - render::kCapeHeightBlocks),
          "hem corners at shoulder height minus the 1-block cape height");
    check(nearly(corners[0].pos.z, -render::kCapeBackOffset) &&
              nearly(corners[4].pos.z, -render::kCapeBackOffset),
          "cape hangs behind the back (-z for yaw 0) at the back offset");
    // Yaw 0 -> right = (-1,0,0), so TR/MR sit at -x and TL/ML at +x; the
    // total width is the 10/16 block cape width.
    check(nearly(std::fabs(corners[1].pos.x - corners[0].pos.x), render::kCapeWidthBlocks),
          "cape spans the full 10/16 block width");
    check(nearly(corners[2].pos.x, corners[1].pos.x) &&
              nearly(corners[3].pos.x, corners[0].pos.x) &&
              nearly(corners[4].pos.x, corners[3].pos.x) &&
              nearly(corners[5].pos.x, corners[2].pos.x),
          "lower segment stays aligned under the upper segment (no sway)");
    check(corners[3].pos.y > corners[4].pos.y && corners[2].pos.y > corners[5].pos.y,
          "hinge sits above the hem");

    // The six corners share one plane when idle: x spread is symmetric and
    // the width of the lower segment matches the upper segment.
    check(nearly(std::fabs(corners[5].pos.x - corners[4].pos.x), render::kCapeWidthBlocks),
          "lower segment has the same width as the upper segment");

    std::printf("cape quad UVs\n");
    check(nearly(corners[0].u, render::kCapeU0) && nearly(corners[0].v, render::kCapeV0) &&
              nearly(corners[1].u, render::kCapeU1) && nearly(corners[1].v, render::kCapeV0),
          "upper segment covers the top rows of the face");
    check(nearly(corners[4].v, render::kCapeV1) && nearly(corners[5].v, render::kCapeV1) &&
              corners[4].u == corners[0].u && corners[5].u == corners[1].u,
          "lower segment covers the bottom rows with the same u span");
    check(corners[2].v == corners[3].v && corners[2].v > render::kCapeV0 &&
              corners[2].v < render::kCapeV1,
          "hinge UV splits the face halfway");

    std::printf("cape quad sway\n");
    // At phase pi/2 (sin = 1) the hem swings backwards (further -z for yaw 0,
    // i.e. further away from the player's back).
    render::CapeVertex sway[6];
    render::buildCapeQuad(0.0f, 0.0f, 0.0f, 0.0f, 3.14159265f * 0.5f, sway);
    check(sway[4].pos.z < corners[4].pos.z - 0.02f && sway[5].pos.z < corners[5].pos.z - 0.02f,
          "hem sways backwards at the peak of the phase");
    // The rotation axis is the player's right vector, so the hem's x
    // coordinate (the width direction) is invariant.
    check(nearly(sway[4].pos.x, corners[4].pos.x) && nearly(sway[5].pos.x, corners[5].pos.x),
          "sway rotates around the right axis (x coordinate invariant)");
    check(nearlyV(sway[2].pos, corners[2].pos) && nearlyV(sway[3].pos, corners[3].pos),
          "hinge corners stay fixed while the hem swings");
    check(nearly(std::fabs(sway[5].pos.x - sway[4].pos.x), render::kCapeWidthBlocks),
          "sway preserves the hem width");

    // Sway amplitude is bounded: even at the peak the hem stays within a
    // sensible arc (9 degrees over a 0.5-block segment ~ 0.08 blocks).
    const float hemSwing = std::fabs(sway[4].pos.z - corners[4].pos.z);
    check(hemSwing < 0.15f,
          "sway amplitude stays subtle (9 degrees)");

    // Mirror symmetry: at yaw 180 the back direction flips to +z.
    render::CapeVertex turned[6];
    render::buildCapeQuad(0.0f, 0.0f, 0.0f, 180.0f, 0.0f, turned);
    check(turned[0].pos.z > 0.0f && nearly(turned[0].pos.z, render::kCapeBackOffset),
          "yaw 180 puts the cape on +z (player turned around)");

    // A different feet position translates the whole cape.
    render::CapeVertex moved[6];
    render::buildCapeQuad(10.0f, 5.0f, -3.0f, 0.0f, 0.0f, moved);
    const render::Vec3 movedCenter = capeCenter(moved);
    const render::Vec3 baseCenter = capeCenter(corners);
    check(nearly(movedCenter.x - baseCenter.x, 10.0f) &&
              nearly(movedCenter.y - baseCenter.y, 5.0f) &&
              nearly(movedCenter.z - baseCenter.z, -3.0f),
          "feet position translates the cape quad");

    std::printf("rotation helper\n");
    // Rotating around an axis parallel to the point vector leaves it unchanged.
    const render::Vec3 axis{0.0f, 1.0f, 0.0f};
    const render::Vec3 pivot{1.0f, 0.0f, 1.0f};
    const render::Vec3 onAxis = pivot + axis * 2.0f;
    check(nearlyV(render::rotateAroundAxis(onAxis, pivot, axis, 1.234f), onAxis),
          "points on the axis are invariant");
    // 90 degrees around Y maps +X to -Z.
    const render::Vec3 rotated = render::rotateAroundAxis({2.0f, 0.0f, 1.0f},
                                                          {1.0f, 0.0f, 1.0f},
                                                          axis, 3.14159265f * 0.5f);
    check(nearly(rotated.x, 1.0f) && nearly(rotated.y, 0.0f) && nearly(rotated.z, 0.0f),
          "90 degree rotation around Y turns +X into -Z");

    std::printf("\n%s\n", g_failures == 0 ? "all custom capes render math checks passed"
                                          : "SOME RENDER MATH CHECKS FAILED");
    return g_failures == 0 ? 0 : 1;
}

// Host test for the Hitbox module's screen-space projection (the HUD fallback).
//
// The projection is what puts a box on top of an entity, so the conventions
// matter: a target dead ahead has to land in the middle of the surface, one to
// the east has to land right of centre when the camera faces south, and an edge
// that crosses the camera plane has to be clipped instead of wrapping around
// the screen.
//
// Build: g++ -std=c++20 -I include src/modules/visual/hitbox_projection.hpp
//        tests/hitbox_projection_test.cpp -o /tmp/hitbox_projection_test
// Run:   /tmp/hitbox_projection_test

#include "modules/visual/hitbox_projection.hpp"

#include <cmath>
#include <cstdio>
#include <string>

using bedrocktools::sdk::AABB;
using bedrocktools::sdk::Vec2;
using bedrocktools::sdk::Vec3;
namespace proj = hitboxhud;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) {
        std::printf("  ok   %s\n", message.c_str());
    } else {
        std::printf("  FAIL %s\n", message.c_str());
        ++g_failures;
    }
}

bool near(float a, float b, float epsilon = 0.01f) { return std::fabs(a - b) <= epsilon; }

const proj::Projection kProjection = proj::makeProjection(800.0f, 450.0f, 70.0f);

float xAt(const proj::Camera& cam, const Vec3& point) {
    float x = 0.0f;
    float y = 0.0f;
    proj::project(cam, kProjection, point, x, y);
    return x;
}

float yAt(const proj::Camera& cam, const Vec3& point) {
    float x = 0.0f;
    float y = 0.0f;
    proj::project(cam, kProjection, point, x, y);
    return y;
}

} // namespace

int main() {
    std::printf("hitbox screen projection\n");

    // Yaw 0 faces +Z (south); the camera sits at the origin looking south.
    const proj::Camera south = proj::computeCamera({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f});
    check(near(south.forward.x, 0.0f) && near(south.forward.z, 1.0f),
          "yaw 0 looks towards +Z");

    float x = 0.0f;
    float y = 0.0f;
    check(proj::project(south, kProjection, {0.0f, 0.0f, 10.0f}, x, y) &&
              near(x, 400.0f) && near(y, 225.0f),
          "a target dead ahead lands in the middle of the surface");

    // Standing at yaw 0 (facing south, +Z), the player's right hand points west
    // (-X): facing north, east is on the right, so facing south it is on the
    // left. The camera basis follows the same rule, which is what keeps the
    // overlay on the correct side of the screen.
    const float eastX = xAt(south, {10.0f, 0.0f, 10.0f});
    const float westX = xAt(south, {-10.0f, 0.0f, 10.0f});
    check(eastX < 400.0f && westX > 400.0f, "+X (east) is left of a south-facing camera");
    check(near(400.0f - eastX, westX - 400.0f, 1.0f), "the projection is symmetric around the centre");

    // Up is +Y, and the surface's y grows downwards. Both points stay in front
    // of the camera plane, which is where projection is defined at all.
    check(yAt(south, {0.0f, 10.0f, 10.0f}) < 225.0f, "+Y is above the centre");
    check(yAt(south, {0.0f, -10.0f, 10.0f}) > 225.0f, "-Y is below the centre");

    // Yaw -90 faces +X, and then a target to the east is dead ahead.
    const proj::Camera east = proj::computeCamera({0.0f, 0.0f, 0.0f}, {0.0f, -90.0f});
    check(near(east.forward.x, 1.0f) && near(east.forward.z, 0.0f), "yaw -90 looks towards +X");
    check(proj::project(east, kProjection, {10.0f, 0.0f, 0.0f}, x, y) &&
              near(x, 400.0f) && near(y, 225.0f),
          "the same target is centred once the camera faces it");

    // Pitch drives the vertical axis, and +/-90 stays well defined.
    const proj::Camera up = proj::computeCamera({0.0f, 0.0f, 0.0f}, {-90.0f, 0.0f});
    check(near(up.forward.y, 1.0f), "pitch -90 looks straight up");
    check(proj::project(up, kProjection, {0.0f, 10.0f, 0.0f}, x, y) && near(y, 225.0f),
          "looking straight up keeps the projection well defined");

    // A point behind the camera cannot be projected.
    check(!proj::project(south, kProjection, {0.0f, 0.0f, -10.0f}, x, y),
          "a point behind the camera is rejected");

    // ---- segments ----------------------------------------------------------
    proj::Segment segment;
    check(proj::projectSegment(south, kProjection, {0.0f, -1.0f, 5.0f}, {0.0f, 1.0f, 5.0f},
                               0.0f, segment) &&
              near(segment.x1, 400.0f) && near(segment.x2, 400.0f) &&
              segment.y1 > segment.y2,
          "a vertical edge projects to a vertical on-screen segment");

    check(!proj::projectSegment(south, kProjection, {0.0f, 0.0f, -5.0f}, {0.0f, 1.0f, -5.0f},
                                0.0f, segment),
          "an edge fully behind the camera is dropped");

    // An edge running through the camera plane keeps its visible half instead
    // of wrapping around the screen: the end behind the camera is pulled onto
    // the plane, so both endpoints project to sensible coordinates.
    check(proj::projectSegment(south, kProjection, {0.0f, -1.0f, -5.0f}, {0.0f, -1.0f, 5.0f},
                               0.0f, segment) &&
              std::isfinite(segment.x1) && std::isfinite(segment.y1) &&
              std::isfinite(segment.x2) && std::isfinite(segment.y2),
          "an edge crossing the camera plane is clipped to a finite segment");
    // The end pulled onto the plane is still finite (a plain divide by a
    // near-zero depth would not be), which is the whole point of the clip.
    check(segment.y1 > 225.0f && segment.y2 > 225.0f && std::fabs(segment.y1) < 1.0e6f,
          "the clipped end is finite and stays below the centre");

    // Clamping keeps an entity that straddles the camera from producing
    // coordinates that the overlay cannot represent.
    proj::projectSegment(south, kProjection, {0.0f, 0.0f, 0.01f}, {0.0f, 0.0f, 5.0f}, 3200.0f, segment);
    check(segment.x1 <= 800.0f + 3200.0f && segment.x1 >= -3200.0f,
          "surface coordinates are clamped, not unbounded");

    // ---- boxes -------------------------------------------------------------
    const AABB box{{-0.3f, -0.9f, 8.0f}, {0.3f, 0.9f, 8.6f}};
    std::vector<proj::Segment> segments;
    proj::projectBox(south, kProjection, box.min, box.max, segments);
    check(segments.size() == 12, "a box in front of the camera projects all twelve edges");

    float minX = 1e30f;
    float maxX = -1e30f;
    float minY = 1e30f;
    float maxY = -1e30f;
    for (const auto& s : segments) {
        minX = std::min({minX, s.x1, s.x2});
        maxX = std::max({maxX, s.x1, s.x2});
        minY = std::min({minY, s.y1, s.y2});
        maxY = std::max({maxY, s.y1, s.y2});
    }
    check(minX < 400.0f && maxX > 400.0f && minY < 225.0f && maxY > 225.0f,
          "the projected box is centred on the surface");
    check(maxX - minX > 0.0f && maxY - minY > 0.0f, "the projected box has a real extent");

    const AABB behind{{-0.3f, -0.9f, -9.0f}, {0.3f, 0.9f, -8.4f}};
    segments.clear();
    proj::projectBox(south, kProjection, behind.min, behind.max, segments);
    check(segments.empty(), "a box behind the camera projects nothing");

    // ---- projection parameters --------------------------------------------
    const proj::Projection narrow = proj::makeProjection(800.0f, 450.0f, 30.0f);
    const proj::Projection wide = proj::makeProjection(800.0f, 450.0f, 120.0f);
    check(narrow.tanHalfFov < wide.tanHalfFov, "a wider field of view has a wider tangent");
    check(near(proj::makeProjection(800.0f, 450.0f, 1000.0f).tanHalfFov, wide.tanHalfFov),
          "an out-of-range field of view is clamped");
    check(near(proj::makeProjection(800.0f, 450.0f, 70.0f).aspect, 800.0f / 450.0f),
          "the aspect ratio follows the surface");

    const proj::Camera degenerate = proj::computeCamera({0.0f, 0.0f, 0.0f}, {90.0f, 0.0f});
    check(near(std::sqrt(degenerate.right.x * degenerate.right.x +
                         degenerate.right.y * degenerate.right.y +
                         degenerate.right.z * degenerate.right.z), 1.0f),
          "the camera basis stays orthonormal looking straight down");

    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d hitbox projection check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all hitbox projection checks passed\n");
    return 0;
}

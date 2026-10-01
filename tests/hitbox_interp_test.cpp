// Regression test for Hitbox partial-tick interpolation.
//
// The collision AABB is a 20 Hz tick sample while the game draws the entity
// mesh at an interpolated render position (renderPos = lerp(prev, cur,
// partialTick)). Drawing the raw tick box makes it snap a whole tick of
// movement ahead of the body, so the module moves every box onto that render
// position. These checks pin down the timing fraction, the direction of the
// correction and the sanity guards.
//
//     g++ -std=c++20 -I include -I src tests/hitbox_interp_test.cpp -o /tmp/hitbox_interp_test
//     /tmp/hitbox_interp_test

#include "modules/visual/hitbox_interp.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using bedrocktools::sdk::Vec3;

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

bool near(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}

bool vecNear(const Vec3& v, float x, float y, float z) {
    return near(v.x, x) && near(v.y, y) && near(v.z, z);
}

Vec3 lerp(const Vec3& a, const Vec3& b, float t) {
    return Vec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
} // namespace

int main() {
    std::printf("hitbox partial-tick fraction\n");

    // 20 Hz tick: 0 right after the tick, 1 right before the next one.
    check(hitbox::partialTickFraction(0.0f, 0.05f) == 0.0f, "just after the tick -> 0");
    check(near(hitbox::partialTickFraction(0.025f, 0.05f), 0.5f), "halfway through the tick -> 0.5");
    check(hitbox::partialTickFraction(0.05f, 0.05f) == 1.0f, "at the next tick -> 1");
    check(hitbox::partialTickFraction(0.09f, 0.05f) == 1.0f, "late frame is clamped to 1");
    check(hitbox::partialTickFraction(-0.01f, 0.05f) == 0.0f, "negative elapsed is clamped to 0");
    check(hitbox::partialTickFraction(std::nanf(""), 0.05f) == 0.0f, "NaN elapsed does not leak through");

    // Unknown interval must keep the raw tick AABB, not a stale position.
    check(hitbox::partialTickFraction(0.01f, 0.0f) == 1.0f, "zero interval -> raw box (1)");
    check(hitbox::partialTickFraction(0.01f, -0.05f) == 1.0f, "negative interval -> raw box (1)");

    std::printf("hitbox render-position offset\n");

    // Walking along +X: cur is the tick AABB position, prev the previous tick.
    const Vec3 prev{9.5f, 64.0f, 10.0f};
    const Vec3 cur{10.0f, 64.0f, 10.0f};

    // alpha = 0: the model is drawn at the previous tick position, so the box
    // has to move a full tick of movement back onto it.
    check(vecNear(hitbox::renderPositionOffset(cur, prev, 0.0f), -0.5f, 0.0f, 0.0f),
          "alpha 0 -> box shifted back one full tick of movement");
    check(vecNear(hitbox::renderPositionOffset(cur, prev, 0.5f), -0.25f, 0.0f, 0.0f),
          "alpha 0.5 -> box shifted back half a tick of movement");
    check(vecNear(hitbox::renderPositionOffset(cur, prev, 1.0f), 0.0f, 0.0f, 0.0f),
          "alpha 1 -> raw tick box (old behaviour)");

    // The shifted box is exactly lerp(prev, cur, alpha), i.e. the same
    // position the game draws the entity mesh at.
    for (float alpha = 0.0f; alpha <= 1.0f; alpha += 0.25f) {
        const Vec3 offset = hitbox::renderPositionOffset(cur, prev, alpha);
        const Vec3 shifted{cur.x + offset.x, cur.y + offset.y, cur.z + offset.z};
        check(vecNear(shifted, lerp(prev, cur, alpha).x, lerp(prev, cur, alpha).y,
                      lerp(prev, cur, alpha).z),
              "shifted box equals lerp(prev, cur, alpha)");
    }

    // Standing still is a no-op, so idle entities never move.
    check(vecNear(hitbox::renderPositionOffset(cur, cur, 0.3f), 0.0f, 0.0f, 0.0f),
          "no movement -> no offset");

    std::printf("hitbox tick-delta sanity guard\n");

    check(hitbox::isPlausibleTickDelta(Vec3{0.28f, 0.0f, 0.0f}), "sprinting player accepted");
    check(hitbox::isPlausibleTickDelta(Vec3{0.0f, -3.9f, 0.0f}), "terminal-velocity fall accepted");
    check(hitbox::isPlausibleTickDelta(Vec3{hitbox::kMaxTickDelta, 0.0f, 0.0f}),
          "boundary delta accepted");
    check(hitbox::isPlausibleTickDelta(Vec3{-0.3f, 0.42f, 0.3f}), "jump arc accepted");

    check(!hitbox::isPlausibleTickDelta(Vec3{40.0f, 0.0f, 0.0f}), "teleport rejected");
    check(!hitbox::isPlausibleTickDelta(Vec3{0.0f, -500.0f, 0.0f}), "bad read rejected");
    check(!hitbox::isPlausibleTickDelta(Vec3{std::nanf(""), 0.0f, 0.0f}), "NaN delta rejected");
    check(!hitbox::isPlausibleTickDelta(
              Vec3{std::numeric_limits<float>::infinity(), 0.0f, 0.0f}),
          "infinite delta rejected");

    check(hitbox::isFinite(Vec3{1.0f, 2.0f, 3.0f}), "finite offset accepted");
    check(!hitbox::isFinite(Vec3{std::nanf(""), 2.0f, 3.0f}), "NaN offset rejected");

    std::printf("hitbox position/box containment\n");

    const Vec3 boxMin{10.0f, 64.0f, 10.0f};
    const Vec3 boxMax{10.6f, 65.8f, 10.6f};

    check(hitbox::positionNearBox(Vec3{10.3f, 64.0f, 10.3f}, boxMin, boxMax),
          "feet centre inside the box accepted");
    check(hitbox::positionNearBox(Vec3{10.3f, 65.8f, 10.3f}, boxMin, boxMax),
          "position at head height accepted");
    check(hitbox::positionNearBox(Vec3{10.3f, 63.2f, 10.3f}, boxMin, boxMax),
          "small collision offset stays within the slack");
    check(!hitbox::positionNearBox(Vec3{10.3f, 62.5f, 10.3f}, boxMin, boxMax),
          "position past the slack rejected");
    check(!hitbox::positionNearBox(Vec3{10.3f, 80.0f, 10.3f}, boxMin, boxMax),
          "position far above the box rejected");
    check(!hitbox::positionNearBox(Vec3{25.0f, 64.5f, 10.3f}, boxMin, boxMax),
          "position far to the side rejected");

    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all hitbox interpolation math checks passed\n");
    return 0;
}

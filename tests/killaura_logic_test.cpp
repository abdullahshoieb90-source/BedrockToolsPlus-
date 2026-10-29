// Unit test for Kill Aura's pure target/aim math (killaura_logic.hpp).
//
// The module reads raw actor memory, so every decision that can be expressed
// without game pointers lives in pure helpers. This test pins the Bedrock
// look-angle convention (the inverse of the forward mapping the Hitbox look
// line draws), the vanilla reach measurement (eyes to the closest point of
// the target box) and the sanity guards that keep half-spawned actors and
// recycled-pointer garbage out of the target list.
//
//     g++ -std=c++20 -I include -I src tests/killaura_logic_test.cpp -o /tmp/killaura_logic_test
//     /tmp/killaura_logic_test

#include "modules/combat/killaura_logic.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using bedrocktools::sdk::AABB;
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

bool near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

bool vecNear(const Vec3& v, float x, float y, float z, float eps = 1e-4f) {
    return near(v.x, x, eps) && near(v.y, y, eps) && near(v.z, z, eps);
}

AABB playerBox(float x, float y, float z) {
    return AABB{{x, y, z}, {x + 0.6f, y + 1.8f, z + 0.6f}};
}
} // namespace

int main() {
    std::printf("killaura box sanity\n");
    check(killaura::isAttackableBox(playerBox(0, 64, 0)), "normal player box accepted");
    check(killaura::isAttackableBox(AABB{{0, 0, 0}, {0.25f, 0.25f, 0.25f}}), "small mob box accepted");
    check(!killaura::isAttackableBox(AABB{{0, 0, 0}, {0, 0, 0}}), "all-zero box rejected");
    check(!killaura::isAttackableBox(AABB{{1, 1, 1}, {0, 0, 0}}), "inverted box rejected");
    check(!killaura::isAttackableBox(AABB{{0, 0, 0}, {100.0f, 2.0f, 1.0f}}), "absurd box rejected");
    check(!killaura::isAttackableBox(
              AABB{{std::nanf(""), 0, 0}, {1, 2, 1}}),
          "NaN box rejected");
    check(!killaura::isAttackableBox(
              AABB{{0, 0, 0}, {1, std::numeric_limits<float>::infinity(), 1}}),
          "infinite box rejected");

    std::printf("killaura reach measurement\n");
    const AABB target = playerBox(2.3f, 64.0f, 0.0f);
    const Vec3 eye{0.3f, 65.62f, 0.3f};
    check(vecNear(killaura::closestPointOnAabb(eye, target), 2.3f, 65.62f, 0.3f),
          "closest point is on the near face at eye height");
    check(near(killaura::distancePointToAabb(eye, target), 2.0f),
          "reach is measured to the box surface, not the center");
    check(near(killaura::distancePointToAabb(Vec3{2.6f, 65.0f, 0.3f}, target), 0.0f),
          "point inside the box has zero reach");
    check(vecNear(killaura::eyePosition(playerBox(0, 64, 0)), 0.3f, 64.0f + 1.62f, 0.3f),
          "eye sits 1.62 above the feet of a 1.8 tall box");
    check(vecNear(killaura::boxCenter(target), 2.6f, 64.9f, 0.3f), "box center");

    std::printf("killaura look angles\n");
    const Vec3 origin{0, 0, 0};
    // Bedrock convention: yaw 0 faces +Z, yaw +90 faces -X, negative pitch
    // looks up. Verified against forwardFromAngles (the game's mapping).
    auto anglesFor = [&](Vec3 dir) { return killaura::lookAngles(origin, dir); };
    check(near(anglesFor({0, 0, 5}).yaw, 0.0f) && near(anglesFor({0, 0, 5}).pitch, 0.0f),
          "target at +Z -> yaw 0 pitch 0");
    check(near(anglesFor({-5, 0, 0}).yaw, 90.0f), "target at -X -> yaw +90");
    check(near(anglesFor({5, 0, 0}).yaw, -90.0f), "target at +X -> yaw -90");
    check(near(std::fabs(anglesFor({0, 0, -5}).yaw), 180.0f), "target at -Z -> |yaw| 180");
    check(near(anglesFor({0, 5, 5}).pitch, -45.0f), "target above -> negative pitch");
    check(near(anglesFor({0, -5, 5}).pitch, 45.0f), "target below -> positive pitch");
    check(near(anglesFor({0, 5, 0}).pitch, -90.0f), "target straight up -> pitch -90");

    bool roundTrip = true;
    const Vec3 dirs[] = {{1, 0.2f, 3}, {-2, -0.5f, 1}, {0.3f, 1.5f, -2}, {-4, 0, -4}, {2, -3, 0.5f}};
    for (const Vec3& dir : dirs) {
        const auto angles = killaura::lookAngles(origin, dir);
        const Vec3 fwd = killaura::forwardFromAngles(angles.pitch, angles.yaw);
        const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (!vecNear(fwd, dir.x / len, dir.y / len, dir.z / len, 1e-3f)) roundTrip = false;
    }
    check(roundTrip, "lookAngles inverts the game's forward mapping for arbitrary directions");

    const killaura::Angles fallback{12.0f, 34.0f};
    const auto degenerate = killaura::lookAngles(origin, origin, fallback);
    check(degenerate.pitch == 12.0f && degenerate.yaw == 34.0f,
          "zero direction keeps the current angles");
    const auto nanAngles = killaura::lookAngles(origin, Vec3{std::nanf(""), 0, 1}, fallback);
    check(nanAngles.pitch == 12.0f && nanAngles.yaw == 34.0f,
          "NaN direction keeps the current angles");

    check(near(killaura::wrapYawDegrees(190.0f), -170.0f), "yaw wraps past +180");
    check(near(killaura::wrapYawDegrees(-190.0f), 170.0f), "yaw wraps past -180");
    check(near(killaura::wrapYawDegrees(540.0f), -180.0f), "yaw wraps multiple turns");
    check(near(killaura::clampPitchDegrees(120.0f), 90.0f), "pitch clamps at +90");
    check(near(killaura::clampPitchDegrees(-120.0f), -90.0f), "pitch clamps at -90");

    std::printf("killaura settings clamps\n");
    check(killaura::clampedCps(0) == 1, "cps clamps to at least 1");
    check(killaura::clampedCps(999) == 30, "cps clamps to at most 30");
    check(killaura::attackIntervalMs(10) == 100, "10 CPS -> 100 ms");
    check(killaura::attackIntervalMs(20) == 50, "20 CPS -> 50 ms (one per tick)");
    check(killaura::attackIntervalMs(30) == 33, "30 CPS -> 33 ms");
    check(near(killaura::clampedRange(3.0f), 3.0f), "range passes through in bounds");
    check(near(killaura::clampedRange(0.2f), 1.0f), "range clamps to at least 1");
    check(near(killaura::clampedRange(50.0f), 8.0f), "range clamps to at most 8");
    check(near(killaura::clampedRange(std::nanf("")), 1.0f), "NaN range falls back to minimum");

    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all killaura logic checks passed\n");
    return 0;
}

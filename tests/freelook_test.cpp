// Host-side tests for the Free Look module logic.
//
// Everything the game-side module does runs through the pure helpers in
// modules/visual/freelook_logic.hpp: angle wrapping, the pitch/yaw swing
// limits, the smooth return animation, the runtime calibration that decodes
// how the game applies turn deltas, and the Inactive/Active/Returning phase
// machine that decides which rotation the player must carry at tick time.
//
//     g++ -std=c++20 -I src tests/freelook_test.cpp -o /tmp/freelook_test
//     /tmp/freelook_test

#include "modules/visual/freelook_logic.hpp"

#include <cmath>
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

void checkNear(float actual, float expected, const std::string& what, float eps = 0.001f) {
    check(std::fabs(actual - expected) <= eps,
          what + " (got " + std::to_string(actual) + ", want " + std::to_string(expected) + ")");
}

} // namespace

static void testAngles() {
    std::printf("angle helpers\n");

    checkNear(freelook::wrapDegrees(190.0f), -170.0f, "wrap 190 -> -170");
    checkNear(freelook::wrapDegrees(-190.0f), 170.0f, "wrap -190 -> 170");
    checkNear(freelook::wrapDegrees(360.0f), 0.0f, "wrap 360 -> 0");
    checkNear(freelook::wrapDegrees(-180.0f), 180.0f, "wrap -180 -> 180");
    checkNear(freelook::wrapDegrees(42.0f), 42.0f, "wrap keeps small values");

    // Shortest arc must cross the +/-180 seam instead of spinning 340 degrees.
    // (-180 and 180 are the same heading, so accept either.)
    const float seamMid = freelook::lerpAngle(-170.0f, 170.0f, 0.5f);
    check(std::fabs(std::fabs(seamMid) - 180.0f) < 0.001f, "lerp across the seam");
    checkNear(freelook::lerpAngle(-170.0f, 170.0f, 0.25f), -175.0f, "lerp quarter across the seam");
    checkNear(freelook::lerpAngle(10.0f, 30.0f, 0.5f), 20.0f, "lerp plain");
}

static void testLookDelta() {
    std::printf("free camera movement\n");

    freelook::Settings settings; // 180 yaw / 90 pitch swing = effectively free
    const freelook::Angles locked{0.0f, 0.0f};

    freelook::Angles cam = freelook::applyLookDelta({0.0f, 0.0f}, 10.0f, 25.0f, locked, settings);
    checkNear(cam.pitch, 10.0f, "pitch accumulates");
    checkNear(cam.yaw, 25.0f, "yaw accumulates");

    // Absolute pitch is clamped to the game range even with a free swing.
    cam = freelook::applyLookDelta({85.0f, 0.0f}, 20.0f, 0.0f, locked, settings);
    checkNear(cam.pitch, 90.0f, "pitch clamps at +90");
    cam = freelook::applyLookDelta({-85.0f, 0.0f}, -20.0f, 0.0f, locked, settings);
    checkNear(cam.pitch, -90.0f, "pitch clamps at -90");

    // Clamping the stored value must not create a dead zone to unwind: after
    // pinning at +90, turning back down moves immediately.
    cam = freelook::applyLookDelta({90.0f, 0.0f}, -5.0f, 0.0f, locked, settings);
    checkNear(cam.pitch, 85.0f, "no dead zone after the pitch clamp");

    // Yaw wraps continuously across the seam; with the default 180 degree
    // swing limit a 170+30 turn pins at the limit (the 180/-180 heading).
    cam = freelook::applyLookDelta({0.0f, 170.0f}, 0.0f, 30.0f, locked, settings);
    check(std::fabs(std::fabs(cam.yaw) - 180.0f) < 0.001f, "yaw pins at the +180 swing limit");
    cam = freelook::applyLookDelta({0.0f, -170.0f}, 0.0f, -30.0f, locked, settings);
    check(std::fabs(std::fabs(cam.yaw) - 180.0f) < 0.001f, "yaw pins at the -180 swing limit");

    // Swing limits are measured around the locked angle, wrapped.
    freelook::Settings limited;
    limited.maxYaw = 90.0f;
    limited.maxPitch = 45.0f;
    cam = freelook::applyLookDelta({0.0f, 0.0f}, 0.0f, 120.0f, locked, limited);
    checkNear(cam.yaw, 90.0f, "yaw swing limit engages");
    cam = freelook::applyLookDelta({0.0f, 90.0f}, 0.0f, 120.0f, locked, limited);
    checkNear(cam.yaw, 90.0f, "yaw stays pinned at the limit");
    cam = freelook::applyLookDelta({0.0f, 90.0f}, 0.0f, -200.0f, locked, limited);
    checkNear(cam.yaw, -90.0f, "yaw swings back to the other limit");

    cam = freelook::applyLookDelta({0.0f, 0.0f}, 60.0f, 0.0f, locked, limited);
    checkNear(cam.pitch, 45.0f, "pitch swing limit engages");

    // Limits hold when the locked angle itself is off-center, including
    // through the seam (locked at 170, limit 90 -> [-110 through +180] ... 100).
    const freelook::Angles lockedYaw{0.0f, 170.0f};
    cam = freelook::applyLookDelta({0.0f, 170.0f}, 0.0f, 30.0f, lockedYaw, limited);
    checkNear(cam.yaw, -160.0f, "swing measured through the seam");
    checkNear(freelook::wrapDegrees(cam.yaw - lockedYaw.yaw), 30.0f, "seam swing stays +30");
    cam = freelook::applyLookDelta({0.0f, 170.0f}, 0.0f, 1000.0f, lockedYaw, limited);
    checkNear(cam.yaw, -100.0f, "seam swing clamps at +90");
}

static void testReturn() {
    std::printf("smooth return\n");

    const freelook::Angles locked{0.0f, 90.0f};
    freelook::Angles cam{20.0f, -90.0f}; // 180 degrees of yaw away

    check(!freelook::returnSettled(cam, locked), "return not settled while far away");

    // Step toward the locked angle along the shortest arc. -90 and 90 are
    // 180 degrees apart, so the halfway point is 0 either way.
    cam = freelook::stepReturn(cam, locked, 0.5f);
    checkNear(cam.pitch, 10.0f, "return steps pitch halfway");
    checkNear(cam.yaw, 0.0f, "return steps yaw halfway");

    // Keep stepping until it settles.
    int steps = 0;
    while (!freelook::returnSettled(cam, locked) && steps < 200) {
        cam = freelook::stepReturn(cam, locked, 0.45f);
        ++steps;
    }
    check(steps < 200, "return converges");
    checkNear(cam.pitch, 0.0f, "return pitch reaches locked", 0.5f);
    checkNear(freelook::wrapDegrees(cam.yaw - locked.yaw), 0.0f, "return yaw reaches locked", 0.5f);
    check(freelook::returnSettled(cam, locked), "return reports settled");

    check(freelook::returnSettled(locked, locked), "already at target counts as settled");
}

static void testCalibration() {
    std::printf("turn-delta calibration\n");

    // Default decode assumes {x = pitch, y = yaw}, identity signs.
    freelook::Calibration cal;
    float dPitch = 0.0f, dYaw = 0.0f;
    freelook::decodeRawDelta(cal, 3.0f, 7.0f, dPitch, dYaw);
    checkNear(dPitch, 3.0f, "default decode maps x to pitch");
    checkNear(dYaw, 7.0f, "default decode maps y to yaw");

    check(!cal.syncKnown, "sync unknown before any observation");

    // A queued (async) turn produces no rotation change: no sync proof.
    freelook::observeTurn(cal, 1.0f, 2.0f, 0.0f, 0.0f);
    check(!cal.syncKnown, "async turn does not prove sync");

    // A turn that immediately changes the rotation proves sync.
    freelook::observeTurn(cal, 1.0f, 2.0f, 1.0f, 2.0f);
    check(cal.syncKnown && cal.syncApply, "applied turn proves sync");

    // A clamped turn (non-zero argument, zero change) must not un-prove sync.
    freelook::observeTurn(cal, 5.0f, 0.0f, 0.0f, 0.0f);
    check(cal.syncApply, "clamped turn keeps sync proven");

    // Single-axis observations identify the axis mapping and signs.
    freelook::Calibration mapped;
    // Purely vertical look: x-only argument, pitch-only response, negative sign.
    freelook::observeTurn(mapped, -2.0f, 0.0f, 2.0f, 0.0f);
    check(mapped.axisXIsPitch, "x-only pitch turn marks x as pitch");
    checkNear(mapped.signX, -1.0f, "x sign learned from the vertical turn");
    // Purely horizontal look: y-only argument, yaw-only response.
    freelook::observeTurn(mapped, 0.0f, 4.0f, 0.0f, 4.0f);
    checkNear(mapped.signY, 1.0f, "y sign learned from the horizontal turn");

    freelook::decodeRawDelta(mapped, -2.0f, 4.0f, dPitch, dYaw);
    checkNear(dPitch, 2.0f, "calibrated decode applies the learned x sign");
    checkNear(dYaw, 4.0f, "calibrated decode applies the learned y sign");

    // Swapped layout: argument.x drives yaw instead of pitch.
    freelook::Calibration swapped;
    freelook::observeTurn(swapped, 3.0f, 0.0f, 0.0f, 3.0f);
    check(!swapped.axisXIsPitch, "x-only yaw turn marks x as yaw");
    checkNear(swapped.signX, 1.0f, "x sign learned for yaw");
    freelook::decodeRawDelta(swapped, 3.0f, 5.0f, dPitch, dYaw);
    checkNear(dPitch, 5.0f, "swapped decode maps y to pitch");
    checkNear(dYaw, 3.0f, "swapped decode maps x to yaw");

    // Diagonal turns carry no decisive information and must not disturb a
    // learned mapping.
    freelook::observeTurn(swapped, 2.0f, 3.0f, 5.0f, 2.0f);
    check(!swapped.axisXIsPitch, "diagonal turn keeps the learned mapping");

    // Zero arguments are pure noise and change nothing.
    freelook::Calibration untouched;
    freelook::observeTurn(untouched, 0.0f, 0.0f, 4.0f, 0.0f);
    check(!untouched.syncKnown && untouched.axisXIsPitch, "zero argument changes nothing");
}

static void testPhaseMachine() {
    std::printf("phase machine\n");

    freelook::Core core;
    check(core.phase() == freelook::Core::Phase::Inactive, "starts inactive");

    // Not requested: ticks are untouched.
    check(!core.preTick(true, {10.0f, 20.0f}).has_value(), "inactive tick writes nothing");

    // Requested: engage at the current rotation.
    core.setRequestActive(true);
    auto body = core.preTick(true, {10.0f, 20.0f});
    check(body.has_value(), "engaging returns a body angle");
    checkNear(body->pitch, 10.0f, "locked pitch captured");
    checkNear(body->yaw, 20.0f, "locked yaw captured");
    check(core.active(), "engaged phase is active");

    // Turns redirect into the camera; the locked angle never moves.
    core.applyTurn(15.0f, -40.0f);
    auto view = core.postTick();
    check(view.has_value(), "active tick produces a camera angle");
    checkNear(view->pitch, 25.0f, "camera pitch carries the turn");
    checkNear(view->yaw, -20.0f, "camera yaw carries the turn");

    body = core.preTick(true, {25.0f, -20.0f});
    checkNear(body->pitch, 10.0f, "body stays locked at the tick");
    checkNear(body->yaw, 20.0f, "body yaw stays locked at the tick");

    // Turns while not active (e.g. mid-return) are dropped.
    core.setRequestActive(false);
    body = core.preTick(true, {25.0f, -20.0f});
    check(core.phase() == freelook::Core::Phase::Returning, "release starts the return");
    check(body.has_value(), "return keeps the body locked");
    checkNear(body->pitch, 10.0f, "return body pitch locked");
    checkNear(body->yaw, 20.0f, "return body yaw locked");

    core.applyTurn(50.0f, 50.0f); // dropped: not active
    view = core.postTick();
    check(view.has_value(), "return produces a camera angle");
    check(std::fabs(view->pitch - 25.0f) < 15.0f && std::fabs(view->pitch - 25.0f) > 0.0f,
          "return steps the camera toward the body");

    // Run the return out.
    int steps = 0;
    while (core.phase() != freelook::Core::Phase::Inactive && steps < 200) {
        core.preTick(true, core.camera());
        core.postTick();
        ++steps;
    }
    check(core.phase() == freelook::Core::Phase::Inactive, "return finishes");
    checkNear(core.camera().yaw, 20.0f, "camera back at the locked yaw", 0.5f);
    check(!core.postTick().has_value(), "inactive post-tick writes nothing");

    // Re-engage during a return: the camera continues from where it is.
    core.setRequestActive(true);
    (void)core.preTick(true, {0.0f, 0.0f}); // engage
    core.applyTurn(0.0f, 90.0f);
    core.setRequestActive(false);
    (void)core.preTick(true, {0.0f, 90.0f}); // start returning
    check(core.phase() == freelook::Core::Phase::Returning, "returning mid-test");
    core.setRequestActive(true);
    body = core.preTick(true, {0.0f, 0.0f});
    check(core.active(), "re-engage resumes from the return");
    checkNear(body->yaw, 0.0f, "re-engage keeps the original lock");
    view = core.postTick();
    checkNear(view->yaw, 90.0f, "camera kept across the re-engage");

    // Module disabled mid-free-look: the core releases like a normal release
    // (the smooth return still runs, because the tick handlers keep firing);
    // the module itself additionally snaps the camera home through
    // forceInactive, which is what onDisable calls.
    core.setRequestActive(false);
    body = core.preTick(false, {0.0f, 90.0f});
    check(core.phase() == freelook::Core::Phase::Returning, "disabled tick releases smoothly");
    check(body.has_value(), "disabled tick still writes the locked body");
    checkNear(body->yaw, 0.0f, "disabled release body yaw locked");
    core.forceInactive();
    check(core.phase() == freelook::Core::Phase::Inactive, "forceInactive drops the phase");
    check(!core.postTick().has_value(), "nothing written after forceInactive");

    // forceInactive resets the camera to the locked angle.
    core.setRequestActive(true);
    (void)core.preTick(true, {5.0f, 5.0f}); // engage at 5/5
    core.applyTurn(10.0f, 10.0f);           // camera now 15/15
    core.setRequestActive(false);
    core.forceInactive();
    check(core.phase() == freelook::Core::Phase::Inactive, "forceInactive drops the phase again");
    checkNear(core.camera().pitch, 5.0f, "forceInactive resets the camera pitch");
    checkNear(core.camera().yaw, 5.0f, "forceInactive resets the camera yaw");
}

static void testTurnRedirectionThroughCore() {
    std::printf("measured turn redirection\n");

    // Simulates the calibrated hook path: the game applies +5 pitch / +10 yaw,
    // the module reverts the player and redirects the measured delta.
    freelook::Core core;
    core.setRequestActive(true);
    (void)core.preTick(true, {0.0f, 0.0f});

    core.applyTurn(5.0f, 10.0f);
    core.applyTurn(5.0f, 10.0f);
    const auto view = core.postTick();
    checkNear(view->pitch, 10.0f, "measured pitch deltas accumulate");
    checkNear(view->yaw, 20.0f, "measured yaw deltas accumulate");

    // The same turn through the raw-decode path (uncalibrated hook).
    freelook::Core raw;
    raw.setRequestActive(true);
    (void)raw.preTick(true, {0.0f, 0.0f});
    float dPitch = 0.0f, dYaw = 0.0f;
    freelook::decodeRawDelta(raw.calibration, 5.0f, 10.0f, dPitch, dYaw);
    raw.applyTurn(dPitch, dYaw);
    const auto rawView = raw.postTick();
    checkNear(rawView->pitch, 5.0f, "raw decode pitch matches identity layout");
    checkNear(rawView->yaw, 10.0f, "raw decode yaw matches identity layout");
}

int main() {
    std::printf("free look logic\n");
    testAngles();
    testLookDelta();
    testReturn();
    testCalibration();
    testPhaseMachine();
    testTurnRedirectionThroughCore();

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("all free look checks passed\n");
        return 0;
    }
    std::printf("%d free look check(s) failed\n", g_failures);
    return 1;
}

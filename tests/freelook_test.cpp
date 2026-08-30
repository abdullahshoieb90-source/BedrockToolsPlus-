// Host-side tests for the Free Look module logic.
//
// Free Look drives the camera through LocalPlayer::applyTurnDelta (which is
// what the modern Bedrock camera system listens to) and freezes the actor
// rotation component (the body). Everything that decides how much of a turn
// reaches the camera, how the camera is walked back onto the body on release
// and when the body is unlocked again lives in the pure helpers of
// modules/visual/freelook_logic.hpp, which is what this test covers:
// the swing limits, the release animation and the Inactive/Active/Returning
// phase machine.
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

void checkTurn(const freelook::Turn& actual, float pitch, float yaw, const std::string& what,
               float eps = 0.001f) {
    check(std::fabs(actual.pitch - pitch) <= eps && std::fabs(actual.yaw - yaw) <= eps,
          what + " (got {" + std::to_string(actual.pitch) + ", " + std::to_string(actual.yaw) +
              "}, want {" + std::to_string(pitch) + ", " + std::to_string(yaw) + "})");
}

} // namespace

static void testAngles() {
    std::printf("angle helpers\n");

    checkNear(freelook::wrapDegrees(190.0f), -170.0f, "wrap 190 -> -170");
    checkNear(freelook::wrapDegrees(-190.0f), 170.0f, "wrap -190 -> 170");
    checkNear(freelook::wrapDegrees(360.0f), 0.0f, "wrap 360 -> 0");
    checkNear(freelook::wrapDegrees(-180.0f), 180.0f, "wrap -180 -> 180");
    checkNear(freelook::wrapDegrees(42.0f), 42.0f, "wrap keeps small values");

    checkNear(freelook::clampf(5.0f, 0.0f, 3.0f), 3.0f, "clamp above");
    checkNear(freelook::clampf(-5.0f, 0.0f, 3.0f), 0.0f, "clamp below");
}

static void testTurnClipping() {
    std::printf("turn clipping\n");

    freelook::Settings settings; // 180 yaw / 90 pitch = effectively unlimited
    const freelook::Angles locked{0.0f, 0.0f};
    const freelook::Turn none{0.0f, 0.0f};

    // Inside the limits the delta reaches the camera untouched — Free Look
    // must not scale or reshape the look input.
    checkTurn(freelook::clipTurn(none, locked, {10.0f, 25.0f}, settings), 10.0f, 25.0f,
              "delta passes through unchanged");
    checkTurn(freelook::clipTurn({10.0f, 25.0f}, locked, {-3.0f, -5.0f}, settings), -3.0f, -5.0f,
              "negative delta passes through unchanged");

    // Absolute pitch: the camera never leaves [-90, 90], so a delta running
    // into the pole is trimmed to what is left rather than dropped.
    checkTurn(freelook::clipTurn({85.0f, 0.0f}, locked, {20.0f, 0.0f}, settings), 5.0f, 0.0f,
              "pitch trimmed at +90");
    checkTurn(freelook::clipTurn({-85.0f, 0.0f}, locked, {-20.0f, 0.0f}, settings), -5.0f, 0.0f,
              "pitch trimmed at -90");
    checkTurn(freelook::clipTurn({90.0f, 0.0f}, locked, {5.0f, 0.0f}, settings), 0.0f, 0.0f,
              "pitch pinned at +90 blocks further up");
    // No dead zone: turning back down moves immediately.
    checkTurn(freelook::clipTurn({90.0f, 0.0f}, locked, {-5.0f, 0.0f}, settings), -5.0f, 0.0f,
              "no dead zone after the pitch pole");

    // The absolute pole is measured on the resulting camera angle, not on the
    // swing, so it also engages when the lock itself is already tilted.
    const freelook::Angles tilted{80.0f, 0.0f};
    checkTurn(freelook::clipTurn(none, tilted, {20.0f, 0.0f}, settings), 10.0f, 0.0f,
              "pole measured from the locked pitch");

    // Swing limits.
    freelook::Settings limited;
    limited.maxYaw = 90.0f;
    limited.maxPitch = 45.0f;

    checkTurn(freelook::clipTurn(none, locked, {60.0f, 0.0f}, limited), 45.0f, 0.0f,
              "pitch swing limit trims the delta");
    checkTurn(freelook::clipTurn({45.0f, 0.0f}, locked, {10.0f, 0.0f}, limited), 0.0f, 0.0f,
              "pitch stays pinned at the swing limit");
    checkTurn(freelook::clipTurn({45.0f, 0.0f}, locked, {-5.0f, 0.0f}, limited), -5.0f, 0.0f,
              "no dead zone after the pitch swing limit");
    checkTurn(freelook::clipTurn(none, locked, {-60.0f, 0.0f}, limited), -45.0f, 0.0f,
              "pitch swing limit trims downward too");

    checkTurn(freelook::clipTurn(none, locked, {0.0f, 120.0f}, limited), 0.0f, 90.0f,
              "yaw swing limit trims the delta");
    checkTurn(freelook::clipTurn({0.0f, 90.0f}, locked, {0.0f, 120.0f}, limited), 0.0f, 0.0f,
              "yaw stays pinned at the swing limit");
    checkTurn(freelook::clipTurn({0.0f, 90.0f}, locked, {0.0f, -200.0f}, limited), 0.0f, -180.0f,
              "yaw swings across to the other limit");

    // The yaw limit works on the incrementally accumulated swing, never on
    // wrapDegrees(camera - locked): the wrapped difference flips sign at the
    // seam, which would hand the far side of the lock to the next delta and
    // teleport the camera 350 degrees around.
    freelook::Settings wide; // maxYaw 180: the swing can reach the seam
    checkTurn(freelook::clipTurn({0.0f, -180.0f}, locked, {0.0f, -10.0f}, wide), 0.0f, 0.0f,
              "swing pinned at -180 does not jump to the far side");
    checkTurn(freelook::clipTurn({0.0f, -180.0f}, locked, {0.0f, 10.0f}, wide), 0.0f, 10.0f,
              "swing at -180 still turns back");
    checkTurn(freelook::clipTurn({0.0f, 180.0f}, locked, {0.0f, 10.0f}, wide), 0.0f, 0.0f,
              "swing pinned at +180 blocks further turning");

    // Limits hold when the locked angle itself sits near the seam: only the
    // swing matters, the absolute camera yaw may wrap freely.
    const freelook::Angles nearSeam{0.0f, 170.0f};
    checkTurn(freelook::clipTurn(none, nearSeam, {0.0f, 30.0f}, limited), 0.0f, 30.0f,
              "turning through the seam is not clipped");
    checkTurn(freelook::clipTurn({0.0f, 30.0f}, nearSeam, {0.0f, 1000.0f}, limited), 0.0f, 60.0f,
              "seam swing clamps at +90");

    // A zero swing limit freezes the camera completely.
    freelook::Settings frozen;
    frozen.maxYaw = 0.0f;
    frozen.maxPitch = 0.0f;
    checkTurn(freelook::clipTurn(none, locked, {50.0f, 50.0f}, frozen), 0.0f, 0.0f,
              "zero limits block everything");
}

static void testReturnStep() {
    std::printf("release animation\n");

    freelook::Settings smooth;
    smooth.smoothReturn = true;
    smooth.returnLerp = 0.45f;

    // The compensating delta walks the camera back: opposite sign, a share of
    // the remaining swing per tick.
    checkTurn(freelook::returnStep({20.0f, -40.0f}, smooth), -9.0f, 18.0f,
              "smooth return steps 45% of the swing back");

    freelook::Settings snap;
    snap.smoothReturn = false;
    checkTurn(freelook::returnStep({20.0f, -40.0f}, snap), -20.0f, 40.0f,
              "snap return undoes the whole swing at once");
    checkTurn(freelook::returnStep({0.0f, 0.0f}, snap), 0.0f, 0.0f, "no swing, no step");

    // A misconfigured lerp cannot stall the return (or overshoot it).
    freelook::Settings broken;
    broken.smoothReturn = true;
    broken.returnLerp = 0.0f;
    checkTurn(freelook::returnStep({100.0f, 0.0f}, broken), -5.0f, 0.0f, "zero lerp is floored");
    broken.returnLerp = 5.0f;
    checkTurn(freelook::returnStep({100.0f, 0.0f}, broken), -100.0f, 0.0f, "huge lerp is capped");

    check(freelook::swingSettled({0.2f, -0.3f}), "small swing counts as settled");
    check(!freelook::swingSettled({0.2f, -3.0f}), "large swing is not settled");
    check(freelook::swingZero({0.0f, 0.0f}), "zero swing detected");
    check(!freelook::swingZero({0.0f, 0.01f}), "near-zero swing is not zero");
}

static void testMovementFrameLock() {
    std::printf("movement frame lock\n");

    freelook::MovementFrameLock lock;
    constexpr std::uint16_t kCamera = freelook::MovementFrameLock::CameraRelativeMovement;
    constexpr std::uint16_t kRot = freelook::MovementFrameLock::RotControlledByMoveDirection;
    constexpr std::uint16_t kMask = freelook::MovementFrameLock::LockMask;

    // Idle: pass-through, never touches the value or engages.
    check(!lock.locked(), "lock starts idle");
    check(lock.tick(false, 0x1AB) == 0x1AB, "idle tick passes the flags through");
    check(!lock.locked(), "idle tick does not engage");

    // Engaging clears exactly the two camera-relative bits, keeping the rest.
    const std::uint16_t scheme = static_cast<std::uint16_t>(kCamera | kRot);
    check(lock.tick(true, scheme) == 0, "engaging clears the camera-relative scheme");
    check(lock.locked(), "engaging arms the lock");
    check(lock.tick(true, static_cast<std::uint16_t>(kMask | 0x21)) == 0x21,
          "other flags survive while the lock is on");

    // Releasing restores the saved bits, preserving whatever else changed.
    check(lock.tick(false, static_cast<std::uint16_t>(kMask | 0x21)) ==
              static_cast<std::uint16_t>(scheme | 0x21),
          "release restores the saved scheme bits");
    check(!lock.locked(), "release disarms the lock");
    check(lock.tick(false, 0x9999) == 0x9999, "released lock passes through again");

    // A re-engage captures the then-current scheme, not the old saved one.
    const std::uint16_t partial = kCamera;  // only strafe-relative remains
    lock.tick(true, kMask);
    lock.tick(false, kMask);
    (void)lock.tick(true, partial);
    check(lock.tick(false, static_cast<std::uint16_t>(partial | 0x2)) ==
              static_cast<std::uint16_t>(partial | 0x2),
          "re-engage restores the newly captured scheme");
    check(!lock.locked(), "re-engaged lock releases cleanly");

    // Reset drops everything without writing (player replaced / world left).
    (void)lock.tick(true, kMask);
    lock.reset();
    check(!lock.locked(), "reset disarms without a restore write");
    check(lock.tick(true, 0x104) == 0x104, "after reset the next engage re-captures");
}

static void testPhaseMachine() {
    std::printf("phase machine\n");

    freelook::Core core;
    check(core.phase() == freelook::Core::Phase::Inactive, "starts inactive");

    // Inactive: turns are none of our business and ticks are untouched.
    checkTurn(core.filterTurn({7.0f, 9.0f}), 7.0f, 9.0f, "inactive turn passes through");
    check(!core.preTick(true, {10.0f, 20.0f}).has_value(), "inactive tick writes nothing");
    check(!core.postTick().body.has_value(), "inactive post-tick writes nothing");

    // Requested: engage and lock the body where it faces.
    core.setRequestActive(true);
    auto body = core.preTick(true, {10.0f, 20.0f});
    check(body.has_value(), "engaging returns a body angle");
    checkNear(body->pitch, 10.0f, "locked pitch captured");
    checkNear(body->yaw, 20.0f, "locked yaw captured");
    check(core.active(), "engaged phase is active");
    checkTurn(core.swing(), 0.0f, 0.0f, "engaging starts with no swing");

    // Turns reach the camera untouched and are booked into the swing; the
    // body never moves with them.
    checkTurn(core.filterTurn({15.0f, -40.0f}), 15.0f, -40.0f, "active turn reaches the camera");
    checkTurn(core.swing(), 15.0f, -40.0f, "swing tracks the camera");
    checkNear(core.camera().pitch, 25.0f, "camera pitch = locked + swing");
    checkNear(core.camera().yaw, -20.0f, "camera yaw = locked + swing");
    checkNear(core.locked().pitch, 10.0f, "locked pitch untouched by the turn");
    checkNear(core.locked().yaw, 20.0f, "locked yaw untouched by the turn");

    auto out = core.postTick();
    check(out.body.has_value(), "active post-tick re-asserts the body");
    checkNear(out.body->pitch, 10.0f, "post-tick body pitch locked");
    checkNear(out.body->yaw, 20.0f, "post-tick body yaw locked");
    check(!out.camera.has_value(), "active post-tick sends no camera delta");

    body = core.preTick(true, {10.0f, 20.0f});
    check(body.has_value() && body->pitch == 10.0f && body->yaw == 20.0f,
          "body stays locked at the next tick");

    // Release: the body stays locked, the camera is walked home.
    core.setRequestActive(false);
    body = core.preTick(true, {10.0f, 20.0f});
    check(core.phase() == freelook::Core::Phase::Returning, "release starts the return");
    check(body.has_value() && body->yaw == 20.0f, "return keeps the body locked");

    // Fresh input is swallowed while the return owns the camera.
    checkTurn(core.filterTurn({50.0f, 50.0f}), 0.0f, 0.0f, "input is zeroed during the return");
    checkTurn(core.swing(), 15.0f, -40.0f, "swallowed input does not move the swing");

    // Every step sends a compensating delta and shrinks the swing; the body
    // is only unlocked once the camera has arrived.
    freelook::Turn compensated{0.0f, 0.0f};
    int steps = 0;
    while (core.phase() != freelook::Core::Phase::Inactive && steps < 200) {
        out = core.postTick();
        check(out.body.has_value(), "returning post-tick keeps the body locked");
        if (out.camera) {
            compensated.pitch += out.camera->pitch;
            compensated.yaw += out.camera->yaw;
        }
        ++steps;
        if (core.phase() != freelook::Core::Phase::Inactive) {
            core.preTick(true, {10.0f, 20.0f});
        }
    }
    check(steps > 1, "smooth return takes several ticks");
    check(steps < 200, "smooth return finishes");
    checkTurn(compensated, -15.0f, 40.0f, "compensating deltas undo the swing exactly");
    checkTurn(core.swing(), 0.0f, 0.0f, "swing is closed out");
    check(core.phase() == freelook::Core::Phase::Inactive, "body unlocked after the camera lands");
    check(!core.postTick().body.has_value(), "nothing written once inactive");
    checkTurn(core.filterTurn({4.0f, 4.0f}), 4.0f, 4.0f, "turns pass through again");

    // Snap return: one full step, done.
    freelook::Core snap;
    snap.settings.smoothReturn = false;
    snap.setRequestActive(true);
    (void)snap.preTick(true, {0.0f, 0.0f});
    (void)snap.filterTurn({12.0f, 30.0f});
    snap.setRequestActive(false);
    (void)snap.preTick(true, {0.0f, 0.0f});
    check(snap.phase() == freelook::Core::Phase::Returning, "snap release still returns first");
    out = snap.postTick();
    check(out.camera.has_value(), "snap return sends a camera delta");
    checkTurn(*out.camera, -12.0f, -30.0f, "snap return undoes the swing in one step");
    check(snap.phase() == freelook::Core::Phase::Inactive, "snap return finishes in one tick");
    checkTurn(snap.swing(), 0.0f, 0.0f, "snap return leaves no swing");

    // Releasing without any swing skips the return entirely.
    freelook::Core idle;
    idle.setRequestActive(true);
    (void)idle.preTick(true, {3.0f, 4.0f});
    idle.setRequestActive(false);
    (void)idle.preTick(true, {3.0f, 4.0f});
    check(idle.phase() == freelook::Core::Phase::Inactive, "release without swing ends at once");
    check(!idle.postTick().camera.has_value(), "no compensation without swing");

    // Re-engage mid-return: the camera keeps the swing it still has.
    freelook::Core again;
    again.setRequestActive(true);
    (void)again.preTick(true, {0.0f, 0.0f});
    (void)again.filterTurn({0.0f, 90.0f});
    again.setRequestActive(false);
    (void)again.preTick(true, {0.0f, 0.0f});
    (void)again.postTick(); // one return step
    const float partial = again.swing().yaw;
    check(partial > 0.0f && partial < 90.0f, "return moved part of the way");
    again.setRequestActive(true);
    body = again.preTick(true, {0.0f, 0.0f});
    check(again.active(), "re-engage resumes from the return");
    checkNear(body->yaw, 0.0f, "re-engage keeps the original lock");
    checkNear(again.swing().yaw, partial, "re-engage keeps the current swing");
    checkTurn(again.filterTurn({0.0f, 5.0f}), 0.0f, 5.0f, "input flows again after re-engage");

    // The module disabling itself mid-swing: forceInactive drops everything
    // without compensating (the module sends the catch-up delta itself).
    again.forceInactive();
    check(again.phase() == freelook::Core::Phase::Inactive, "forceInactive drops the phase");
    checkTurn(again.swing(), 0.0f, 0.0f, "forceInactive clears the swing");

    // A tick with the module disabled releases like a normal release.
    freelook::Core off;
    off.setRequestActive(true);
    (void)off.preTick(true, {0.0f, 0.0f});
    (void)off.filterTurn({0.0f, 20.0f});
    body = off.preTick(false, {0.0f, 0.0f});
    check(off.phase() == freelook::Core::Phase::Returning, "disabled tick releases the camera");
    check(body.has_value() && body->yaw == 0.0f, "disabled tick still locks the body");
}

static void testLimitsThroughCore() {
    std::printf("limits through the core\n");

    freelook::Core core;
    core.settings.maxYaw = 60.0f;
    core.settings.maxPitch = 30.0f;
    core.setRequestActive(true);
    (void)core.preTick(true, {0.0f, 170.0f}); // locked near the +/-180 seam

    // The first flick is trimmed to the limits, the next one is swallowed.
    checkTurn(core.filterTurn({100.0f, 100.0f}), 30.0f, 60.0f, "first flick trims to the limits");
    checkTurn(core.filterTurn({10.0f, 10.0f}), 0.0f, 0.0f, "further input at the limit is blocked");
    checkNear(core.camera().pitch, 30.0f, "camera pitch sits at the limit");
    checkNear(core.camera().yaw, -130.0f, "camera yaw wrapped across the seam");
    checkNear(core.locked().yaw, 170.0f, "body still locked at the seam");

    // Turning back is immediate and the return compensates the real swing,
    // seam or not.
    checkTurn(core.filterTurn({-5.0f, -5.0f}), -5.0f, -5.0f, "turning back is not blocked");
    core.setRequestActive(false);
    (void)core.preTick(true, {0.0f, 170.0f});
    core.settings.smoothReturn = false;
    const auto out = core.postTick();
    check(out.camera.has_value(), "return compensates across the seam");
    checkTurn(*out.camera, -25.0f, -55.0f, "compensation matches the accumulated swing");
    check(core.phase() == freelook::Core::Phase::Inactive, "body unlocked after the seam return");
}

int main() {
    std::printf("free look logic\n");
    testAngles();
    testTurnClipping();
    testReturnStep();
    testMovementFrameLock();
    testPhaseMachine();
    testLimitsThroughCore();

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("all free look checks passed\n");
        return 0;
    }
    std::printf("%d free look check(s) failed\n", g_failures);
    return 1;
}

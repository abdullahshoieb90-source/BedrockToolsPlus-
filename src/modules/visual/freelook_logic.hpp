#pragma once

// Pure state machine and math for the Free Look module.
//
// Free Look decouples the camera from the player's body rotation: while it is
// active the look input still turns the *camera*, but the player's rotation —
// which drives movement, attacks and the rotation sent to the server — stays
// locked at the angle it had when Free Look started.
//
// Everything in this header is plain C++ with no game dependencies so the
// host-side tests (tests/freelook_test.cpp) cover the exact logic the game
// runs: angle wrapping, the pitch/yaw swing limits, the smooth return, the
// calibration that decodes how the game applies turn deltas, and the
// Inactive/Active/Returning phase transitions.

#include <cmath>
#include <cstdint>
#include <optional>

namespace freelook {

// ---------------------------------------------------------------------------
// Angles.
// ---------------------------------------------------------------------------

struct Angles {
    float pitch = 0.0f; // degrees, [-90, 90]
    float yaw = 0.0f;   // degrees, wrapped to (-180, 180]

    friend bool operator==(const Angles&, const Angles&) = default;
};

// Wrap a degree value into (-180, 180].
inline float wrapDegrees(float value) {
    while (value > 180.0f) value -= 360.0f;
    while (value <= -180.0f) value += 360.0f;
    return value;
}

// Shortest-arc interpolation between two angles in degrees, so yaw never
// spins the long way around the +/-180 seam (same trick as the Wings module).
inline float lerpAngle(float from, float to, float t) {
    return from + wrapDegrees(to - from) * t;
}

inline float clampf(float value, float lo, float hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

// ---------------------------------------------------------------------------
// Free camera movement.
// ---------------------------------------------------------------------------

struct Settings {
    // How far the camera may swing away from the locked body angle, in
    // degrees. 180 yaw / 90 pitch is effectively unlimited.
    float maxYaw = 180.0f;
    float maxPitch = 90.0f;

    // Animate the camera back to the body angle on release instead of
    // snapping. `returnLerp` is the per-tick (20 tps) lerp factor.
    bool smoothReturn = true;
    float returnLerp = 0.45f;
};

// Apply one turn delta (in degrees, same sign space as the player rotation)
// to the free camera, clamped to the swing limits around the locked angle.
inline Angles applyLookDelta(Angles cam, float deltaPitch, float deltaYaw,
                             const Angles& locked, const Settings& settings) {
    // The swing limits accumulate incrementally from the camera's current
    // swing. Measuring the wrapped total instead would let a flick past the
    // limit teleport the camera to the far side of the lock.
    //
    // Pitch: bound both the swing relative to the locked pitch and the
    // absolute game range, and clamp the stored value so there is no dead
    // zone to unwind when a limit lets go.
    const float pitchSwing =
        clampf((cam.pitch - locked.pitch) + deltaPitch, -settings.maxPitch, settings.maxPitch);
    cam.pitch = clampf(locked.pitch + pitchSwing, -90.0f, 90.0f);

    // Yaw: the swing is wrapped so turning across the +/-180 seam stays
    // continuous, and the final angle is re-wrapped into (-180, 180].
    const float yawSwing =
        clampf(wrapDegrees(cam.yaw - locked.yaw) + deltaYaw, -settings.maxYaw, settings.maxYaw);
    cam.yaw = wrapDegrees(locked.yaw + yawSwing);

    return cam;
}

// One 20 tps step of the release animation.
inline Angles stepReturn(const Angles& cam, const Angles& locked, float lerp) {
    return Angles{lerpAngle(cam.pitch, locked.pitch, lerp),
                  lerpAngle(cam.yaw, locked.yaw, lerp)};
}

inline bool returnSettled(const Angles& cam, const Angles& locked, float epsilon = 0.5f) {
    return std::fabs(cam.pitch - locked.pitch) <= epsilon &&
           std::fabs(wrapDegrees(cam.yaw - locked.yaw)) <= epsilon;
}

// ---------------------------------------------------------------------------
// Turn-delta calibration.
//
// The hook intercepts LocalPlayer::applyTurnDelta(Vec2 const&). We never
// assume the layout of that Vec2: whenever the game applies turns normally we
// compare the argument with the rotation change it actually produced and
// learn, at runtime,
//   * whether the function applies the delta synchronously (it changes the
//     rotation before returning) or merely queues it for the next tick, and
//   * which component of the argument drives pitch vs yaw, and with which
//     sign.
// While calibrated we redirect the *measured* rotation change into the camera
// (exact, including the game's own clamping); before that we fall back to the
// natural {x = pitch, y = yaw} layout.
// ---------------------------------------------------------------------------

struct Calibration {
    bool syncKnown = false;   // we have seen a turn being applied immediately
    bool syncApply = false;   // true once synchronous application is proven
    bool axisXIsPitch = true; // argument.x drives pitch (else it drives yaw)
    float signX = 1.0f;       // sign of argument.x's contribution
    float signY = 1.0f;       // sign of argument.y's contribution
};

// Observe one pass-through call: the argument handed to applyTurnDelta and
// the rotation change it produced ({0, 0} when it only queued the turn).
inline void observeTurn(Calibration& cal, float argX, float argY, float dPitch, float dYaw) {
    constexpr float kEps = 1e-4f;

    const bool argTurned = std::fabs(argX) > kEps || std::fabs(argY) > kEps;
    const bool rotTurned = std::fabs(dPitch) > kEps || std::fabs(dYaw) > kEps;

    // Only a rotation change proves synchronous application. The reverse is
    // not evidence of anything: a clamped turn (pitch pinned at +/-90) also
    // produces zero change from a non-zero argument.
    if (argTurned && rotTurned) {
        cal.syncKnown = true;
        cal.syncApply = true;
    }

    // Single-axis arguments identify the axis mapping and its sign. Diagonal
    // turns carry no decisive information, so they are skipped; purely
    // horizontal and purely vertical drags happen within seconds of play.
    const bool xOnly = std::fabs(argX) > kEps && std::fabs(argY) <= kEps;
    const bool yOnly = std::fabs(argY) > kEps && std::fabs(argX) <= kEps;
    const bool pitchOnly = std::fabs(dPitch) > kEps && std::fabs(dYaw) <= kEps;
    const bool yawOnly = std::fabs(dYaw) > kEps && std::fabs(dPitch) <= kEps;

    const auto signOf = [](float arg, float applied) {
        return (arg > 0.0f) == (applied > 0.0f) ? 1.0f : -1.0f;
    };

    if (xOnly && pitchOnly) {
        cal.axisXIsPitch = true;
        cal.signX = signOf(argX, dPitch);
    } else if (xOnly && yawOnly) {
        cal.axisXIsPitch = false;
        cal.signX = signOf(argX, dYaw);
    } else if (yOnly && pitchOnly) {
        cal.axisXIsPitch = false;
        cal.signY = signOf(argY, dPitch);
    } else if (yOnly && yawOnly) {
        cal.axisXIsPitch = true;
        cal.signY = signOf(argY, dYaw);
    }
}

// Decode a raw applyTurnDelta argument into a rotation delta using whatever
// the calibration has learned so far (identity layout by default).
inline void decodeRawDelta(const Calibration& cal, float argX, float argY,
                           float& dPitch, float& dYaw) {
    if (cal.axisXIsPitch) {
        dPitch = argX * cal.signX;
        dYaw = argY * cal.signY;
    } else {
        dPitch = argY * cal.signY;
        dYaw = argX * cal.signX;
    }
}

// ---------------------------------------------------------------------------
// Phase machine.
//
//   Inactive   - Free Look off; the game turns the player normally.
//   Active     - look input is redirected into the free camera; the body
//                rotation is forced to the locked angle for every tick.
//   Returning  - released, but the camera is still animating back to the
//                body angle. The body stays locked until it arrives.
//
// The module calls preTick()/postTick() around LocalPlayer::normalTick and
// writes whatever they return into the player's rotation component:
// preTick returns the locked body angle for the tick (movement and the
// MovePlayerPacket then use it), postTick returns the free camera angle that
// the frames until the next tick render with.
// ---------------------------------------------------------------------------

class Core {
public:
    enum class Phase : std::uint8_t { Inactive, Active, Returning };

    Settings settings;
    Calibration calibration;

    Phase phase() const { return mPhase; }
    bool directing() const { return mPhase != Phase::Inactive; }
    bool active() const { return mPhase == Phase::Active; }
    bool useMeasuredTurns() const { return calibration.syncApply; }
    const Angles& locked() const { return mLocked; }
    const Angles& camera() const { return mCam; }

    void setRequestActive(bool value) { mRequestActive = value; }
    bool requestActive() const { return mRequestActive; }

    // Drop everything without animating (module disabled, player changed,
    // world left). The caller restores the locked angle if we were directing.
    void forceInactive() {
        mPhase = Phase::Inactive;
        mCam = mLocked;
    }

    // Redirect one turn into the free camera. `dPitch`/`dYaw` are in degrees
    // in the player rotation's sign space (either measured from the game or
    // decoded from the raw argument).
    void applyTurn(float dPitch, float dYaw) {
        if (mPhase != Phase::Active) return;
        mCam = applyLookDelta(mCam, dPitch, dYaw, mLocked, settings);
    }

    // Run at the top of every LocalPlayer tick. `current` is the player's
    // rotation right now (game thread). Returns the rotation the body must
    // have for this tick, or nullopt when Free Look is not involved at all.
    std::optional<Angles> preTick(bool moduleEnabled, const Angles& current) {
        if (mPhase == Phase::Inactive) {
            if (!moduleEnabled || !mRequestActive) return std::nullopt;
            // Engage: lock the body where it currently faces.
            mLocked = current;
            mCam = current;
            mPhase = Phase::Active;
            return mLocked;
        }

        if (moduleEnabled && mRequestActive) {
            // Still requested (or re-engaged mid-return): stay active.
            mPhase = Phase::Active;
            return mLocked;
        }

        // Released: either animate the camera home or let go immediately.
        if (settings.smoothReturn && !returnSettled(mCam, mLocked)) {
            mPhase = Phase::Returning;
        } else {
            mPhase = Phase::Inactive;
            mCam = mLocked;
        }
        return mLocked;
    }

    // Run right after the LocalPlayer tick. Returns the rotation the frames
    // until the next tick should render with, or nullopt when the game
    // should keep whatever it has.
    std::optional<Angles> postTick() {
        if (mPhase == Phase::Inactive) return std::nullopt;

        if (mPhase == Phase::Returning) {
            mCam = stepReturn(mCam, mLocked, settings.returnLerp);
            if (returnSettled(mCam, mLocked)) {
                mPhase = Phase::Inactive;
                mCam = mLocked;
                return mLocked; // final restore write
            }
        }
        return mCam;
    }

private:
    Phase mPhase = Phase::Inactive;
    bool mRequestActive = false;
    Angles mLocked;
    Angles mCam;
};

} // namespace freelook

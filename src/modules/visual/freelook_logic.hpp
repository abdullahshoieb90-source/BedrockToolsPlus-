#pragma once

// Pure state machine and math for the Free Look module.
//
// How Free Look maps onto modern Bedrock (confirmed by field testing):
//
//   * LocalPlayer::applyTurnDelta drives the *camera*. It belongs to the new
//     camera system and does not touch the actor rotation at all.
//   * The actor rotation component (Actor::mActorRotationComponent, a Vec2
//     laid out as { x = pitch, y = yaw }) is the *body*: the player model,
//     the movement direction and the rotation that goes out in packets.
//
// So the module does exactly two things while it is engaged:
//
//   * turn deltas keep flowing through to the camera untouched — they are
//     only trimmed where the camera would swing further than Max Yaw /
//     Max Pitch away from the locked angle, and zeroed while the release
//     animation is running, and
//   * the body rotation is pinned to the angle Free Look started at, around
//     every tick.
//
// The camera angle is never read back from the game: it is tracked by summing
// the deltas that were allowed through, in the identity layout the function
// takes ({ x = pitch, y = yaw }). On release the accumulated swing is undone
// with compensating deltas sent through the original applyTurnDelta, and the
// body is unlocked only once the camera has arrived back on it.
//
// Everything in this header is plain C++ with no game dependencies so the
// host-side tests (tests/freelook_test.cpp) cover the exact logic the game
// runs: the swing limits, the release animation and the
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

// A pitch/yaw pair in degrees. Used for turn deltas (the applyTurnDelta
// argument, identity layout { x = pitch, y = yaw }) and for the camera's
// accumulated swing away from the locked body angle.
struct Turn {
    float pitch = 0.0f;
    float yaw = 0.0f;

    friend bool operator==(const Turn&, const Turn&) = default;
};

// Wrap a degree value into (-180, 180].
inline float wrapDegrees(float value) {
    while (value > 180.0f) value -= 360.0f;
    while (value <= -180.0f) value += 360.0f;
    return value;
}

inline float clampf(float value, float lo, float hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

// ---------------------------------------------------------------------------
// Settings.
// ---------------------------------------------------------------------------

struct Settings {
    // How far the camera may swing away from the locked body angle, in
    // degrees. 180 yaw / 90 pitch is effectively unlimited.
    float maxYaw = 180.0f;
    float maxPitch = 90.0f;

    // Animate the camera back onto the body on release instead of snapping.
    // `returnLerp` is the per-tick (20 tps) share of the remaining swing.
    bool smoothReturn = true;
    float returnLerp = 0.45f;
};

// ---------------------------------------------------------------------------
// Movement frame lock.
//
// On modern Bedrock the WASD/stick input is interpreted relative to the
// camera: `MoveInputComponent` carries the camera-relative scheme in two of
// its 11 flags (`IsCameraRelativeMovementEnabled` and
// `IsRotControlledByMoveDirection`), so merely pinning the actor rotation is
// not enough — swinging the camera would drag the player in the swung
// direction while the body stays "frozen".
//
// The lock therefore also forces the input scheme to the player-relative one
// for as long as the camera is separated from the body (Active or Returning):
// W/S then follow the player's facing and A/D strafe, and nothing turns the
// body with the movement direction. Whatever the two bits were before the
// engage is remembered and restored once the camera has arrived back on the
// body, so Free Look leaves the user's control scheme exactly as it was.
// ---------------------------------------------------------------------------

class MovementFrameLock {
public:
    // MoveInputComponent::Flag::IsCameraRelativeMovementEnabled /
    // IsRotControlledByMoveDirection. Kept as plain bit numbers here so this
    // header stays free of game dependencies; the SDK enum is defined in
    // bedrocktools/sdk/input/MoveInput.hpp.
    static constexpr std::uint16_t CameraRelativeMovement = std::uint16_t{1u << 9};
    static constexpr std::uint16_t RotControlledByMoveDirection = std::uint16_t{1u << 10};
    static constexpr std::uint16_t LockMask =
        CameraRelativeMovement | RotControlledByMoveDirection;

    bool locked() const { return mLocked; }

    // One tick's worth of policy, called around `Core::preTick` with the same
    // `directing` value: while the camera and body are apart the two
    // camera-relative bits are cleared and the pre-lock value is captured on
    // the first forced tick; once they are one again the captured bits are
    // written back (the rest of the current flag value is left untouched).
    std::uint16_t tick(bool directing, std::uint16_t current) {
        if (directing) {
            if (!mLocked) {
                mSaved = current;
                mLocked = true;
            }
            return current & ~LockMask;
        }
        if (mLocked) {
            mLocked = false;
            const std::uint16_t restore = static_cast<std::uint16_t>(
                (current & ~LockMask) | (mSaved & LockMask));
            mSaved = 0;
            return restore;
        }
        return current;
    }

    // Drop the state without a restore write (player replaced, world torn
    // down): the old actor's component must never be touched again, and the
    // next engage captures the new player's scheme afresh.
    void reset() {
        mLocked = false;
        mSaved = 0;
    }

private:
    std::uint16_t mSaved = 0;
    bool mLocked = false;
};

// ---------------------------------------------------------------------------
// Swing limits.
// ---------------------------------------------------------------------------

// Trim one turn delta so the camera stays inside the swing limits. `swing` is
// the camera's current offset from `locked`; the return value is the part of
// `delta` that may still be handed to the game.
inline Turn clipTurn(const Turn& swing, const Angles& locked, const Turn& delta,
                     const Settings& settings) {
    // Pitch: the swing limit around the locked pitch *and* the absolute
    // [-90, 90] range the game itself never leaves. Both are applied to the
    // resulting camera angle, so a delta that runs into a limit is trimmed
    // down to whatever is still free instead of being dropped whole — there
    // is no dead zone to unwind when the limit lets go again.
    const float maxPitch = clampf(settings.maxPitch, 0.0f, 90.0f);
    const float camPitch = locked.pitch + swing.pitch;
    float wantPitch = camPitch + delta.pitch;
    wantPitch = clampf(wantPitch, locked.pitch - maxPitch, locked.pitch + maxPitch);
    wantPitch = clampf(wantPitch, -90.0f, 90.0f);

    // Yaw: the swing is accumulated incrementally and clamped as such. It is
    // deliberately never re-derived as wrapDegrees(camera - locked): that
    // difference flips sign at 180 degrees, so a flick into the limit would
    // hand the next delta a swing of the opposite sign and teleport the
    // camera to the far side of the lock.
    const float maxYaw = clampf(settings.maxYaw, 0.0f, 180.0f);
    const float wantSwing = clampf(swing.yaw + delta.yaw, -maxYaw, maxYaw);

    return Turn{wantPitch - camPitch, wantSwing - swing.yaw};
}

// ---------------------------------------------------------------------------
// Release animation.
// ---------------------------------------------------------------------------

// The compensating delta for one 20 tps step of the release: sent through the
// original applyTurnDelta, it moves the camera back toward the body.
// Smooth Return off means one full step — the camera lands this tick.
inline Turn returnStep(const Turn& swing, const Settings& settings) {
    // The lerp is floored so a misconfigured 0 cannot leave the body locked
    // forever.
    const float t = settings.smoothReturn ? clampf(settings.returnLerp, 0.05f, 1.0f) : 1.0f;
    return Turn{-swing.pitch * t, -swing.yaw * t};
}

// Close enough that the remaining swing can be closed out in one exact step.
inline bool swingSettled(const Turn& swing, float epsilon = 0.5f) {
    return std::fabs(swing.pitch) <= epsilon && std::fabs(swing.yaw) <= epsilon;
}

inline bool swingZero(const Turn& swing) {
    return swing.pitch == 0.0f && swing.yaw == 0.0f;
}

// ---------------------------------------------------------------------------
// Phase machine.
//
//   Inactive   - Free Look off; turn deltas pass through untouched and the
//                body is left alone.
//   Active     - turn deltas are trimmed to the swing limits and accumulated
//                into the camera swing; the body is pinned to the locked
//                angle around every tick.
//   Returning  - released, but the camera is still being walked back onto the
//                body with compensating deltas. Input stays zeroed and the
//                body stays locked until the camera arrives.
//
// The module calls preTick() before LocalPlayer::normalTick and postTick()
// after it. Both hand back the body rotation to force into the rotation
// component (before the tick so movement and the MovePlayerPacket use it,
// after the tick so the player model renders with it); postTick additionally
// hands back the compensating camera delta of the release animation.
// ---------------------------------------------------------------------------

class Core {
public:
    enum class Phase : std::uint8_t { Inactive, Active, Returning };

    struct TickOutcome {
        std::optional<Angles> body; // force this rotation on the actor
        std::optional<Turn> camera; // feed this delta to applyTurnDelta
    };

    Settings settings;

    Phase phase() const { return mPhase; }
    bool directing() const { return mPhase != Phase::Inactive; }
    bool active() const { return mPhase == Phase::Active; }
    bool returning() const { return mPhase == Phase::Returning; }

    const Angles& locked() const { return mLocked; }
    const Turn& swing() const { return mSwing; }

    // Where the camera points, as tracked from the deltas we let through.
    Angles camera() const {
        return Angles{mLocked.pitch + mSwing.pitch, wrapDegrees(mLocked.yaw + mSwing.yaw)};
    }

    void setRequestActive(bool value) { mRequestActive = value; }
    bool requestActive() const { return mRequestActive; }

    // Drop everything without compensating (player replaced, world left).
    // Only safe when the camera is about to be reset by the game anyway: the
    // camera is moved with deltas, so letting go mid-swing otherwise would
    // strand it away from the body.
    void forceInactive() {
        mPhase = Phase::Inactive;
        mSwing = Turn{};
    }

    // Hook path: returns the part of the delta the game may still apply to
    // the camera, and books it into the swing.
    Turn filterTurn(const Turn& delta) {
        if (mPhase == Phase::Active) {
            const Turn allowed = clipTurn(mSwing, mLocked, delta, settings);
            mSwing.pitch += allowed.pitch;
            mSwing.yaw += allowed.yaw;
            return allowed;
        }
        // Returning: the camera is being steered home, so fresh input is
        // swallowed until it gets there. Inactive: nothing to do with it.
        if (mPhase == Phase::Returning) return Turn{};
        return delta;
    }

    // Run at the top of every LocalPlayer tick. `current` is the body
    // rotation right now. Returns the rotation the body must carry through
    // this tick, or nullopt when Free Look is not involved at all.
    std::optional<Angles> preTick(bool moduleEnabled, const Angles& current) {
        if (mPhase == Phase::Inactive) {
            if (!moduleEnabled || !mRequestActive) return std::nullopt;
            // Engage: lock the body where it currently faces. The camera is
            // on the body at this point, so the swing starts at zero.
            mLocked = current;
            mSwing = Turn{};
            mPhase = Phase::Active;
            return mLocked;
        }

        if (moduleEnabled && mRequestActive) {
            // Still requested (or re-engaged mid-return): stay active and
            // keep the swing the camera already has.
            mPhase = Phase::Active;
            return mLocked;
        }

        // Released. The body stays locked until the camera is back on it.
        if (swingZero(mSwing)) {
            mPhase = Phase::Inactive;
        } else {
            mPhase = Phase::Returning;
        }
        return mLocked;
    }

    // Run right after the LocalPlayer tick.
    TickOutcome postTick() {
        TickOutcome outcome;
        if (mPhase == Phase::Inactive) return outcome;

        // Re-assert the lock after the tick so the player model renders with
        // the locked angle too.
        outcome.body = mLocked;

        if (mPhase == Phase::Returning) {
            Turn step = returnStep(mSwing, settings);
            Turn rest{mSwing.pitch + step.pitch, mSwing.yaw + step.yaw};
            if (swingSettled(rest)) {
                // Last step: close the gap exactly instead of crawling at it.
                step = Turn{-mSwing.pitch, -mSwing.yaw};
                rest = Turn{};
            }
            mSwing = rest;
            outcome.camera = step;
            if (swingZero(mSwing)) {
                // Camera and body are one again: unlock the body.
                mPhase = Phase::Inactive;
            }
        }
        return outcome;
    }

private:
    Phase mPhase = Phase::Inactive;
    bool mRequestActive = false;
    Angles mLocked;
    Turn mSwing;
};

} // namespace freelook

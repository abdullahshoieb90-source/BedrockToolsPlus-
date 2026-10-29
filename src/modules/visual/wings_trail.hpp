#pragma once

// Secondary motion for the Wings module.
//
// The wings are bolted to the player's back, so without this they follow the
// body exactly: they stop the instant the player stops, and they inherit every
// jump as a rigid snap. Real wings do not work that way - they have mass. This
// header is the small spring system that gives them that mass.
//
// Three effects, all driven from the same measured velocity the animation
// controller already uses:
//
//   * drag     the wings lag behind the player while moving, so they trail out
//              when running and swing back into place when the player stops,
//   * jolt     a jump (or a shove) kicks a decaying shudder that rocks the pair,
//   * lift     the same lag, applied vertically, drops the wings a little as
//              the player rises and floats them as the player falls.
//
// Everything is stored in Bedrock model pixels in the wing's own frame, so the
// geometry builder can apply it as one rigid move. The state machine is pure:
// it takes a delta time and a velocity and mutates a struct, no clocks and no
// Minecraft types, which is why the host tests can step it frame by frame.

#include "modules/visual/wings_shape.hpp"

#include <cmath>

namespace bedrocktools::modules::wings {

// How far the wings trail behind the player, as a time: the offset is the
// player's velocity multiplied by this, so it scales with speed automatically.
// 55 ms is the value where a walk still looks planted and a sprint reads as a
// real drag. With vanilla numbers a walk is 4.3 blocks/s, which gives about
// 3.7 model pixels of trail - roughly a fifth of a block.
inline constexpr float kWingTrailLagSeconds = 0.055f;

// Hard ceiling on the trail. Without it a jump would fling the wings most of a
// block away from the body, which reads as the wings detaching.
inline constexpr float kWingTrailMaxPx = 4.5f;

// Critically damped response of the trail springs. Omega is how fast the wings
// settle into their new trail (26 rad/s ~ 150 ms to get most of the way there),
// and 1.0 of critical damping is deliberate: any less and a stop would overshoot
// into a wobble, any more and the trail would feel sticky.
inline constexpr float kWingTrailOmega = 26.0f;
inline constexpr float kWingTrailDamping = 1.0f;

// The jump shudder is the opposite of the trail springs: it is meant to be
// visible, so it rings.
//
// The frequency is bounded by the tick rate, not by taste. The springs are
// stepped once per tick and the render path reads the result as-is, so the
// shudder is sampled at 20 Hz. A 4.8 Hz ring (the first value tried here) only
// gets four samples per cycle, and on screen that aliases into a random jitter
// instead of a rock. 14 rad/s is about 2.2 Hz - nine samples per cycle - which
// a tick-rate driver can actually draw. 0.18 of critical damping gives two or
// three visible rocks over roughly half a second.
inline constexpr float kWingTrailRollOmega = 14.0f;
inline constexpr float kWingTrailRollDamping = 0.18f;

// Rotation rate (degrees/s) a jump injects into the shudder. The resulting
// amplitude is roughly kick / omega, i.e. about 8 degrees, which the clamp below
// leaves alone while still catching a stacked kick.
inline constexpr float kWingTrailJumpRollKick = 120.0f;
inline constexpr float kWingTrailMaxRollDeg = 11.0f;

// Ridge angle that counts as "the shudder is at full strength", used to scale
// the extra membrane flutter.
inline constexpr float kWingTrailFullRollDeg = 7.0f;

// Fixed integration step. The springs are stiff compared to a tick, so integrating
// them once per frame would explode after a hitch (dt is clamped only at 0.25 s,
// which is six times the stable step here). Every entry point substeps at this
// rate instead.
inline constexpr float kWingTrailSubstep = 1.0f / 120.0f;

// Seconds of ring-down after which the shudder is treated as finished.
inline constexpr int kWingTrailMaxSubsteps = 32;

// Transient state of the secondary motion. All-zero is "at rest", which is what
// a freshly spawned player starts from.
struct WingTrail {
    Vec3 offsetPx{};              // current rigid offset of the wings, model px
    Vec3 ratePxPerSec{};          // its velocity, model px/s
    float rollDeg = 0.0f;         // shudder roll about the spine, degrees
    float rollRateDegPerSec = 0.0f;
    bool primed = false;          // has seen a velocity sample yet
    float prevVerticalSpeed = 0.0f;
};

// The shudder's current strength, 0..1. Derived rather than stored so it can
// never disagree with the roll it is describing.
inline float wingTrailShake(const WingTrail& t) {
    return clamp01(std::fabs(t.rollDeg) / kWingTrailFullRollDeg);
}

// World velocity (blocks/s) -> model-space velocity in model pixels per second.
//
// The wing frame is the one buildWing() works in: +x is the player's left, +y is
// up, +z is the player's back. modelPointToWorld() maps that frame with
// x -> -right and z -> -forward, so the same mapping (and its signs) is used
// here - get a sign wrong and the wings trail *into* the direction of travel.
inline Vec3 velocityToModelPx(const Vec3& velocityBlocks, const Vec3& right, const Vec3& forward) {
    constexpr float kBlocksToPx = 1.0f / kPxToBlocks;  // 16 model px per block
    const Vec3 flatRight{right.x, 0.0f, right.z};
    const Vec3 flatForward{forward.x, 0.0f, forward.z};
    return Vec3{-dot(velocityBlocks, flatRight) * kBlocksToPx,
                velocityBlocks.y * kBlocksToPx,
                -dot(velocityBlocks, flatForward) * kBlocksToPx};
}

// True when this velocity sample is a jump rather than ordinary movement: the
// upward speed jumped by a lot since the last tick. A vanilla jump changes vy by
// about 8.4 blocks/s in a single tick, while walking up a step or being nudged
// is well under 2, so 4.0 separates them with a wide margin.
inline bool wingTrailDetectsJump(const WingTrail& t, float verticalSpeed, float threshold = 4.0f) {
    if (!t.primed) return false;
    return verticalSpeed - t.prevVerticalSpeed > threshold;
}

// Advances the springs by one frame.
//
//   dtSeconds  elapsed time (clamped internally; safe to pass a hitch)
//   velocityPx world velocity already converted by velocityToModelPx()
//   jumped     this frame is a jump / shove, so kick the shudder
//
// Returns true when the wings are visually at rest, which the render path uses
// to skip the transform entirely.
inline bool stepWingTrail(WingTrail& t, float dtSeconds, const Vec3& velocityPx, bool jumped) {
    if (jumped) {
        t.rollRateDegPerSec += kWingTrailJumpRollKick;
    }

    if (dtSeconds > 0.0f) {
        const float dt = clampf(dtSeconds, 0.0f, 0.25f);

        // Target trail: opposite the motion, scaled by the lag time. This is the
        // whole "delay" - a steady run holds a steady trail, and a stop swings
        // the wings forward past neutral on their way back.
        Vec3 target{-velocityPx.x * kWingTrailLagSeconds,
                    -velocityPx.y * kWingTrailLagSeconds,
                    -velocityPx.z * kWingTrailLagSeconds};
        const float targetLen = length(target);
        if (targetLen > kWingTrailMaxPx) target = target * (kWingTrailMaxPx / targetLen);

        int steps = static_cast<int>(dt / kWingTrailSubstep) + 1;
        if (steps > kWingTrailMaxSubsteps) steps = kWingTrailMaxSubsteps;
        const float h = dt / static_cast<float>(steps);

        const float omegaSq = kWingTrailOmega * kWingTrailOmega;
        const float damp = 2.0f * kWingTrailDamping * kWingTrailOmega;
        const float rollOmegaSq = kWingTrailRollOmega * kWingTrailRollOmega;
        const float rollDamp = 2.0f * kWingTrailRollDamping * kWingTrailRollOmega;

        for (int i = 0; i < steps; ++i) {
            // Semi-implicit Euler: update the rate first, then the position, so
            // the stiff springs cannot pump energy in and drift.
            t.ratePxPerSec.x += ((target.x - t.offsetPx.x) * omegaSq - t.ratePxPerSec.x * damp) * h;
            t.ratePxPerSec.y += ((target.y - t.offsetPx.y) * omegaSq - t.ratePxPerSec.y * damp) * h;
            t.ratePxPerSec.z += ((target.z - t.offsetPx.z) * omegaSq - t.ratePxPerSec.z * damp) * h;
            t.offsetPx.x += t.ratePxPerSec.x * h;
            t.offsetPx.y += t.ratePxPerSec.y * h;
            t.offsetPx.z += t.ratePxPerSec.z * h;

            t.rollRateDegPerSec += (-t.rollDeg * rollOmegaSq - t.rollRateDegPerSec * rollDamp) * h;
            t.rollDeg += t.rollRateDegPerSec * h;
            // A second kick while already ringing can stack past the clamp; let
            // the clamp eat the rate too, or the wings would stick at the limit.
            if (t.rollDeg > kWingTrailMaxRollDeg) {
                t.rollDeg = kWingTrailMaxRollDeg;
                if (t.rollRateDegPerSec > 0.0f) t.rollRateDegPerSec = 0.0f;
            } else if (t.rollDeg < -kWingTrailMaxRollDeg) {
                t.rollDeg = -kWingTrailMaxRollDeg;
                if (t.rollRateDegPerSec < 0.0f) t.rollRateDegPerSec = 0.0f;
            }
        }
    }

    // A tiny offset is invisible at 16 px per block, so snap the last pixels away
    // instead of leaving the springs to crawl towards zero forever.
    if (length(t.offsetPx) < 0.02f && length(t.ratePxPerSec) < 0.5f) {
        t.offsetPx = Vec3{};
        t.ratePxPerSec = Vec3{};
    }
    if (std::fabs(t.rollDeg) < 0.12f && std::fabs(t.rollRateDegPerSec) < 1.5f) {
        t.rollDeg = 0.0f;
        t.rollRateDegPerSec = 0.0f;
    }

    return length(t.offsetPx) < 0.02f && t.rollDeg == 0.0f;
}

// Records the vertical speed this sample was taken at, so the next call can tell
// a jump from a walk. Kept separate from stepWingTrail() because the module needs
// the *previous* sample to decide whether this one is a jump.
inline void wingTrailRememberSample(WingTrail& t, float verticalSpeed) {
    t.prevVerticalSpeed = verticalSpeed;
    t.primed = true;
}

}  // namespace bedrocktools::modules::wings

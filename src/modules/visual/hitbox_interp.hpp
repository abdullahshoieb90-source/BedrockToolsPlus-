#pragma once

// Per-actor sample interpolation for the Hitbox overlay.
//
// The collision AABB the module reads is a tick sample (normally 20 Hz) while
// the game draws the entity mesh at a position interpolated between the last
// two samples by the partial-tick fraction. The module therefore has to draw
// its box at
//
//     lerp(prevSample, curSample, partialTick)
//
// instead of at the raw tick box, which snaps at the tick rate.
//
// The partial-tick phase is measured from the box itself: whenever an actor's
// collision box changes, that is a new sample, so the rate the samples arrive
// at is the rate the game actually runs at. An earlier revision took the phase
// from the local player's tick callback instead; whenever that callback fired
// off-cadence (which happens while gliding), the phase collapsed to zero and
// the box stayed frozen on the previous sample - invisible while walking, but
// several blocks behind a firework-boosted elytra flight.
//
// Pure functions so host tests can cover the sampling, the timing, the
// clamping and the sanity guards without the tessellator hook - see
// tests/hitbox_interp_test.cpp.

#include <bedrocktools/sdk/Types.hpp>
#include <cmath>

namespace hitbox {

// A single tick can move an entity several blocks (falling, elytra, boats,
// knockback). Anything past this is a teleport - or a bad read of a recycled
// actor pointer - and must never be interpolated across.
inline constexpr float kMaxSampleJump = 8.0f;

// Sanity bounds for the measured sample interval: a tick is 50 ms, so anything
// outside [1 ms, 500 ms] is a hitch, not a sample rate.
inline constexpr float kMinSampleInterval = 0.001f;
inline constexpr float kMaxSampleInterval = 0.5f;

// Per-actor interpolation history: the last two observed box centres and the
// interval measured between them.
struct Track {
    bedrocktools::sdk::Vec3 prevCenter{0.0f, 0.0f, 0.0f};
    bedrocktools::sdk::Vec3 curCenter{0.0f, 0.0f, 0.0f};
    float intervalSeconds = 0.05f;
    bool hasSample = false; // false until the first sample arrives
    bool hasPrev = false;   // false until a second, plausible sample arrives
};

inline bool isFinite(const bedrocktools::sdk::Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Width of one sample of movement, used to reject teleports and bad reads.
inline bool isPlausibleSampleJump(const bedrocktools::sdk::Vec3& from,
                                  const bedrocktools::sdk::Vec3& to) {
    if (!isFinite(from) || !isFinite(to)) return false;
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float dz = to.z - from.z;
    const float lengthSq = dx * dx + dy * dy + dz * dz;
    return lengthSq <= kMaxSampleJump * kMaxSampleJump;
}

// Fraction of the current sample interval that has already elapsed: 0 right
// after a sample, 1 right before the next one. Unknown or out-of-range timing
// returns 1, which leaves the newest sample in place.
inline float sampleFraction(float elapsedSeconds, float intervalSeconds) {
    if (!(intervalSeconds > 0.0f)) return 1.0f;
    if (!(elapsedSeconds > 0.0f)) return 0.0f; // also catches NaN
    const float fraction = elapsedSeconds / intervalSeconds;
    return fraction > 1.0f ? 1.0f : fraction;
}

// Translation that takes the newest sample onto the interpolated position
// lerp(prev, cur, alpha).
inline bedrocktools::sdk::Vec3 sampleOffset(const bedrocktools::sdk::Vec3& prevCenter,
                                            const bedrocktools::sdk::Vec3& curCenter,
                                            float alpha) {
    const float back = 1.0f - alpha;
    return bedrocktools::sdk::Vec3{(prevCenter.x - curCenter.x) * back,
                                   (prevCenter.y - curCenter.y) * back,
                                   (prevCenter.z - curCenter.z) * back};
}

// Feed the actor's current box centre into its history. Returns true when a
// new sample was accepted, which is the caller's signal to restart the
// interpolation phase (elapsed = 0). `elapsedSeconds` is the time since the
// previous accepted sample and only re-measures the interval.
inline bool pushSample(Track& track, const bedrocktools::sdk::Vec3& center,
                       float elapsedSeconds) {
    if (!isFinite(center)) return false;
    if (track.hasSample && center == track.curCenter) return false; // unchanged

    if (track.hasSample && isPlausibleSampleJump(track.curCenter, center)) {
        track.prevCenter = track.curCenter;
        track.hasPrev = true;
        if (elapsedSeconds >= kMinSampleInterval && elapsedSeconds <= kMaxSampleInterval) {
            track.intervalSeconds = elapsedSeconds;
        }
    } else {
        // First sample ever, or a jump too large to interpolate across:
        // keep prev == cur so the shift is a no-op for this step.
        track.prevCenter = center;
        track.hasPrev = false;
    }

    track.curCenter = center;
    track.hasSample = true;
    return true;
}

} // namespace hitbox

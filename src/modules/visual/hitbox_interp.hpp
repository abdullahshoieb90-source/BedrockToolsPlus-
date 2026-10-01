#pragma once

// Partial-tick position interpolation for the Hitbox overlay.
//
// The collision AABB the module reads is refreshed once per tick (20 Hz),
// while the game draws the entity mesh at an interpolated render position
// between the previous and the current tick position. Drawing the raw tick
// box therefore makes the box snap a whole tick of movement ahead of the
// smoothly moving body, which reads as the hitbox "lagging behind" the
// player.
//
// These helpers move the tick box back onto that render position:
//
//     renderPos = prev + (cur - prev) * alpha
//     offset    = renderPos - cur = (prev - cur) * (1 - alpha)
//
// with alpha = 0 right after a tick and 1 right before the next one, i.e.
// exactly the fraction the game uses for the entity mesh. At alpha = 1 the
// correction is zero, so unknown timing (or the menu toggle being off)
// degrades to the authoritative tick AABB.
//
// Pure functions so host tests can cover the timing, the clamping and the
// sanity guards without the tessellator hook - see tests/hitbox_interp_test.cpp.

#include <bedrocktools/sdk/Types.hpp>
#include <cmath>

namespace hitbox {

// A single tick can move an entity several blocks (falling, elytra, boats,
// knockback). Anything past this is a teleport - or a bad read of the
// previous-position field - and the box must stay on the authoritative tick
// AABB instead of being dragged across the world.
inline constexpr float kMaxTickDelta = 8.0f;

// Fraction of the current tick that has already elapsed: 0 right after a
// tick, 1 right before the next one. Unknown or out-of-range timing returns
// 1, which leaves the raw tick AABB in place.
inline float partialTickFraction(float elapsedSeconds, float tickIntervalSeconds) {
    if (!(tickIntervalSeconds > 0.0f)) return 1.0f;
    if (!(elapsedSeconds > 0.0f)) return 0.0f; // also catches NaN
    const float fraction = elapsedSeconds / tickIntervalSeconds;
    return fraction > 1.0f ? 1.0f : fraction;
}

// Translation that takes the authoritative tick AABB onto the interpolated
// render position renderPos = prev + (cur - prev) * alpha.
inline bedrocktools::sdk::Vec3 renderPositionOffset(const bedrocktools::sdk::Vec3& cur,
                                                    const bedrocktools::sdk::Vec3& prev,
                                                    float alpha) {
    const float back = 1.0f - alpha;
    return bedrocktools::sdk::Vec3{(prev.x - cur.x) * back,
                                   (prev.y - cur.y) * back,
                                   (prev.z - cur.z) * back};
}

inline bool isFinite(const bedrocktools::sdk::Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// True when an actor position belongs to the box being drawn: an actor's
// position sits inside its collision box (players stand on its bottom face,
// tall mobs keep their feet at it). A position nowhere near the box means
// the state-vector offsets do not match this build, and the caller must keep
// the raw tick AABB instead of shifting it by a made-up delta. The previous
// tick position is deliberately not checked here - a fast fall or elytra
// flight moves several blocks per tick - the tick-delta width covers that.
inline bool positionNearBox(const bedrocktools::sdk::Vec3& position,
                            const bedrocktools::sdk::Vec3& boxMin,
                            const bedrocktools::sdk::Vec3& boxMax,
                            float slack = 1.0f) {
    return position.x >= boxMin.x - slack && position.x <= boxMax.x + slack &&
           position.y >= boxMin.y - slack && position.y <= boxMax.y + slack &&
           position.z >= boxMin.z - slack && position.z <= boxMax.z + slack;
}

// Width of one tick of movement. Rejects non-finite values and teleports so
// a single bad sample can never fling the box away from the entity.
inline bool isPlausibleTickDelta(const bedrocktools::sdk::Vec3& delta) {
    if (!isFinite(delta)) return false;
    const float lengthSq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
    return lengthSq <= kMaxTickDelta * kMaxTickDelta;
}

} // namespace hitbox

#pragma once

// Pure target-selection and aim math for Kill Aura.
//
// Everything in this header operates on plain SDK value types (Vec3/AABB) so
// the rules that decide *who* gets attacked and *where* the player looks can
// be unit-tested on a host compiler without Minecraft pointers - see
// tests/killaura_logic_test.cpp. The module itself (killaura.cpp) only reads
// actor memory, applies these helpers, and calls the game's own attack path.

#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <cmath>

namespace killaura {

using bedrocktools::sdk::AABB;
using bedrocktools::sdk::Vec3;

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kRadToDeg = 180.0f / kPi;
inline constexpr float kDegToRad = kPi / 180.0f;

// Reach / size sanity bounds. Size limits reject actors that cannot be a
// living player or mob (boats are ~1.5 wide, the ender dragon aside nothing
// attackable is anywhere near 16 blocks tall), which keeps recycled-pointer
// garbage and decorative actors out of the target list.
inline constexpr float kMinBoxExtent = 0.05f;
inline constexpr float kMaxBoxExtent = 16.0f;

// The eye sits at 1.62 of the player's 1.8 height; 0.9 of the box height
// reproduces that for players and is a sane aim height for everything else.
inline constexpr float kEyeHeightFraction = 0.9f;

inline constexpr int kMinCps = 1;
inline constexpr int kMaxCps = 30;
inline constexpr float kMinRange = 1.0f;
inline constexpr float kMaxRange = 8.0f;

inline bool isFinite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// A box the aura may attack: finite, non-empty, ordered, and of a plausible
// living-actor size. The all-zero box the game reports for half-spawned or
// already-removed actors is rejected by the extent check.
inline bool isAttackableBox(const AABB& box) {
    if (!isFinite(box.min) || !isFinite(box.max)) return false;
    const float dx = box.max.x - box.min.x;
    const float dy = box.max.y - box.min.y;
    const float dz = box.max.z - box.min.z;
    return dx >= kMinBoxExtent && dy >= kMinBoxExtent && dz >= kMinBoxExtent &&
           dx <= kMaxBoxExtent && dy <= kMaxBoxExtent && dz <= kMaxBoxExtent;
}

inline Vec3 boxCenter(const AABB& box) {
    return Vec3{(box.min.x + box.max.x) * 0.5f,
                (box.min.y + box.max.y) * 0.5f,
                (box.min.z + box.max.z) * 0.5f};
}

// Where the local player's eyes are, derived from their collision box.
inline Vec3 eyePosition(const AABB& box) {
    Vec3 eye = boxCenter(box);
    eye.y = box.min.y + (box.max.y - box.min.y) * kEyeHeightFraction;
    return eye;
}

// The point of the box closest to `point` (the point itself when inside).
// This is both the reach reference and the hit position handed to the game:
// vanilla measures attack distance from the eyes to the closest point of
// the target box, not to its center.
inline Vec3 closestPointOnAabb(const Vec3& point, const AABB& box) {
    return Vec3{std::clamp(point.x, box.min.x, box.max.x),
                std::clamp(point.y, box.min.y, box.max.y),
                std::clamp(point.z, box.min.z, box.max.z)};
}

inline float distance(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

inline float distancePointToAabb(const Vec3& point, const AABB& box) {
    return distance(point, closestPointOnAabb(point, box));
}

// Bedrock look angles: pitch is stored in the rotation component's x
// (negative looks up), yaw in its y. The forward vector the game derives
// from them is (-sin(yaw)*cos(pitch), -sin(pitch), cos(yaw)*cos(pitch)) -
// the same convention the Hitbox look line uses - so these two helpers are
// exact inverses of that mapping.
struct Angles {
    float pitch = 0.0f;
    float yaw = 0.0f;
};

inline Vec3 forwardFromAngles(float pitchDegrees, float yawDegrees) {
    const float pitch = pitchDegrees * kDegToRad;
    const float yaw = yawDegrees * kDegToRad;
    return Vec3{-std::sin(yaw) * std::cos(pitch),
                -std::sin(pitch),
                std::cos(yaw) * std::cos(pitch)};
}

inline float wrapYawDegrees(float yaw) {
    yaw = std::fmod(yaw, 360.0f);
    if (yaw >= 180.0f) yaw -= 360.0f;
    if (yaw < -180.0f) yaw += 360.0f;
    return yaw;
}

inline float clampPitchDegrees(float pitch) {
    return std::clamp(pitch, -90.0f, 90.0f);
}

// Angles that face `target` from `eye`. A degenerate (zero-length) direction
// keeps the current angles, so standing inside a huge box never spins the
// camera to a NaN.
inline Angles lookAngles(const Vec3& eye, const Vec3& target, Angles fallback = {}) {
    const Vec3 dir{target.x - eye.x, target.y - eye.y, target.z - eye.z};
    const float length = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (!(length > 1e-6f) || !std::isfinite(length)) return fallback;

    Angles out;
    out.pitch = clampPitchDegrees(std::asin(std::clamp(-dir.y / length, -1.0f, 1.0f)) * kRadToDeg);
    out.yaw = wrapYawDegrees(std::atan2(-dir.x, dir.z) * kRadToDeg);
    return out;
}

inline int clampedCps(int cps) {
    return std::clamp(cps, kMinCps, kMaxCps);
}

inline float clampedRange(float range) {
    if (!std::isfinite(range)) return kMinRange;
    return std::clamp(range, kMinRange, kMaxRange);
}

// Milliseconds between attacks for a clicks-per-second setting. Integer
// division is intentional: 30 CPS -> 33 ms (the tick is 50 ms anyway, so
// anything above 20 CPS simply attacks every tick).
inline int attackIntervalMs(int cps) {
    return 1000 / clampedCps(cps);
}

} // namespace killaura

#pragma once

#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Pure geometry and animation helpers for Block Outline. Keeping these free of
// Minecraft pointers makes the renderer's coordinate/facing rules host-testable.
namespace blockoutline {

using bedrocktools::sdk::BlockPos;
using bedrocktools::sdk::Vec3;

struct Box {
    Vec3 min;
    Vec3 max;
};

struct Edge {
    Vec3 from;
    Vec3 to;
};

using Face = std::array<Vec3, 4>;

// Bedrock's Facing enum, as stored in HitResult::mFacing.
enum Facing : int {
    Down = 0,
    Up = 1,
    North = 2,
    South = 3,
    West = 4,
    East = 5,
};

inline constexpr bool validFacing(int facing) {
    return facing >= Down && facing <= East;
}

inline constexpr Box makeBlockBox(const BlockPos& position, float expansion = 0.0f) {
    return {
        {static_cast<float>(position.x) - expansion,
         static_cast<float>(position.y) - expansion,
         static_cast<float>(position.z) - expansion},
        {static_cast<float>(position.x + 1) + expansion,
         static_cast<float>(position.y + 1) + expansion,
         static_cast<float>(position.z + 1) + expansion},
    };
}

inline constexpr std::array<Edge, 12> boxEdges(const Box& box) {
    const Vec3& mn = box.min;
    const Vec3& mx = box.max;
    return {{
        // Bottom ring.
        {{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}},
        {{mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z}},
        {{mx.x, mn.y, mx.z}, {mn.x, mn.y, mx.z}},
        {{mn.x, mn.y, mx.z}, {mn.x, mn.y, mn.z}},
        // Top ring.
        {{mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z}},
        {{mx.x, mx.y, mn.z}, {mx.x, mx.y, mx.z}},
        {{mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}},
        {{mn.x, mx.y, mx.z}, {mn.x, mx.y, mn.z}},
        // Vertical edges.
        {{mn.x, mn.y, mn.z}, {mn.x, mx.y, mn.z}},
        {{mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}},
        {{mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}},
        {{mn.x, mn.y, mx.z}, {mn.x, mx.y, mx.z}},
    }};
}

inline constexpr Face boxFace(const Box& box, int facing) {
    const Vec3& mn = box.min;
    const Vec3& mx = box.max;
    switch (facing) {
        case Down:
            return {{{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z},
                     {mx.x, mn.y, mx.z}, {mn.x, mn.y, mx.z}}};
        case Up:
            return {{{mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z},
                     {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}}};
        case North:
            return {{{mn.x, mn.y, mn.z}, {mn.x, mx.y, mn.z},
                     {mx.x, mx.y, mn.z}, {mx.x, mn.y, mn.z}}};
        case South:
            return {{{mn.x, mn.y, mx.z}, {mn.x, mx.y, mx.z},
                     {mx.x, mx.y, mx.z}, {mx.x, mn.y, mx.z}}};
        case West:
            return {{{mn.x, mn.y, mn.z}, {mn.x, mx.y, mn.z},
                     {mn.x, mx.y, mx.z}, {mn.x, mn.y, mx.z}}};
        case East:
            return {{{mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z},
                     {mx.x, mx.y, mx.z}, {mx.x, mn.y, mx.z}}};
        default:
            return {};
    }
}

inline constexpr std::array<Face, 6> boxFaces(const Box& box) {
    return {{
        boxFace(box, Down),
        boxFace(box, Up),
        boxFace(box, North),
        boxFace(box, South),
        boxFace(box, West),
        boxFace(box, East),
    }};
}

// Saturation/value are both 1. Hue wraps, so animation can pass an unbounded
// number of degrees without accumulating special cases in the renderer.
inline std::uint32_t hsvToRgb(float hueDegrees) {
    float hue = std::fmod(hueDegrees, 360.0f);
    if (hue < 0.0f) hue += 360.0f;

    const float x = 1.0f - std::fabs(std::fmod(hue / 60.0f, 2.0f) - 1.0f);
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    if (hue < 60.0f) {
        r = 1.0f; g = x;
    } else if (hue < 120.0f) {
        r = x; g = 1.0f;
    } else if (hue < 180.0f) {
        g = 1.0f; b = x;
    } else if (hue < 240.0f) {
        g = x; b = 1.0f;
    } else if (hue < 300.0f) {
        r = x; b = 1.0f;
    } else {
        r = 1.0f; b = x;
    }

    return (static_cast<std::uint32_t>(r * 255.0f + 0.5f) << 16) |
           (static_cast<std::uint32_t>(g * 255.0f + 0.5f) << 8) |
            static_cast<std::uint32_t>(b * 255.0f + 0.5f);
}

inline std::uint32_t animatedRgb(std::uint32_t configuredColor,
                                 bool rainbow,
                                 double seconds,
                                 float speed) {
    if (!rainbow) return configuredColor & 0x00FFFFFFu;
    const float safeSpeed = std::clamp(speed, 0.05f, 1.0f);
    return hsvToRgb(static_cast<float>(seconds * static_cast<double>(safeSpeed) * 360.0));
}

// A pulse never disappears completely; keeping at least 42% opacity avoids a
// distracting blink while still making the animation clearly visible.
inline float pulseMultiplier(bool pulse, double seconds, float speed) {
    if (!pulse) return 1.0f;
    constexpr double kTau = 6.28318530717958647692;
    const double safeSpeed = static_cast<double>(std::clamp(speed, 0.05f, 1.0f));
    const float wave = static_cast<float>((std::sin(seconds * safeSpeed * kTau) + 1.0) * 0.5);
    return 0.42f + 0.58f * wave;
}

inline float clampedOpacity(float opacity, float multiplier = 1.0f) {
    return std::clamp(opacity, 0.0f, 1.0f) * std::clamp(multiplier, 0.0f, 1.0f);
}

} // namespace blockoutline

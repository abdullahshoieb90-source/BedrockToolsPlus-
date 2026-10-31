#pragma once

#include <bedrocktools/sdk/Types.hpp>

#include <array>
#include <cmath>
#include <optional>

// Engine-independent building blocks for a future spatial container UI. The
// panel is fixed in world space after creation; UV (0,0) is its top-left.
namespace bedrocktools::spatialgui {
using sdk::Vec2;
using sdk::Vec3;

inline Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 mul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}
inline bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline std::optional<Vec3> unit(Vec3 v) {
    if (!finite(v)) return std::nullopt;
    const float length2 = dot(v, v);
    if (!std::isfinite(length2) || length2 < 1e-12f) return std::nullopt;
    return mul(v, 1.0f / std::sqrt(length2));
}

struct Panel {
    Vec3 center;
    Vec3 right; // unit vectors, perpendicular to each other
    Vec3 up;
    float width;
    float height;
};

// Create a vertical panel facing the viewer, with the orientation captured at
// open time. A directly vertical viewing direction has no stable horizontal
// facing and is rejected rather than producing NaNs.
inline std::optional<Panel> makePanel(Vec3 center, Vec3 viewer, float width, float height) {
    if (!finite(center) || !finite(viewer) || !std::isfinite(width) ||
        !std::isfinite(height) || width <= 0 || height <= 0) return std::nullopt;
    Vec3 facing = sub(viewer, center);
    facing.y = 0;
    const auto normal = unit(facing);
    if (!normal) return std::nullopt;
    constexpr Vec3 up{0, 1, 0};
    const auto right = unit(cross(up, *normal));
    if (!right) return std::nullopt;
    return Panel{center, *right, up, width, height};
}

// Top-left, top-right, bottom-right, bottom-left in world space.
inline std::array<Vec3, 4> corners(const Panel& p) {
    const auto r = mul(p.right, p.width * 0.5f);
    const auto u = mul(p.up, p.height * 0.5f);
    return {add(sub(p.center, r), u), add(add(p.center, r), u),
            sub(add(p.center, r), u), sub(sub(p.center, r), u)};
}

// Intersect a world-space ray with the front of the panel. Reject back faces,
// parallel rays, intersections behind the origin, and points outside it.
// Direction need not be normalized. Edge coordinates 0 and 1 are inclusive.
inline std::optional<Vec2> hitTest(const Panel& p, Vec3 origin, Vec3 direction) {
    if (!finite(p.center) || !finite(p.right) || !finite(p.up) || !finite(origin) ||
        !finite(direction) || !std::isfinite(p.width) || !std::isfinite(p.height) ||
        p.width <= 0 || p.height <= 0) return std::nullopt;
    const auto ray = unit(direction);
    if (!ray) return std::nullopt;
    const auto normal = unit(cross(p.right, p.up));
    if (!normal) return std::nullopt;
    const float denom = dot(*ray, *normal);
    if (denom >= -1e-6f) return std::nullopt;
    const float distance = dot(sub(p.center, origin), *normal) / denom;
    if (!std::isfinite(distance) || distance < 0) return std::nullopt;
    const Vec3 offset = sub(add(origin, mul(*ray, distance)), p.center);
    const float u = dot(offset, p.right) / p.width + 0.5f;
    const float v = 0.5f - dot(offset, p.up) / p.height;
    if (!std::isfinite(u) || !std::isfinite(v) || u < 0 || u > 1 || v < 0 || v > 1)
        return std::nullopt;
    return Vec2{u, v};
}
} // namespace bedrocktools::spatialgui

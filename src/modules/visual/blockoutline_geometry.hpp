#pragma once

#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

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

// Flattens box outlines into the line segments a renderer submits: twelve edges
// per box, in the order boxEdges() returns them. `out` is reused across frames
// so the flattening does not allocate once the buffer has grown to size.
inline void collectBoxEdges(const std::vector<Box>& boxes, std::vector<Edge>& out) {
    out.clear();
    out.reserve(boxes.size() * 12);
    for (const auto& box : boxes) {
        for (const auto& edge : boxEdges(box)) out.push_back(edge);
    }
}

// Camera-facing beam around an edge, used wherever a line has to be wider than
// a hairline: GLES drivers on Android ignore glLineWidth, so each edge becomes
// a quad whose width follows the thickness setting. `halfWidth` is in world
// units and `out` holds the four corners relative to the camera (the space the
// tessellator works in) in one winding; callers emit the reverse winding too so
// materials with back-face culling keep the strip. Ends overshoot by half the
// width so neighboring edges of a box stay closed. Returns false only for a
// zero-length edge, which cannot produce a quad.
inline bool makeEdgeBeam(const Edge& edge,
                         const Vec3& camera,
                         float halfWidth,
                         std::array<Vec3, 4>& out) {
    const Vec3 p1{edge.from.x - camera.x, edge.from.y - camera.y, edge.from.z - camera.z};
    const Vec3 p2{edge.to.x - camera.x, edge.to.y - camera.y, edge.to.z - camera.z};

    float dx = p2.x - p1.x;
    float dy = p2.y - p1.y;
    float dz = p2.z - p1.z;
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length < 0.00001f) return false;
    dx /= length;
    dy /= length;
    dz /= length;

    // The camera is the origin in this space, so the vector to the edge
    // midpoint is the view direction: dir x midpoint is perpendicular to both.
    const float mx = (p1.x + p2.x) * 0.5f;
    const float my = (p1.y + p2.y) * 0.5f;
    const float mz = (p1.z + p2.z) * 0.5f;
    float sx = dy * mz - dz * my;
    float sy = dz * mx - dx * mz;
    float sz = dx * my - dy * mx;
    float sideLength = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (sideLength < 0.00001f) {
        // Looking directly along an edge: choose a stable arbitrary
        // perpendicular instead of dropping that edge for one frame.
        if (std::fabs(dy) < 0.9f) {
            sx = -dz; sy = 0.0f; sz = dx;
        } else {
            sx = 1.0f; sy = 0.0f; sz = 0.0f;
        }
        sideLength = std::sqrt(sx * sx + sy * sy + sz * sz);
        if (sideLength < 0.00001f) return false;
    }
    sx = sx / sideLength * halfWidth;
    sy = sy / sideLength * halfWidth;
    sz = sz / sideLength * halfWidth;

    const float ex = dx * halfWidth;
    const float ey = dy * halfWidth;
    const float ez = dz * halfWidth;
    out = {{
        {p1.x - ex - sx, p1.y - ey - sy, p1.z - ez - sz},
        {p2.x + ex - sx, p2.y + ey - sy, p2.z + ez - sz},
        {p2.x + ex + sx, p2.y + ey + sy, p2.z + ez + sz},
        {p1.x - ex + sx, p1.y - ey + sy, p1.z - ez + sz},
    }};
    return true;
}

// Half width for a line that has to keep the same size on screen at any range:
// the world-space width grows with how far the point is from the camera, so a
// tracer that spans tens of blocks does not shrink to a sub-pixel hair at its
// far end (which is what a constant world width does). `minDistance` floors the
// growth so an end that sits next to the eye cannot blow up into a wedge.
inline float screenConstantHalfWidth(float halfWidthAtOneBlock,
                                     const Vec3& point,
                                     const Vec3& camera,
                                     float minDistance) {
    const float dx = point.x - camera.x;
    const float dy = point.y - camera.y;
    const float dz = point.z - camera.z;
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    return halfWidthAtOneBlock * std::max(distance, minDistance);
}

// Everything the tapered builder needs: the segment in camera-relative space
// plus the direction its cross-section widens in.
struct TaperedAxis {
    Vec3 from{};
    Vec3 to{};
    Vec3 dir{};   // unit, from -> to
    Vec3 side{};  // unit, perpendicular to dir
};

// Resolves a segment into that axis. `side` is the direction a viewer sees the
// line widen in: perpendicular to the segment and to the eye ray that reaches
// the segment's midpoint, so the ribbon built from it faces the camera. One
// such direction is used for the whole ribbon rather than one per end, which
// keeps its four corners coplanar and its two long edges from crossing: a
// per-end perpendicular is unstable exactly where a tracer lives, because the
// eye ray to a snapline's own end is nearly parallel to the segment itself.
inline bool taperedAxis(const Edge& edge, const Vec3& camera, TaperedAxis& axis) {
    axis.from = {edge.from.x - camera.x, edge.from.y - camera.y, edge.from.z - camera.z};
    axis.to = {edge.to.x - camera.x, edge.to.y - camera.y, edge.to.z - camera.z};

    float dx = axis.to.x - axis.from.x;
    float dy = axis.to.y - axis.from.y;
    float dz = axis.to.z - axis.from.z;
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length < 0.00001f) return false;
    dx /= length;
    dy /= length;
    dz /= length;
    axis.dir = {dx, dy, dz};

    // The camera is the origin of this space, so dir x (vector to the midpoint)
    // is perpendicular to both the segment and the eye ray through it: the
    // direction a viewer sees the line widen in.
    const float mx = (axis.from.x + axis.to.x) * 0.5f;
    const float my = (axis.from.y + axis.to.y) * 0.5f;
    const float mz = (axis.from.z + axis.to.z) * 0.5f;
    float sx = dy * mz - dz * my;
    float sy = dz * mx - dx * mz;
    float sz = dx * my - dy * mx;
    float sideLength = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (sideLength < 0.00001f) {
        // Looking straight along the segment: choose a stable arbitrary
        // perpendicular instead of dropping the ribbon for one frame.
        if (std::fabs(dy) < 0.9f) {
            sx = -dz; sy = 0.0f; sz = dx;
        } else {
            sx = 1.0f; sy = 0.0f; sz = 0.0f;
        }
        sideLength = std::sqrt(sx * sx + sy * sy + sz * sz);
        if (sideLength < 0.00001f) return false;
    }
    axis.side = {sx / sideLength, sy / sideLength, sz / sideLength};
    return true;
}

// One ribbon of a cross-section: the segment widened by `side`, with an
// independent half width at each end. That independence is what keeps a long
// line one steady width on screen instead of thinning to a sub-pixel hair at
// range. Corners come back near-far-far-near in one winding, both ends
// overshooting by their own half width so the strip stays closed.
inline std::array<Vec3, 4> taperedRibbon(const TaperedAxis& axis,
                                         const Vec3& side,
                                         float halfWidthFrom,
                                         float halfWidthTo) {
    const Vec3 overshootFrom{axis.dir.x * halfWidthFrom, axis.dir.y * halfWidthFrom,
                             axis.dir.z * halfWidthFrom};
    const Vec3 overshootTo{axis.dir.x * halfWidthTo, axis.dir.y * halfWidthTo,
                           axis.dir.z * halfWidthTo};
    return {{
        {axis.from.x - overshootFrom.x - side.x * halfWidthFrom,
         axis.from.y - overshootFrom.y - side.y * halfWidthFrom,
         axis.from.z - overshootFrom.z - side.z * halfWidthFrom},
        {axis.to.x + overshootTo.x - side.x * halfWidthTo,
         axis.to.y + overshootTo.y - side.y * halfWidthTo,
         axis.to.z + overshootTo.z - side.z * halfWidthTo},
        {axis.to.x + overshootTo.x + side.x * halfWidthTo,
         axis.to.y + overshootTo.y + side.y * halfWidthTo,
         axis.to.z + overshootTo.z + side.z * halfWidthTo},
        {axis.from.x - overshootFrom.x + side.x * halfWidthFrom,
         axis.from.y - overshootFrom.y + side.y * halfWidthFrom,
         axis.from.z - overshootFrom.z + side.z * halfWidthFrom},
    }};
}

// Camera-facing ribbon with an independent half width at each end, i.e. a
// tapered strip. Returns false only for a zero-length segment.
inline bool makeTaperedBeam(const Edge& edge,
                            const Vec3& camera,
                            float halfWidthFrom,
                            float halfWidthTo,
                            std::array<Vec3, 4>& out) {
    TaperedAxis axis{};
    if (!taperedAxis(edge, camera, axis)) return false;
    out = taperedRibbon(axis, axis.side, halfWidthFrom, halfWidthTo);
    return true;
}

// Saturation/value are both 1. Hue wraps so animation can pass an unbounded
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

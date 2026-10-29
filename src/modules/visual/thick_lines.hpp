#pragma once

// Shared thick-line rendering for the world-space overlays (Block Outline,
// Hitbox). Mobile GLES drivers ignore glLineWidth, so every "thick line" is
// real geometry: one camera-facing quad per segment, emitted with both
// windings so back-face culling can never hide one.
//
// Corners: extending a segment quad past its endpoint by half a width only
// closes a joint when the neighbor continues straight. Two segments meeting
// at a right angle (every box corner) each push their own half width beyond
// the corner in different directions, which reads as overlapping spurs / a
// pinwheel at the corners. The quads are therefore cut square at their
// endpoints, and every endpoint shared by two or more segments gets a small
// camera-facing square "joint dot" centered on the corner. The dot covers
// the differing sideways offsets of both segment ends, so corners read as
// clean right angles from any viewing angle.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <bedrocktools/sdk/Types.hpp>

namespace thicklines {

using bedrocktools::sdk::Vec3;

// Joint dots are a touch larger than the line half width so the square-cut
// ends of both meeting segments stay covered from every viewing angle.
inline constexpr float kJointDotScale = 1.05f;

inline Vec3 subtract(const Vec3& a, const Vec3& b) {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x};
}

inline float length(const Vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

// Orthonormal basis of the plane through `point` perpendicular to the view
// ray. The joint dot is drawn in this plane, so it always faces the camera.
inline void cameraPlaneBasis(const Vec3& point, const Vec3& cam,
                             Vec3& right, Vec3& up) {
    Vec3 view = subtract(point, cam);
    if (length(view) < 1e-6f) view = Vec3{0.0f, 0.0f, -1.0f};

    Vec3 referenceUp{0.0f, 1.0f, 0.0f};
    if (std::fabs(view.y) > 0.999f) referenceUp = Vec3{0.0f, 0.0f, 1.0f};

    right = cross(view, referenceUp);
    float rightLen = length(right);
    if (rightLen < 1e-6f) {
        right = Vec3{1.0f, 0.0f, 0.0f};
        rightLen = 1.0f;
    }
    right = Vec3{right.x / rightLen, right.y / rightLen, right.z / rightLen};
    up = cross(right, view);
    const float upLen = length(up);
    if (upLen > 1e-6f) {
        up = Vec3{up.x / upLen, up.y / upLen, up.z / upLen};
    }
}

// One camera-facing quad per segment (both windings, 8 vertices), relative
// to the camera like the rest of the mesh. The quad is centered on the
// segment's centerline and cut square at the endpoints - joints are closed
// by emitJointDot, so no geometry sticks out past a corner any more.
template <class Emit>
inline void emitSegment(const Vec3& world1,
                        const Vec3& world2,
                        const Vec3& cam,
                        float halfWidth,
                        Emit&& emit) {
    const Vec3 p1 = subtract(world1, cam);
    const Vec3 p2 = subtract(world2, cam);

    Vec3 dir = subtract(p2, p1);
    const float len = length(dir);
    if (len < 1e-5f) return;
    dir = Vec3{dir.x / len, dir.y / len, dir.z / len};

    // The camera sits at the origin of this camera-relative space, so the
    // vector to the segment midpoint is the view ray. side = dir x view is
    // perpendicular to both the segment and the eye ray => the quad always
    // faces the player.
    const Vec3 mid{(p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f, (p1.z + p2.z) * 0.5f};
    Vec3 side = cross(dir, mid);
    float sideLen = length(side);
    if (sideLen < 1e-5f) {
        // Looking straight down the segment: pick any stable perpendicular.
        if (std::fabs(dir.y) < 0.9f) {
            side = Vec3{-dir.z, 0.0f, dir.x};
        } else {
            side = Vec3{1.0f, 0.0f, 0.0f};
        }
        sideLen = length(side);
        if (sideLen < 1e-5f) return;
    }
    side = Vec3{side.x / sideLen * halfWidth,
                side.y / sideLen * halfWidth,
                side.z / sideLen * halfWidth};

    const Vec3 quad[4] = {
        Vec3{p1.x - side.x, p1.y - side.y, p1.z - side.z},
        Vec3{p2.x - side.x, p2.y - side.y, p2.z - side.z},
        Vec3{p2.x + side.x, p2.y + side.y, p2.z + side.z},
        Vec3{p1.x + side.x, p1.y + side.y, p1.z + side.z},
    };
    for (int i = 0; i < 4; ++i) emit(quad[i]);
    for (int i = 3; i >= 0; --i) emit(quad[i]);
}

// A small camera-facing square centered on a point where segments meet
// (both windings, 8 vertices). It covers the differing sideways offsets of
// the meeting segment ends, so corners stay solid right angles instead of
// growing spurs.
template <class Emit>
inline void emitJointDot(const Vec3& point, const Vec3& cam,
                         float halfWidth, Emit&& emit) {
    Vec3 right;
    Vec3 up;
    cameraPlaneBasis(point, cam, right, up);
    const float extent = halfWidth * kJointDotScale;
    const Vec3 p = subtract(point, cam);

    const Vec3 quad[4] = {
        Vec3{p.x - right.x * extent - up.x * extent,
             p.y - right.y * extent - up.y * extent,
             p.z - right.z * extent - up.z * extent},
        Vec3{p.x - right.x * extent + up.x * extent,
             p.y - right.y * extent + up.y * extent,
             p.z - right.z * extent + up.z * extent},
        Vec3{p.x + right.x * extent + up.x * extent,
             p.y + right.y * extent + up.y * extent,
             p.z + right.z * extent + up.z * extent},
        Vec3{p.x + right.x * extent - up.x * extent,
             p.y + right.y * extent - up.y * extent,
             p.z + right.z * extent - up.z * extent},
    };
    for (int i = 0; i < 4; ++i) emit(quad[i]);
    for (int i = 3; i >= 0; --i) emit(quad[i]);
}

// A resolution far below any visible line width; identical mesh corners
// quantize to the same key.
struct PointKey {
    std::int64_t x, y, z;
    bool operator==(const PointKey& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

inline PointKey keyOf(const Vec3& p) {
    return PointKey{std::llround(static_cast<double>(p.x) * 8192.0),
                    std::llround(static_cast<double>(p.y) * 8192.0),
                    std::llround(static_cast<double>(p.z) * 8192.0)};
}

// Endpoints shared by two or more segments: the corners and junctions that
// need a joint dot. An endpoint touched by only one segment (for example
// the tip of the look line) stays a clean square cut. Accepts any iterable
// of two-Vec3 aggregates (std::pair, the geometry Edge struct, ...).
template <class Segments>
inline std::vector<Vec3> collectJoints(const Segments& segments) {
    struct Entry {
        PointKey key;
        Vec3 point;
        int count;
    };
    std::vector<Entry> entries;
    entries.reserve(segments.size() * 2);

    for (const auto& segment : segments) {
        const auto& [from, to] = segment;
        const Vec3 endpoints[2] = {from, to};
        for (const Vec3& endpoint : endpoints) {
            const PointKey key = keyOf(endpoint);
            bool found = false;
            for (auto& entry : entries) {
                if (entry.key == key) {
                    ++entry.count;
                    found = true;
                    break;
                }
            }
            if (!found) entries.push_back(Entry{key, endpoint, 1});
        }
    }

    std::vector<Vec3> joints;
    for (const auto& entry : entries) {
        if (entry.count >= 2) joints.push_back(entry.point);
    }
    return joints;
}

// Total vertices the thick pass emits: both windings of every segment quad
// and every joint dot. Use as the tessellator's reserved-vertex count.
inline int thickVertexCount(std::size_t segments, std::size_t joints) {
    return static_cast<int>((segments + joints) * 8);
}

} // namespace thicklines

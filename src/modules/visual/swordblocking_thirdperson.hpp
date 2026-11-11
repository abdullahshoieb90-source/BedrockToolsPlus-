#pragma once

// Third-person guard, pure geometry.
//
// First person works by posing the item matrix the game itself builds. Third
// person is different: the game draws the carried item as part of the player
// model through the actor renderer, and none of the ItemInHandRenderer hooks
// see it, so there is no matrix to borrow (the same reason the upstream view
// model module is first-person only).
//
// What the module does instead is draw the guard itself: two flat, textured
// quads - the sword sprite on both faces of one blade, plus a small guard cross
// - placed in the player's right hand in world space. It is a representation of
// the block, not a replacement weapon: the vanilla held item is still drawn,
// and everything here is client-side and depth tested, so it can never look
// like it reaches through a wall.
//
// Everything that decides where the blade goes lives in this header, so
// tests/swordblocking_test.cpp can check the geometry without a game.

#include "swordblocking_logic.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <bedrocktools/sdk/Types.hpp>

namespace swordblocking {

using bedrocktools::sdk::Vec3;

// ---------------------------------------------------------------------------
// Hand anchor
// ---------------------------------------------------------------------------

// Model-space offset of the right hand along the body's right/up/forward axes.
// The player model is 32 pixels tall (1.8 blocks), the shoulder sits at about
// y = 24 and the hand hangs around y = 12-14 at rest; the block pose pulls it up
// in front of the chest. Distances are model pixels, /16 for blocks.
struct HandOffset {
    float right = 3.0f;
    float up = 15.0f;
    float forward = 5.5f;
};

struct BodyFrame {
    Vec3 origin{}; // feet position in world space
    float yawDegrees = 0.0f;
};

inline Vec3 add(const Vec3& a, const Vec3& b) {
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 scale(const Vec3& v, float factor) {
    return Vec3{v.x * factor, v.y * factor, v.z * factor};
}

// Body right/up/forward for a Bedrock actor yaw, which is measured in degrees
// clockwise from -Z (+Z is yaw 180), i.e. the same convention the game uses for
// the player model and the wings module.
inline void bodyAxes(float yawDegrees, Vec3& right, Vec3& up, Vec3& forward) {
    const float radians = yawDegrees * 3.14159265358979323846f / 180.0f;
    const float s = std::sin(radians);
    const float c = std::cos(radians);
    // forward = (-sin, 0, cos) points where the actor looks.
    forward = Vec3{-s, 0.0f, c};
    right = Vec3{c, 0.0f, s};
    up = Vec3{0.0f, 1.0f, 0.0f};
}

inline Vec3 handPosition(const BodyFrame& body, const HandOffset& offset) {
    Vec3 right;
    Vec3 up;
    Vec3 forward;
    bodyAxes(body.yawDegrees, right, up, forward);
    constexpr float kPixelsPerBlock = 16.0f;
    Vec3 position = body.origin;
    position = add(position, scale(right, offset.right / kPixelsPerBlock));
    position = add(position, scale(up, offset.up / kPixelsPerBlock));
    position = add(position, scale(forward, offset.forward / kPixelsPerBlock));
    return position;
}

// ---------------------------------------------------------------------------
// Blade orientation
// ---------------------------------------------------------------------------

// The guard is described in the plane the player sees from third person: the
// blade points along `angleDegrees` measured from the body's forward axis,
// positive toward the body's right, and `leanDegrees` tips the blade toward the
// player (0 = flat against the screen, 90 = pointing at the camera).
struct BladeOrientation {
    float angleDegrees = 62.0f; // crosses the screen to the left (negative: right)
    float leanDegrees = 12.0f;  // a gentle tip toward the viewer
};

// Orthonormal basis of the blade: `axis` runs from grip to tip, `face` is the
// blade's normal (the direction the flat of the sword faces).
inline void bladeBasis(const BodyFrame& body, const BladeOrientation& orientation,
                       Vec3& axis, Vec3& face, Vec3& edge) {
    Vec3 right;
    Vec3 up;
    Vec3 forward;
    bodyAxes(body.yawDegrees, right, up, forward);

    const float angle = orientation.angleDegrees * 3.14159265358979323846f / 180.0f;
    const float spread = std::sin(angle);
    const float rise = std::cos(angle);

    // Start in the plane facing the viewer, then lean the blade forward. With
    // the default third-person camera (behind the player) a positive angle
    // carries the blade across the screen to the left, the way the first-person
    // guard leans; a negative angle mirrors it, i.e. Up Right / Down Right.
    Vec3 planned{
        -forward.x * rise - right.x * spread,
        up.y * rise,
        -forward.z * rise - right.z * spread,
    };
    const float length = std::sqrt(planned.x * planned.x + planned.y * planned.y + planned.z * planned.z);
    if (length > 1e-6f) planned = scale(planned, 1.0f / length);

    // The flat of the blade starts facing the camera (opposite the body's
    // forward) and is then rotated with the blade's lean.
    const float lean = orientation.leanDegrees * 3.14159265358979323846f / 180.0f;
    Vec3 flat{-forward.x, 0.0f, -forward.z};
    // Component of the lean that tips the blade toward the viewer.
    flat = Vec3{flat.x * std::cos(lean) + planned.x * std::sin(lean),
                0.0f + planned.y * std::sin(lean),
                flat.z * std::cos(lean) + planned.z * std::sin(lean)};

    axis = planned;
    face = flat;
    // Re-orthogonalise: the flat must stay perpendicular to the blade.
    const float dot = face.x * axis.x + face.y * axis.y + face.z * axis.z;
    face = Vec3{face.x - axis.x * dot, face.y - axis.y * dot, face.z - axis.z * dot};
    const float faceLength = std::sqrt(face.x * face.x + face.y * face.y + face.z * face.z);
    if (faceLength > 1e-6f) face = scale(face, 1.0f / faceLength);
    else {
        // Degenerate (blade pointing straight at the camera): pick any
        // perpendicular so the quads stay well formed.
        face = Vec3{-axis.y, axis.x, 0.0f};
        const float fallback = std::sqrt(face.x * face.x + face.y * face.y + face.z * face.z);
        if (fallback > 1e-6f) face = scale(face, 1.0f / fallback);
    }
    edge = Vec3{axis.y * face.z - axis.z * face.y,
                axis.z * face.x - axis.x * face.z,
                axis.x * face.y - axis.y * face.x};
}

// ---------------------------------------------------------------------------
// Quads
// ---------------------------------------------------------------------------

struct Quad {
    std::array<Vec3, 4> points; // counter-clockwise seen from `face`
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
    std::uint8_t shade = 0; // 0 = item texture, 1 = guard cross colour
};

// The sword sprite occupies these UVs of the vanilla items atlas.
struct BladeStyle {
    float u0 = 0.0f;   // netherite_sword sprite in items.png
    float v0 = 0.00390625f;
    float u1 = 0.0625f;
    float v1 = 0.06640625f;
};

// Blade length in blocks. A Bedrock sword is 16x16 pixels of sprite on a
// 0.5 pixel-thick plate, drawn at the held scale, which reads as roughly
// 0.9 blocks from grip to tip.
struct BladeGeometry {
    float length = 0.9f;
    float width = 0.13f;
    float crossLength = 0.26f;
    float crossWidth = 0.05f;
};

// Two quads per blade face and one small cross, in world space, at `weight`.
// Everything scales with the weight so the guard grows out of the hand instead
// of appearing at full size on the first frame.
inline std::vector<Quad> guardQuads(const BodyFrame& body,
                                    const HandOffset& offset,
                                    const BladeOrientation& orientation,
                                    const BladeGeometry& geometry,
                                    const BladeStyle& style,
                                    float weight) {
    std::vector<Quad> quads;
    const float w = std::isfinite(weight) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
    if (w <= activeThreshold) return quads;

    const Vec3 grip = handPosition(body, offset);
    Vec3 axis;
    Vec3 face;
    Vec3 edge;
    bladeBasis(body, orientation, axis, face, edge);

    const float length = geometry.length * w;
    const float width = geometry.width * w;

    // The sprite's own origin is at the grip end, so the quad starts there.
    const Vec3 tip = add(grip, scale(axis, length));
    const Vec3 halfEdge = scale(edge, width * 0.5f);

    Quad blade;
    blade.points = {add(grip, halfEdge), add(tip, halfEdge), add(tip, scale(halfEdge, -1.0f)),
                    add(grip, scale(halfEdge, -1.0f))};
    blade.u0 = style.u0;
    blade.v0 = style.v0;
    blade.u1 = style.u1;
    blade.v1 = style.v1;
    quads.push_back(blade);

    // The cross guard, drawn a little before the grip so it reads as a hilt.
    const Vec3 crossCenter = add(grip, scale(axis, geometry.crossLength * 0.35f * w));
    const Vec3 crossHalf = scale(edge, geometry.crossLength * 0.5f * w);
    const Vec3 crossHalfWidth = scale(axis, geometry.crossWidth * 0.5f * w);

    Quad cross;
    cross.points = {add(add(crossCenter, crossHalf), crossHalfWidth),
                    add(add(crossCenter, scale(crossHalf, -1.0f)), crossHalfWidth),
                    add(add(crossCenter, scale(crossHalf, -1.0f)), scale(crossHalfWidth, -1.0f)),
                    add(add(crossCenter, crossHalf), scale(crossHalfWidth, -1.0f))};
    cross.shade = 1;
    quads.push_back(cross);

    return quads;
}

// Vertex count a batch of quads costs, for the tessellator's bookkeeping.
inline std::size_t quadVertexCount(const std::vector<Quad>& quads) {
    return quads.size() * 4;
}

} // namespace swordblocking

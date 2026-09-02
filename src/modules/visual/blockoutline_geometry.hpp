#pragma once

#include <array>

namespace bedrocktools::modules::blockoutline {

struct Point {
    float x;
    float y;
    float z;

    constexpr bool operator==(const Point&) const = default;
};

struct Line {
    Point from;
    Point to;
};

struct Quad {
    Point a;
    Point b;
    Point c;
    Point d;
};

// Builds the twelve edges of a block-sized axis-aligned box.  Keeping this
// geometry independent from Minecraft makes it easy to verify on the host and
// avoids rebuilding edge topology every frame.
constexpr std::array<Line, 12> makeBox(float x, float y, float z, float expand = 0.002f) {
    const float x0 = x - expand;
    const float y0 = y - expand;
    const float z0 = z - expand;
    const float x1 = x + 1.0f + expand;
    const float y1 = y + 1.0f + expand;
    const float z1 = z + 1.0f + expand;

    return {{
        // Bottom face.
        {{x0, y0, z0}, {x1, y0, z0}},
        {{x1, y0, z0}, {x1, y0, z1}},
        {{x1, y0, z1}, {x0, y0, z1}},
        {{x0, y0, z1}, {x0, y0, z0}},
        // Top face.
        {{x0, y1, z0}, {x1, y1, z0}},
        {{x1, y1, z0}, {x1, y1, z1}},
        {{x1, y1, z1}, {x0, y1, z1}},
        {{x0, y1, z1}, {x0, y1, z0}},
        // Vertical edges.
        {{x0, y0, z0}, {x0, y1, z0}},
        {{x1, y0, z0}, {x1, y1, z0}},
        {{x1, y0, z1}, {x1, y1, z1}},
        {{x0, y0, z1}, {x0, y1, z1}},
    }};
}

// Face order returned by makeFaces: -Y, +Y, -Z, +Z, -X, +X. Kept next to the
// function so visibility helpers can index it safely.
constexpr std::size_t kFaceCount = 6;

// Decides which of the six faces face the eye. Bedrock's fill materials are
// not guaranteed to depth-test custom geometry, so emitting every face makes
// the far side of the block bleed through and the box reads as a colored 3D
// volume instead of a surface tint. For a convex box the eye-facing faces are
// exactly the ones a depth-tested material would keep anyway; when the eye is
// inside the box everything is kept.
constexpr std::array<bool, kFaceCount> makeFaceVisibility(
    const std::array<Quad, kFaceCount>& faces, Point eye) {
    float x0 = faces[0].a.x, x1 = x0;
    float y0 = faces[0].a.y, y1 = y0;
    float z0 = faces[0].a.z, z1 = z0;
    for (const auto& face : faces) {
        const Point verts[4] = {face.a, face.b, face.c, face.d};
        for (const Point& v : verts) {
            if (v.x < x0) x0 = v.x;
            if (v.x > x1) x1 = v.x;
            if (v.y < y0) y0 = v.y;
            if (v.y > y1) y1 = v.y;
            if (v.z < z0) z0 = v.z;
            if (v.z > z1) z1 = v.z;
        }
    }

    const bool inside = eye.x > x0 && eye.x < x1 &&
                        eye.y > y0 && eye.y < y1 &&
                        eye.z > z0 && eye.z < z1;
    return {{
        inside || eye.y < y0,  // -Y
        inside || eye.y > y1,  // +Y
        inside || eye.z < z0,  // -Z
        inside || eye.z > z1,  // +Z
        inside || eye.x < x0,  // -X
        inside || eye.x > x1,  // +X
    }};
}

// Decides which of the twelve edges returned by makeBox are visible. An edge
// of a convex box is visible when at least one face touching it faces the
// eye; far-side edges would otherwise show through the block whenever the
// fill material skips the depth test, which makes a thick outline read as a
// full 3D wireframe cube instead of a flat frame.
constexpr std::array<bool, 12> makeEdgeVisibility(
    const std::array<Line, 12>& box, Point eye) {
    float x0 = box[0].from.x, x1 = x0;
    float y0 = box[0].from.y, y1 = y0;
    float z0 = box[0].from.z, z1 = z0;
    for (const auto& line : box) {
        const Point verts[2] = {line.from, line.to};
        for (const Point& v : verts) {
            if (v.x < x0) x0 = v.x;
            if (v.x > x1) x1 = v.x;
            if (v.y < y0) y0 = v.y;
            if (v.y > y1) y1 = v.y;
            if (v.z < z0) z0 = v.z;
            if (v.z > z1) z1 = v.z;
        }
    }

    const bool inside = eye.x > x0 && eye.x < x1 &&
                        eye.y > y0 && eye.y < y1 &&
                        eye.z > z0 && eye.z < z1;
    const bool faceVisible[kFaceCount] = {
        inside || eye.y < y0,  // -Y
        inside || eye.y > y1,  // +Y
        inside || eye.z < z0,  // -Z
        inside || eye.z > z1,  // +Z
        inside || eye.x < x0,  // -X
        inside || eye.x > x1,  // +X
    };

    // Endpoints of an edge share the exact bound coordinate on every face the
    // edge touches (both derive from the same makeBox constants), so exact
    // equality is safe here.
    std::array<bool, 12> visible{};
    for (std::size_t i = 0; i < box.size(); ++i) {
        const Point& a = box[i].from;
        const Point& b = box[i].to;
        visible[i] = (a.y == y0 && b.y == y0 && faceVisible[0]) ||
                     (a.y == y1 && b.y == y1 && faceVisible[1]) ||
                     (a.z == z0 && b.z == z0 && faceVisible[2]) ||
                     (a.z == z1 && b.z == z1 && faceVisible[3]) ||
                     (a.x == x0 && b.x == x0 && faceVisible[4]) ||
                     (a.x == x1 && b.x == x1 && faceVisible[5]);
    }
    return visible;
}

// Thick outline geometry: flat strips that lie ON the visible faces of the
// block instead of camera-facing billboards floating in space.
//
// The old approach turned every edge into a camera-facing quad that also
// overshot both ends by half the width. Those bars have real depth (they
// stick out of the block towards the eye and away from it) and, where the
// three edges of the near corner meet, they form the "corner of a box" look,
// so raising Line Size made the frame read as a 3D cube even with Block 3d
// off. Painting the frame onto the surface of each visible face keeps the
// exact same silhouette as the classic hairline wireframe, just wider, and
// it can never look like a volume because nothing leaves the face plane.
//
// Each visible face gets four mitred strips along its edges, inset towards
// the middle of the face (never outside the block), lifted `lift` off the
// surface so they do not z-fight with the block itself. An edge shared by two
// visible faces gets a strip on each face, which is what the vanilla hairline
// shows as well.
inline constexpr std::size_t kMaxFrameQuads = kFaceCount * 4;

struct FrameQuads {
    std::array<Quad, kMaxFrameQuads> quads{};
    std::size_t count = 0;
};

// Widths above this would make opposite strips of a face overlap; the frame
// is clamped so the middle of the face always stays open.
inline constexpr float kMaxFrameWidth = 0.45f;

constexpr FrameQuads makeThickFrame(float x, float y, float z,
                                    const std::array<bool, kFaceCount>& faceVisible,
                                    float width, float lift = 0.004f) {
    FrameQuads out{};
    if (width <= 0.0f) return out;
    if (width > kMaxFrameWidth) width = kMaxFrameWidth;
    if (lift < 0.0f) lift = 0.0f;

    const float lo[3] = {x, y, z};
    const float hi[3] = {x + 1.0f, y + 1.0f, z + 1.0f};

    // Face order (matches makeFaces / makeFaceVisibility): -Y, +Y, -Z, +Z,
    // -X, +X. `axis` is the face normal axis, `positive` its sign.
    struct FaceDef { int axis; bool positive; };
    constexpr FaceDef kFaces[kFaceCount] = {
        {1, false}, {1, true}, {2, false}, {2, true}, {0, false}, {0, true},
    };

    auto makePoint = [&](int axis, float plane, int uAxis, float u, int vAxis, float v) {
        float p[3] = {0.0f, 0.0f, 0.0f};
        p[axis] = plane;
        p[uAxis] = u;
        p[vAxis] = v;
        return Point{p[0], p[1], p[2]};
    };

    for (std::size_t f = 0; f < kFaceCount; ++f) {
        if (!faceVisible[f]) continue;
        const int n = kFaces[f].axis;
        const int uAxis = (n + 1) % 3;
        const int vAxis = (n + 2) % 3;
        const float plane = kFaces[f].positive ? hi[n] + lift : lo[n] - lift;
        const float u0 = lo[uAxis], u1 = hi[uAxis];
        const float v0 = lo[vAxis], v1 = hi[vAxis];
        const float w = width;

        // Rectangles in (u, v) on this face: two full-length strips along the
        // u bounds and two shortened strips along the v bounds so the corners
        // are covered exactly once (mitred, no overlap, no gap).
        struct Rect { float ua, ub, va, vb; };
        const Rect rects[4] = {
            {u0, u0 + w, v0, v1},
            {u1 - w, u1, v0, v1},
            {u0 + w, u1 - w, v0, v0 + w},
            {u0 + w, u1 - w, v1 - w, v1},
        };
        for (const Rect& r : rects) {
            out.quads[out.count++] = Quad{
                makePoint(n, plane, uAxis, r.ua, vAxis, r.va),
                makePoint(n, plane, uAxis, r.ub, vAxis, r.va),
                makePoint(n, plane, uAxis, r.ub, vAxis, r.vb),
                makePoint(n, plane, uAxis, r.ua, vAxis, r.vb),
            };
        }
    }
    return out;
}

// Maps the "Line Size" menu slider (1 = hairline) to the world-space width of
// the surface strips. Kept in the header so the tests can pin the mapping;
// 1.0 or less means "hairline only, no strips".
constexpr float frameWidthForLineSize(float lineSize) {
    if (lineSize <= 1.05f) return 0.0f;
    if (lineSize > 10.0f) lineSize = 10.0f;
    // 2 -> 0.025 blocks, 10 -> 0.125 blocks (two texture pixels).
    return lineSize * 0.0125f;
}

// Builds the six faces of a block-sized axis-aligned box as quads. Used by the
// "3D" rendering mode to fill the box with a translucent overlay so the block
// reads as a solid volume instead of a bare wireframe.
constexpr std::array<Quad, 6> makeFaces(float x, float y, float z, float expand = 0.0f) {
    const float x0 = x - expand;
    const float y0 = y - expand;
    const float z0 = z - expand;
    const float x1 = x + 1.0f + expand;
    const float y1 = y + 1.0f + expand;
    const float z1 = z + 1.0f + expand;

    return {{
        // -Y (bottom).
        {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}},
        // +Y (top).
        {{x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}},
        // -Z.
        {{x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}},
        // +Z.
        {{x0, y0, z1}, {x0, y1, z1}, {x1, y1, z1}, {x1, y0, z1}},
        // -X.
        {{x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}},
        // +X.
        {{x1, y0, z0}, {x1, y0, z1}, {x1, y1, z1}, {x1, y1, z0}},
    }};
}

} // namespace bedrocktools::modules::blockoutline

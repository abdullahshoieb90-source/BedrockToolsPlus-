#pragma once

// Pure geometry + shading helpers for the Wings module.
//
// The world-space overlay draws every wing bone as a small prism with the
// Bedrock tessellator. Everything that decides *where* those prisms sit and
// *how bright* each of their faces is lives here, so it compiles and can be
// unit-tested on the host without Minecraft (tests/wings_shape_test.cpp) and
// so tools/wings_preview.cpp can render the exact same shapes offline.
//
//   Pose2D        2D transform of a bone inside the wing plane
//   WingBone      one bone: parent, anchor, box, rest fan, sweep, taper
//   buildWingBox  the 8 prism corners in Bedrock model pixels
//   kFaceRings    ring-ordered corner indices of the six faces
//   faceBrightness  flat per-face shading used by the overlay
//
// Coordinate conventions (identical to Bedrock model space, 16 px = 1 block):
//   +x  the player's left  (the right wing spans towards -x)
//   +y  up
//   +z  backwards - the back/cape side; the torso spans z in [-2, +2]
//
// A bone only ever rotates inside the wing plane (around the model z axis),
// so the pose is a plain 2D rotation + translation and the box keeps a
// constant z range.

#include <cmath>
#include <cstddef>

namespace bedrocktools::modules::wings {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kPxToBlocks = 1.0f / 16.0f;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, float k) { return {a.x * k, a.y * k, a.z * k}; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(const Vec3& a) { return std::sqrt(dot(a, a)); }

// Returns the zero vector for a degenerate input so callers never divide by
// zero while rendering.
inline Vec3 normalized(const Vec3& a) {
    const float len = length(a);
    if (!(len > 1e-6f)) return {0.0f, 0.0f, 0.0f};
    return {a.x / len, a.y / len, a.z / len};
}

// ---------------------------------------------------------------------------
// 2D pose inside the wing plane
// ---------------------------------------------------------------------------

struct Pose2D {
    float c = 1.0f;   // cos of the accumulated rotation
    float s = 0.0f;   // sin of the accumulated rotation
    float tx = 0.0f;  // pivot position in model px
    float ty = 0.0f;
};

inline Pose2D makePose(float angleRad, float pivotX, float pivotY) {
    return {std::cos(angleRad), std::sin(angleRad), pivotX, pivotY};
}

// Child pose: rotate around the parent frame, then offset by the anchor.
inline Pose2D composePose(const Pose2D& parent, float anchorX, float anchorY, float angleRad) {
    const float rc = std::cos(angleRad);
    const float rs = std::sin(angleRad);
    Pose2D out;
    out.c = parent.c * rc - parent.s * rs;
    out.s = parent.s * rc + parent.c * rs;
    out.tx = parent.c * anchorX - parent.s * anchorY + parent.tx;
    out.ty = parent.s * anchorX + parent.c * anchorY + parent.ty;
    return out;
}

inline void transformPoint(const Pose2D& pose, float x, float y, float& outX, float& outY) {
    outX = pose.c * x - pose.s * y + pose.tx;
    outY = pose.s * x + pose.c * y + pose.ty;
}

// ---------------------------------------------------------------------------
// Bone description
// ---------------------------------------------------------------------------

struct WingBone {
    int parent = -1;                 // bone table index; -1 for the shoulder root
    float anchorX = 0.0f;            // pivot offset from the parent pivot (px)
    float anchorY = 0.0f;
    float boxOX = 0.0f;              // box origin relative to the own pivot (px)
    float boxOY = 0.0f;
    float boxSX = 0.0f;              // box size on the wing plane (px)
    float boxSY = 0.0f;
    float zMin = 0.0f;               // absolute model z of the box (px)
    float zMax = 0.0f;

    // --- Shape detail added on top of the plain box layout ---
    // restDeg: rest-pose angle (deg, "raise positive") added to the animated
    //   angle. Used to fan the feathers open and curl the wing tip so the
    //   silhouette reads as a wing instead of a straight plank.
    float restDeg = 0.0f;
    // sweepPx: extra backwards offset of this bone's box, so the wing curves
    //   away from the back towards its tips instead of lying in one plane.
    float sweepPx = 0.0f;
    // taperPx: the edge far from the bone pivot is narrowed by this many px,
    //   turning the box into a tapered feather. Clamped so the far edge can
    //   never collapse or flip (see buildWingBox).
    float taperPx = 0.0f;
    // spanT: 0 at the shoulder .. 1 at the wing tip; drives the tip gradient.
    float spanT = 0.0f;
    // tint: per-bone brightness multiplier, used to alternate neighbouring
    //   feathers so they stay readable as separate feathers.
    float tint = 1.0f;

    const unsigned char* colOuter = nullptr;   // zMax face (faces away from the body)
    const unsigned char* colInner = nullptr;   // zMin face (faces the body)
    const unsigned char* colEdge = nullptr;    // x faces and the top face
    const unsigned char* colBottom = nullptr;  // yMin face (feather tips highlight)
    int angleIndex = 0;                        // 0=shoulder, 1=upper, 2=tip, 3..6=feathers
};

// ---------------------------------------------------------------------------
// Prism mesh (model px)
// ---------------------------------------------------------------------------

constexpr int kCornerCount = 8;
constexpr int kFaceCount = 6;

// Corner index bits: bit0 = box x side, bit1 = box y side, bit2 = z side.
//   0 (x0,y0,zMin)  1 (x1,y0,zMin)  2 (x0,y1,zMin)  3 (x1,y1,zMin)
//   4 (x0,y0,zMax)  5 (x1,y0,zMax)  6 (x0,y1,zMax)  7 (x1,y1,zMax)
struct WingBox {
    float px[kCornerCount];
    float py[kCornerCount];
    float pz[kCornerCount];
};

enum FaceSlot {
    kFaceInner = 0,    // zMin - faces the body
    kFaceOuter = 1,    // zMax - faces away from the body
    kFaceSpanMin = 2,  // box x-min side
    kFaceSpanMax = 3,  // box x-max side
    kFaceBottom = 4,   // box y-min (feather tips)
    kFaceTop = 5,      // box y-max
};

// Ring-ordered corner indices. Bedrock's quad primitive fans each quad from
// its first vertex, so the four corners MUST walk around the face: a
// Z-ordered list draws only half of the face and a mixed list draws a
// diagonal slice through the box. Both happened with the old flat wing
// faces, which is why the wings used to render as broken half-boxes.
constexpr int kFaceRings[kFaceCount][4] = {
    {0, 1, 3, 2},  // zMin
    {4, 5, 7, 6},  // zMax
    {0, 2, 6, 4},  // x min
    {1, 5, 7, 3},  // x max
    {0, 1, 5, 4},  // y min (bottom)
    {2, 3, 7, 6},  // y max (top)
};

// Builds the eight prism corners of a bone in model px.
inline WingBox buildWingBox(const WingBone& bone, const Pose2D& pose) {
    const float lx[2] = {bone.boxOX, bone.boxOX + bone.boxSX};
    const float ly[2] = {bone.boxOY, bone.boxOY + bone.boxSY};

    // Taper the edge far from the bone pivot (the feather/segment tip) so the
    // wing reads as a set of tapered feathers instead of a comb of bricks.
    const float sizeX = std::fabs(bone.boxSX);
    const float sizeY = std::fabs(bone.boxSY);
    const bool longAxisX = sizeX >= sizeY;
    const float shortSide = longAxisX ? sizeY : sizeX;
    float inset = bone.taperPx * 0.5f;
    if (!(inset > 0.0f)) inset = 0.0f;
    const float maxInset = shortSide * 0.45f;  // the far edge always keeps >= 10%
    if (inset > maxInset) inset = maxInset;
    const float keep = (shortSide > 1e-4f) ? (shortSide - 2.0f * inset) / shortSide : 1.0f;

    // "Far" = the end of the long axis that is further from the bone pivot.
    const int farX = (std::fabs(lx[0]) >= std::fabs(lx[1])) ? 0 : 1;
    const int farY = (std::fabs(ly[0]) >= std::fabs(ly[1])) ? 0 : 1;
    const float midX = (lx[0] + lx[1]) * 0.5f;
    const float midY = (ly[0] + ly[1]) * 0.5f;

    float quadX[4], quadY[4];
    for (int i = 0; i < 4; ++i) {
        float x = lx[i & 1];
        float y = ly[(i >> 1) & 1];
        if (longAxisX) {
            if ((i & 1) == farX) y = midY + (y - midY) * keep;
        } else if (((i >> 1) & 1) == farY) {
            x = midX + (x - midX) * keep;
        }
        transformPoint(pose, x, y, quadX[i], quadY[i]);
    }

    WingBox out{};
    const float zLo = bone.zMin + bone.sweepPx;
    const float zHi = bone.zMax + bone.sweepPx;
    for (int i = 0; i < kCornerCount; ++i) {
        out.px[i] = quadX[i & 3];
        out.py[i] = quadY[i & 3];
        out.pz[i] = ((i >> 2) & 1) ? zHi : zLo;
    }
    return out;
}

// Outward normal of a face in model space. The four side faces rotate with
// the bone; the two flat faces always point along z.
inline Vec3 faceNormalModel(int face, const Pose2D& pose) {
    switch (face) {
        case kFaceInner: return {0.0f, 0.0f, -1.0f};
        case kFaceOuter: return {0.0f, 0.0f, 1.0f};
        case kFaceSpanMin: return {-pose.c, -pose.s, 0.0f};
        case kFaceSpanMax: return {pose.c, pose.s, 0.0f};
        case kFaceBottom: return {pose.s, -pose.c, 0.0f};
        default: return {-pose.s, pose.c, 0.0f};  // kFaceTop
    }
}

// Maps a model-space direction into the world using the player's yaw basis
// (model +x is the player's left, model +z is their back).
inline Vec3 modelDirToWorld(const Vec3& d, const Vec3& right, const Vec3& forward) {
    return {
        right.x * -d.x - forward.x * d.z,
        d.y,
        right.z * -d.x - forward.z * d.z,
    };
}

// Maps a model-space point into camera-relative world space, matching the
// mapping the overlay uses for its vertices.
inline Vec3 modelPointToWorld(float px, float py, float pz, const Vec3& origin,
                              const Vec3& right, const Vec3& forward) {
    const float along = px * kPxToBlocks;
    const float back = pz * kPxToBlocks;
    return {
        origin.x + right.x * -along - forward.x * back,
        origin.y + py * kPxToBlocks,
        origin.z + right.z * -along - forward.z * back,
    };
}

// ---------------------------------------------------------------------------
// Shading
// ---------------------------------------------------------------------------

// Cosmetic-overlay lighting: a soft headlight plus a sky lift. It is view
// dependent on purpose - the wings are drawn after the world as an overlay,
// so they have to stay readable from every angle and never collapse into a
// black silhouette when the sun is behind the player. Range is
// [ambient, ambient + head + sky].
struct WingLight {
    float ambient = 0.56f;
    float head = 0.30f;
    float sky = 0.22f;
};

constexpr WingLight kDefaultLight{0.56f, 0.30f, 0.22f};

// How much brighter the wing tips are than the shoulder (spanT 0 -> 1).
constexpr float kSpanGradient = 0.18f;

inline float faceBrightness(const Vec3& normal, const Vec3& toCamera, const WingLight& light) {
    const Vec3 n = normalized(normal);
    const Vec3 v = normalized(toCamera);
    const float headTerm = dot(n, v);
    const float skyTerm = n.y;
    return light.ambient
         + light.head * (headTerm > 0.0f ? headTerm : 0.0f)
         + light.sky * (skyTerm > 0.0f ? skyTerm : 0.0f);
}

// Brightness multiplier on top of the face shading: a gentle gradient from
// the shoulder to the wing tips plus the per-bone tint.
inline float spanTint(float spanT, float boneTint) {
    const float t = spanT < 0.0f ? 0.0f : (spanT > 1.0f ? 1.0f : spanT);
    return boneTint * (1.0f + kSpanGradient * (t - 0.5f));
}

inline unsigned char shadeChannel(float channel, float factor) {
    const float v = channel * factor;
    if (!(v > 0.0f)) return 0;
    if (v >= 255.0f) return 255;
    return static_cast<unsigned char>(v + 0.5f);
}

struct FaceColor {
    unsigned char r = 0;
    unsigned char g = 0;
    unsigned char b = 0;
};

inline FaceColor shadeFace(const unsigned char* base, float brightness) {
    if (base == nullptr) return {0, 0, 0};
    return {
        shadeChannel(static_cast<float>(base[0]), brightness),
        shadeChannel(static_cast<float>(base[1]), brightness),
        shadeChannel(static_cast<float>(base[2]), brightness),
    };
}

// ---------------------------------------------------------------------------
// Host-side validation helpers (used by tests and the preview tool)
// ---------------------------------------------------------------------------

// Area of a ring of four points (two triangles); 0 for a degenerate ring.
inline float ringArea(const float p[4][3]) {
    float area = 0.0f;
    for (int t = 0; t < 2; ++t) {
        const float* a = p[0];
        const float* b = p[t == 0 ? 1 : 2];
        const float* c = p[t == 0 ? 2 : 3];
        const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
        const float vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        const float cx = uy * vz - uz * vy;
        const float cy = uz * vx - ux * vz;
        const float cz = ux * vy - uy * vx;
        area += 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
    }
    return area;
}

// True when four points form a simple (non self-intersecting) convex quad.
// A Z-ordered "bowtie" ring or a ring that cuts through the box fails this,
// which is exactly what the old wing faces were.
inline bool isConvexRing(const float p[4][3], float eps = 1e-3f) {
    const float ux = p[1][0] - p[0][0], uy = p[1][1] - p[0][1], uz = p[1][2] - p[0][2];
    const float vx = p[2][0] - p[0][0], vy = p[2][1] - p[0][1], vz = p[2][2] - p[0][2];
    float nx = uy * vz - uz * vy;
    float ny = uz * vx - ux * vz;
    float nz = ux * vy - uy * vx;
    const float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (!(nlen > eps)) return false;  // first three points collinear
    nx /= nlen; ny /= nlen; nz /= nlen;

    // Coplanarity.
    const float wx = p[3][0] - p[0][0], wy = p[3][1] - p[0][1], wz = p[3][2] - p[0][2];
    if (std::fabs(nx * wx + ny * wy + nz * wz) > eps) return false;

    // Every consecutive edge pair must turn the same way around the normal.
    float sign = 0.0f;
    for (int i = 0; i < 4; ++i) {
        const float* a = p[i];
        const float* b = p[(i + 1) & 3];
        const float* c = p[(i + 2) & 3];
        const float e1x = b[0] - a[0], e1y = b[1] - a[1], e1z = b[2] - a[2];
        const float e2x = c[0] - b[0], e2y = c[1] - b[1], e2z = c[2] - b[2];
        const float cx = e1y * e2z - e1z * e2y;
        const float cy = e1z * e2x - e1x * e2z;
        const float cz = e1x * e2y - e1y * e2x;
        const float d = cx * nx + cy * ny + cz * nz;
        if (std::fabs(d) < eps) continue;  // collinear corner, tolerated
        const float s = d > 0.0f ? 1.0f : -1.0f;
        if (sign == 0.0f) sign = s;
        else if (s != sign) return false;
    }
    return sign != 0.0f;
}

}  // namespace bedrocktools::modules::wings

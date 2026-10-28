#pragma once

// Pure math + shading helpers for the Wings module.
//
// Everything that has no Minecraft dependency lives here so it compiles and can
// be unit-tested on the host (tests/wings_shape_test.cpp) and so
// tools/wings_preview.cpp can render the exact same geometry the game draws.
//
//   Vec3 / FaceColor   small value types
//   colour helpers     lerp, channel clamping, shading
//   WingLight          the cosmetic overlay lighting model
//   model -> world     the player-yaw basis mapping the overlay uses
//   ring helpers       quad validation used by the host tests
//
// Coordinate conventions (identical to Bedrock model space, 16 px = 1 block):
//   +x  the player's left  (the right wing spans towards -x)
//   +y  up
//   +z  backwards - the back/cape side; the torso spans z in [-2, +2]
//
// The wing surface itself is built in wings_surface.hpp on top of these
// primitives; the selectable styles live in wings_design.hpp.

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

inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// Returns the zero vector for a degenerate input so callers never divide by
// zero while rendering.
inline Vec3 normalized(const Vec3& a) {
    const float len = length(a);
    if (!(len > 1e-6f)) return {0.0f, 0.0f, 0.0f};
    return {a.x / len, a.y / len, a.z / len};
}

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpFloat(float a, float b, float t) { return a + (b - a) * t; }

inline float smoothstep(float edge0, float edge1, float x) {
    if (edge1 <= edge0) return x < edge0 ? 0.0f : 1.0f;
    const float t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------

struct FaceColor {
    unsigned char r = 0;
    unsigned char g = 0;
    unsigned char b = 0;
};

inline unsigned char shadeChannel(float channel, float factor) {
    const float v = channel * factor;
    if (!(v > 0.0f)) return 0;
    if (v >= 255.0f) return 255;
    return static_cast<unsigned char>(v + 0.5f);
}

inline float channelToFloat(unsigned char c) { return static_cast<float>(c); }

inline FaceColor shadeFace(const FaceColor& base, float brightness) {
    return {
        shadeChannel(channelToFloat(base.r), brightness),
        shadeChannel(channelToFloat(base.g), brightness),
        shadeChannel(channelToFloat(base.b), brightness),
    };
}

inline FaceColor lerpColor(const FaceColor& a, const FaceColor& b, float t) {
    const float k = clamp01(t);
    return {
        shadeChannel(lerpFloat(channelToFloat(a.r), channelToFloat(b.r), k), 1.0f),
        shadeChannel(lerpFloat(channelToFloat(a.g), channelToFloat(b.g), k), 1.0f),
        shadeChannel(lerpFloat(channelToFloat(a.b), channelToFloat(b.b), k), 1.0f),
    };
}

// ---------------------------------------------------------------------------
// Lighting
// ---------------------------------------------------------------------------

// Cosmetic-overlay lighting: a soft headlight plus a sky lift. It is view
// dependent on purpose - the wings are drawn after the world as an overlay, so
// they have to stay readable from every angle and never collapse into a black
// silhouette when the sun is behind the player. Range is
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
// the shoulder to the wing tips.
inline float spanTint(float spanT, float boneTint = 1.0f) {
    const float t = spanT < 0.0f ? 0.0f : (spanT > 1.0f ? 1.0f : spanT);
    return boneTint * (1.0f + kSpanGradient * (t - 0.5f));
}

// ---------------------------------------------------------------------------
// Model -> world
// ---------------------------------------------------------------------------

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
// A Z-ordered "bowtie" ring or a ring that cuts through the quad fails this.
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

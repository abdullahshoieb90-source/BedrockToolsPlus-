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
    float ambient = 0.52f;
    float head = 0.26f;
    float sky = 0.20f;
};

// Peak = ambient + head + sky = 0.98. It deliberately stays just under 1.0: the
// palette already encodes the target look, so lighting must shade it down, not
// push it up. With a peak above 1.0 the light palettes (Angel, Fairy) clipped
// to a flat white sheet and lost every gradient.
constexpr WingLight kDefaultLight{0.52f, 0.26f, 0.20f};

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

// Vector from a point on the surface to the camera, used by the overlay's
// headlight term.
//
// This has to be measured between two positions in the SAME space. The overlay
// builds its vertices camera-relative but computes the shading center in world
// space, so negating the center there (as if it were already camera-relative)
// pointed the headlight at the world origin: the wings then changed brightness
// whenever the player walked somewhere else, which read as a washed-out sheet
// from one angle and a dark one from another. Named helper + host test so the
// two spaces cannot be mixed up again.
inline Vec3 toCameraVector(const Vec3& camPos, const Vec3& surfaceCenter) {
    return camPos - surfaceCenter;
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
// Body occlusion
// ---------------------------------------------------------------------------
//
// The wings are mounted on the back, so when the camera is in front of the
// player the body must hide them. Whether the fill material actually depth
// tests is build dependent (ui_fill_color has no depth test on current Bedrock
// builds - see blockoutline.cpp), so the overlay cannot rely on it: it culls
// the wing quads that sit behind the player's own collision box instead. This
// is the same "cull what the camera cannot see" approach the Hitbox module uses
// for solid blocks, applied to the player's body.

// Slab test for the segment origin -> target against an axis-aligned box.
// Returns the normalized entry/exit parameters along the segment.
inline bool segmentHitsBox(const Vec3& origin, const Vec3& target, const Vec3& boxMin,
                           const Vec3& boxMax, float& tEnter, float& tExit) {
    const Vec3 d = target - origin;
    const float o[3] = {origin.x, origin.y, origin.z};
    const float dir[3] = {d.x, d.y, d.z};
    const float mn[3] = {boxMin.x, boxMin.y, boxMin.z};
    const float mx[3] = {boxMax.x, boxMax.y, boxMax.z};

    float tmin = 0.0f;
    float tmax = 1.0f;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(dir[i]) < 1e-9f) {
            // Parallel to this slab: the segment is inside it or misses.
            if (o[i] < mn[i] || o[i] > mx[i]) return false;
            continue;
        }
        const float inv = 1.0f / dir[i];
        float t1 = (mn[i] - o[i]) * inv;
        float t2 = (mx[i] - o[i]) * inv;
        if (t1 > t2) {
            const float tmp = t1;
            t1 = t2;
            t2 = tmp;
        }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return false;
    }
    tEnter = tmin;
    tExit = tmax;
    return true;
}

inline bool pointInsideBox(const Vec3& p, const Vec3& boxMin, const Vec3& boxMax, float margin) {
    return p.x >= boxMin.x - margin && p.x <= boxMax.x + margin &&
           p.y >= boxMin.y - margin && p.y <= boxMax.y + margin &&
           p.z >= boxMin.z - margin && p.z <= boxMax.z + margin;
}

// Cosine between the player's BACK direction and the direction from the body
// to the camera:
//   +1 = camera directly behind the player (normal third-person view)
//    0 = camera exactly side-on
//   -1 = camera directly in front of the player
// The wings are mounted on the back, so this is the value that decides whether
// drawing them makes any sense at all.
inline float cameraBackness(const Vec3& cam, const Vec3& bodyCenter, const Vec3& forward) {
    const Vec3 toCam = cam - bodyCenter;
    const float len = length(toCam);
    if (!(len > 1e-6f)) return 0.0f;
    const Vec3 back = forward * -1.0f;
    return dot(back, toCam * (1.0f / len));
}

// ---------------------------------------------------------------------------
// Exact occlusion: the body's shadow cone
// ---------------------------------------------------------------------------
//
// Culling whole quads leaves a staircase along the body's silhouette, because a
// membrane cell is a couple of model pixels across but a few dozen screen
// pixels at third-person distance. Instead the wing polygons are clipped
// against the volume the body actually hides from the camera.
//
// The hidden volume of a convex box seen from a point is the cone of rays that
// pass through it. That cone is the intersection of every plane through the
// camera that the box lies entirely on one side of (its supporting planes), so
// clipping is a plain Sutherland-Hodgman pass - no projection, no screen-space
// hull, and it stays exact at any angle.

// A wing vertex while it is being clipped: world position plus the Gouraud
// colour that travels with it, so cutting an edge also interpolates the shading
// instead of leaving a hard colour seam at the cut.
struct ClipVertex {
    Vec3 p;
    FaceColor c;
};

constexpr int kMaxShadowPlanes = 12;
constexpr int kMaxClipVerts = 16;

struct ShadowCone {
    Vec3 plane[kMaxShadowPlanes];  // outward normals; the apex is the camera
    int count = 0;
};

// Camera -> the eight corners of the body box, kept in one place so the cone and
// the point tests agree.
inline void bodyCorners(const Vec3& boxMin, const Vec3& boxMax, Vec3 out[8]) {
    for (int i = 0; i < 8; ++i) {
        out[i] = {(i & 1) ? boxMax.x : boxMin.x, (i & 2) ? boxMax.y : boxMin.y,
                  (i & 4) ? boxMax.z : boxMin.z};
    }
}

// Builds the half-spaces of the body's shadow cone. A point is inside the cone
// (i.e. hidden behind the body) when it satisfies every plane.
inline ShadowCone buildShadowCone(const Vec3& apex, const Vec3& boxMin, const Vec3& boxMax) {
    static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                          {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    Vec3 corner[8];
    bodyCorners(boxMin, boxMax, corner);

    ShadowCone cone;
    for (const auto& e : kEdges) {
        const Vec3 a = corner[e[0]] - apex;
        const Vec3 b = corner[e[1]] - apex;
        Vec3 n = cross(a, b);
        const float len = length(n);
        if (!(len > 1e-6f)) continue;  // edge runs straight at the camera
        n = n * (1.0f / len);

        // Keep only the planes the box lies entirely behind; the rest cut
        // through it and are not part of the silhouette.
        float dmax = -3.0e38f;
        float dmin = 3.0e38f;
        for (const Vec3& c : corner) {
            const float d = dot(n, c - apex);
            if (d > dmax) dmax = d;
            if (d < dmin) dmin = d;
        }
        // Orient so the whole box sits on the negative side; the cone is then
        // simply "behind every plane".
        if (dmax <= 1e-4f) {
            // box already on the negative side
        } else if (dmin >= -1e-4f) {
            n = n * -1.0f;
        } else {
            continue;  // plane cuts through the box: not part of the silhouette
        }

        bool duplicate = false;
        for (int i = 0; i < cone.count; ++i) {
            if (dot(cone.plane[i], n) > 0.9999f) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate && cone.count < kMaxShadowPlanes) cone.plane[cone.count++] = n;
    }
    return cone;
}

// A convex polygon on its way through the clipper.
struct ClipPolygon {
    ClipVertex v[kMaxClipVerts];
    int count = 0;
};

// Sutherland-Hodgman against one plane through `apex`. `keepOutside` keeps the
// half-space the plane normal points into.
inline int clipToHalfSpace(const ClipVertex* in, int count, const Vec3& n, const Vec3& apex,
                           bool keepOutside, ClipVertex* out) {
    if (count < 3) return 0;
    if (count > kMaxClipVerts) count = kMaxClipVerts;

    int m = 0;
    for (int i = 0; i < count; ++i) {
        const ClipVertex& cur = in[i];
        const ClipVertex& nxt = in[(i + 1) % count];
        const float dc = dot(n, cur.p - apex);
        const float dn = dot(n, nxt.p - apex);
        const bool curIn = keepOutside ? (dc > 0.0f) : (dc <= 0.0f);
        const bool nxtIn = keepOutside ? (dn > 0.0f) : (dn <= 0.0f);

        if (curIn) out[m++] = cur;
        if (curIn != nxtIn) {
            const float denom = dc - dn;
            const float t = (denom != 0.0f) ? dc / denom : 0.0f;
            out[m].p = cur.p + (nxt.p - cur.p) * t;
            out[m].c = lerpColor(cur.c, nxt.c, t);
            ++m;
        }
    }
    return m;
}

// Splits a polygon into the part the body hides and the part it does not,
// keeping every visible piece.
//
// The visible region is the *complement* of a convex cone, which is not convex,
// so a single clip pass cannot express it. It is written instead as a disjoint
// union of convex pieces:
//
//   visible = (P minus H1) + (P n H1 minus H2) + (P n H1 n H2 minus H3) + ...
//
// where the Hk are the cone's half-spaces. The pieces tile the visible part
// exactly - no gaps and no double-drawn overlap.
inline int clipPolygonOutsideCone(const ClipVertex* in, int count, const ShadowCone& cone,
                                  const Vec3& apex, ClipPolygon* pieces, int maxPieces) {
    if (count < 3 || maxPieces <= 0) return 0;

    ClipPolygon cur;
    cur.count = (count > kMaxClipVerts) ? kMaxClipVerts : count;
    for (int i = 0; i < cur.count; ++i) cur.v[i] = in[i];

    int written = 0;
    for (int k = 0; k < cone.count; ++k) {
        // The piece poking out of this plane, on the visible side.
        ClipPolygon visible;
        visible.count = clipToHalfSpace(cur.v, cur.count, cone.plane[k], apex, true, visible.v);
        if (visible.count >= 3) {
            if (written < maxPieces) pieces[written++] = visible;
        }

        // Everything still inside every plane so far is hidden; stop once it is
        // empty, because the remaining pieces would be empty too.
        ClipPolygon remaining;
        remaining.count = clipToHalfSpace(cur.v, cur.count, cone.plane[k], apex, false, remaining.v);
        cur = remaining;
        if (cur.count < 3) break;
    }
    return written;
}

// Box the body actually occupies, built from the collision AABB.
//
// The collision box is square in plan (0.6 x 0.6) but the rendered player is
// thin: the model torso is only 4 px deep, i.e. z in [-0.125, +0.125] blocks.
// Using the collision box as the occluder would swallow the wing mounts (they
// sit 0.18 blocks behind the center), so the roots would be treated as "inside
// the body" and painted over the torso - exactly the artefact this is meant to
// remove. Shrink the z axis to the model depth and keep the collision box for
// the x/y extents, which is what the arms and torso actually span.
inline void bodyOccluderFromAabb(const Vec3& aabbMin, const Vec3& aabbMax, Vec3& outMin,
                                 Vec3& outMax) {
    constexpr float kModelHalfDepth = 0.125f;  // 2 model px, the torso's half depth
    const float centerZ = (aabbMin.z + aabbMax.z) * 0.5f;
    outMin = {aabbMin.x, aabbMin.y, centerZ - kModelHalfDepth};
    outMax = {aabbMax.x, aabbMax.y, centerZ + kModelHalfDepth};
}

// True when the box (the player's body) sits between the camera and `target`,
// i.e. that point of the wing is on the far side of the body and would be
// hidden by it.
//
// Points inside the box are the wing roots mounted on the body, so they are
// never culled - otherwise the wings would detach from the back.
inline bool hiddenBehindBody(const Vec3& cam, const Vec3& target, const Vec3& boxMin,
                            const Vec3& boxMax, float insideMargin = 0.02f) {
    if (pointInsideBox(target, boxMin, boxMax, insideMargin)) return false;

    float tEnter = 0.0f;
    float tExit = 0.0f;
    if (!segmentHitsBox(cam, target, boxMin, boxMax, tEnter, tExit)) return false;
    // The body has to be crossed strictly before the wing point is reached.
    return tEnter > 1e-4f && tEnter < 1.0f - 1e-4f;
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

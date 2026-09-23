#pragma once

// Screen-space projection for the Hitbox overlay's HUD fallback.
//
// The module's boxes are world-space geometry drawn inside the game's own
// render pass, which is where they belong. On a build where that pass never
// runs (an unresolved LevelRenderer signature, a player-renderer layout that
// does not match the header) the overlay is invisible and there is nothing the
// module can do about it from inside that pass. The launcher HUD layer is
// independent of it, so the same boxes can be projected and submitted there
// instead - see the fallback in hitbox.cpp.
//
// Pure functions with no game or preloader dependency, so host tests can cover
// the projection without a signature table.
//
// Conventions, matching the game:
//   * +X east, +Y up, +Z south; the world is right-handed.
//   * rot.x is pitch (negative = looking up), rot.y is yaw, both degrees.
//     Yaw 0 looks towards +Z and grows towards -X, so the forward vector is
//     (-sin(yaw)cos(pitch), -sin(pitch), cos(yaw)cos(pitch)).
//   * Because the world is right-handed the camera's right vector is
//     cross(forward, up) - building it the other way round mirrors the whole
//     overlay left to right.

#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace hitboxhud {

inline constexpr float kPi = 3.14159265f;
inline constexpr float kDegToRad = kPi / 180.0f;

// Depth in front of the camera below which a point cannot be projected: it
// would divide by a zero or negative depth and land on the wrong side of the
// screen. Segments are clipped against this plane instead.
inline constexpr float kNearPlane = 0.05f;

struct Camera {
    bedrocktools::sdk::Vec3 pos{};
    bedrocktools::sdk::Vec3 right{};
    bedrocktools::sdk::Vec3 up{};
    bedrocktools::sdk::Vec3 forward{};
};

inline bedrocktools::sdk::Vec3 normalize(const bedrocktools::sdk::Vec3& v) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-6f) return {0.0f, 1.0f, 0.0f};
    return {v.x / len, v.y / len, v.z / len};
}

inline bedrocktools::sdk::Vec3 cross(const bedrocktools::sdk::Vec3& a,
                                     const bedrocktools::sdk::Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float dot(const bedrocktools::sdk::Vec3& a, const bedrocktools::sdk::Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Orthonormal camera basis from a position and a yaw/pitch pair.
inline Camera computeCamera(const bedrocktools::sdk::Vec3& pos,
                            const bedrocktools::sdk::Vec2& rotation) {
    const float yawR = rotation.y * kDegToRad;
    const float pitchR = rotation.x * kDegToRad;
    const float cy = std::cos(yawR);
    const float sy = std::sin(yawR);
    const float cp = std::cos(pitchR);
    const float sp = std::sin(pitchR);

    Camera cam;
    cam.pos = pos;
    cam.forward = normalize({-sy * cp, -sp, cy * cp});
    // Derived from the yaw alone: the game has no camera roll, and this stays
    // well defined at pitch +/-90 where cross(forward, up) collapses.
    cam.right = normalize({-cy, 0.0f, -sy});
    cam.up = normalize(cross(cam.right, cam.forward));
    return cam;
}

struct Projection {
    float width = 1.0f;
    float height = 1.0f;
    float tanHalfFov = 1.0f;
    float aspect = 1.0f;
};

inline Projection makeProjection(float width, float height, float fovDegrees) {
    Projection proj;
    proj.width = width > 1.0f ? width : 1.0f;
    proj.height = height > 1.0f ? height : 1.0f;
    const float fov = std::clamp(fovDegrees, 30.0f, 120.0f);
    proj.tanHalfFov = std::tan(fov * 0.5f * kDegToRad);
    proj.aspect = proj.width / proj.height;
    return proj;
}

// Projects one world point to HUD surface coordinates (pixels). False when the
// point is at or behind the camera plane.
inline bool project(const Camera& cam, const Projection& proj,
                    const bedrocktools::sdk::Vec3& world, float& outX, float& outY) {
    const bedrocktools::sdk::Vec3 d{world.x - cam.pos.x, world.y - cam.pos.y,
                                    world.z - cam.pos.z};
    const float vz = dot(d, cam.forward);
    if (!(vz > kNearPlane)) return false;

    const float vx = dot(d, cam.right);
    const float vy = dot(d, cam.up);

    const float ndcX = (vx / vz) / (proj.tanHalfFov * proj.aspect);
    const float ndcY = (vy / vz) / proj.tanHalfFov;

    outX = (ndcX * 0.5f + 0.5f) * proj.width;
    outY = (0.5f - ndcY * 0.5f) * proj.height;
    return true;
}

struct Segment {
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
};

// Projects one world-space segment, clipping it against the near plane so an
// edge that runs through the camera still produces the visible part instead of
// a segment that wraps around the screen.
inline bool projectSegment(const Camera& cam, const Projection& proj,
                           const bedrocktools::sdk::Vec3& a,
                           const bedrocktools::sdk::Vec3& b,
                           float limit, Segment& out) {
    const bedrocktools::sdk::Vec3 da{a.x - cam.pos.x, a.y - cam.pos.y, a.z - cam.pos.z};
    const bedrocktools::sdk::Vec3 db{b.x - cam.pos.x, b.y - cam.pos.y, b.z - cam.pos.z};

    float ax = dot(da, cam.right), ay = dot(da, cam.up), az = dot(da, cam.forward);
    float bx = dot(db, cam.right), by = dot(db, cam.up), bz = dot(db, cam.forward);

    const float za = az - kNearPlane;
    const float zb = bz - kNearPlane;
    if (za < 0.0f && zb < 0.0f) return false;

    if (za < 0.0f) {
        const float t = za / (za - zb);
        ax += (bx - ax) * t;
        ay += (by - ay) * t;
        az = kNearPlane;
    } else if (zb < 0.0f) {
        const float t = zb / (zb - za);
        bx += (ax - bx) * t;
        by += (ay - by) * t;
        bz = kNearPlane;
    }

    auto toSurface = [&](float vx, float vy, float vz, float& sx, float& sy) {
        const float ndcX = (vx / vz) / (proj.tanHalfFov * proj.aspect);
        const float ndcY = (vy / vz) / proj.tanHalfFov;
        sx = (ndcX * 0.5f + 0.5f) * proj.width;
        sy = (0.5f - ndcY * 0.5f) * proj.height;
        if (limit > 0.0f) {
            sx = std::clamp(sx, -limit, proj.width + limit);
            sy = std::clamp(sy, -limit, proj.height + limit);
        }
    };

    toSurface(ax, ay, az, out.x1, out.y1);
    toSurface(bx, by, bz, out.x2, out.y2);
    return std::isfinite(out.x1) && std::isfinite(out.y1) &&
           std::isfinite(out.x2) && std::isfinite(out.y2);
}

// A box has twelve edges; used to reserve room for one frame's commands.
inline constexpr size_t kBoxEdgeCount = 12;

// Corner pairs of the twelve edges of a box (each pair differs in one bit).
inline constexpr int kBoxEdges[12][2] = {
    {0, 1}, {0, 2}, {0, 4}, {1, 3}, {1, 5}, {2, 3},
    {2, 6}, {3, 7}, {4, 5}, {4, 6}, {5, 7}, {6, 7},
};

inline bedrocktools::sdk::Vec3 cornerOf(const bedrocktools::sdk::Vec3& boxMin,
                                        const bedrocktools::sdk::Vec3& boxMax, int index) {
    return {(index & 1) ? boxMax.x : boxMin.x,
            (index & 2) ? boxMax.y : boxMin.y,
            (index & 4) ? boxMax.z : boxMin.z};
}

// The twelve projected edges of an actor box. Edges fully behind the camera
// are dropped, partially visible ones are clipped. `limit` (pixels) keeps an
// entity straddling the camera plane from producing coordinates in the tens of
// thousands.
inline void projectBox(const Camera& cam, const Projection& proj,
                       const bedrocktools::sdk::Vec3& boxMin,
                       const bedrocktools::sdk::Vec3& boxMax,
                       std::vector<Segment>& out, float limit = 0.0f) {
    for (const auto& edge : kBoxEdges) {
        Segment segment;
        if (!projectSegment(cam, proj, cornerOf(boxMin, boxMax, edge[0]),
                            cornerOf(boxMin, boxMax, edge[1]), limit, segment)) {
            continue;
        }
        out.push_back(segment);
    }
}

inline void projectBox(const Camera& cam, const Projection& proj,
                       const bedrocktools::sdk::AABB& box,
                       std::vector<Segment>& out, float limit = 0.0f) {
    projectBox(cam, proj, box.min, box.max, out, limit);
}

} // namespace hitboxhud

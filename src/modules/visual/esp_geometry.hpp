#pragma once

// Camera basis + perspective projection for the Esp overlay.
//
// Pure functions with no game or preloader dependencies so host tests can
// cover the projection math (see tests/esp_geometry_test.cpp) without
// bringing in the signature table or the HUD overlay.
//
// Conventions, matching the game:
//   * Bedrock's world is right-handed: +X east, +Y up, +Z south.
//   * rot.x is pitch (negative = looking up), rot.y is yaw, both degrees.
//     Yaw 0 looks towards +Z (south) and grows towards -X (west).
//   * The camera forward vector is therefore the same look line the Hitbox
//     module derives for its raycast:
//     (-sin(yaw)cos(pitch), -sin(pitch), cos(yaw)cos(pitch)).
//
// Because the world is right-handed, the camera's right-hand direction is
// cross(forward, worldUp) -- NOT cross(worldUp, forward), which is the left
// vector. Building the basis the other way round mirrors the whole overlay
// left-to-right: a target dead ahead still lands in the middle of the screen,
// but every target off-axis lands on the wrong side and the box runs away
// from its entity as soon as the view turns.
//
// The vertical FOV is passed in explicitly so the caller can source it from
// wherever it likes (the Esp module uses a menu slider).

#include <bedrocktools/sdk/Types.hpp>
#include <algorithm>
#include <cmath>

namespace esp {

inline constexpr float kPi = 3.14159265f;
inline constexpr float kDegToRad = kPi / 180.0f;

// Depth in front of the camera below which a point can no longer be
// projected. Points closer than this (or behind the camera) would divide by a
// zero/negative depth and land on the wrong side of the screen, so box edges
// are clipped against this plane instead (see projectBox).
inline constexpr float kNearPlane = 0.05f;

struct Camera {
    bedrocktools::sdk::Vec3 pos;
    bedrocktools::sdk::Vec3 right;
    bedrocktools::sdk::Vec3 up;
    bedrocktools::sdk::Vec3 forward;
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

// Builds the orthonormal camera basis from a position and a yaw/pitch pair
// (rot.x = pitch, rot.y = yaw, both degrees).
inline Camera computeCamera(const bedrocktools::sdk::Vec3& pos,
                            const bedrocktools::sdk::Vec2& rot) {
    const float yawR = rot.y * kDegToRad;
    const float pitchR = rot.x * kDegToRad;
    const float cy = std::cos(yawR);
    const float sy = std::sin(yawR);
    const float cp = std::cos(pitchR);
    const float sp = std::sin(pitchR);

    Camera cam;
    cam.pos = pos;
    cam.forward = normalize({-sy * cp, -sp, cy * cp});
    // Right-hand direction = cross(forward, worldUp), which normalizes to the
    // horizontal (-cos yaw, 0, -sin yaw). Deriving it from the yaw alone keeps
    // it horizontal (the game has no camera roll) and well defined at pitch
    // +/-90, where cross(forward, worldUp) collapses to the zero vector.
    cam.right = normalize({-cy, 0.0f, -sy});
    cam.up = normalize(cross(cam.right, cam.forward));
    return cam;
}

struct SurfaceProjection {
    float width = 1.0f;
    float height = 1.0f;
    float tanHalfFov = 1.0f;
    float aspect = 1.0f;
};

inline SurfaceProjection makeProjection(float width, float height, float fovDegrees) {
    SurfaceProjection proj;
    proj.width = width > 1.0f ? width : 1.0f;
    proj.height = height > 1.0f ? height : 1.0f;
    const float fov = std::clamp(fovDegrees, 30.0f, 120.0f);
    proj.tanHalfFov = std::tan(fov * 0.5f * kDegToRad);
    proj.aspect = proj.width / proj.height;
    return proj;
}

// Projects one world point to surface coordinates (pixels in the HUD surface
// space). Returns false when the point is at or behind the camera plane.
inline bool project(const Camera& cam, const SurfaceProjection& proj,
                    const bedrocktools::sdk::Vec3& world, float& outX, float& outY) {
    const bedrocktools::sdk::Vec3 d = {world.x - cam.pos.x, world.y - cam.pos.y,
                                       world.z - cam.pos.z};
    const float vz = dot(d, cam.forward);
    if (vz <= kNearPlane) return false;

    const float vx = dot(d, cam.right);
    const float vy = dot(d, cam.up);

    const float ndcX = (vx / vz) / (proj.tanHalfFov * proj.aspect);
    const float ndcY = (vy / vz) / proj.tanHalfFov;

    outX = (ndcX * 0.5f + 0.5f) * proj.width;
    outY = (0.5f - ndcY * 0.5f) * proj.height;
    return true;
}

// Tight 2D bounding box of a projected world box, in surface coordinates.
struct ScreenBox {
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    bool visible = false;
};

// Projects an axis-aligned world box (an entity AABB) to its tight 2D box.
//
// Corners behind the near plane cannot be projected, so the box's twelve
// edges are clipped against vz = kNearPlane and the intersection points are
// projected along with the surviving corners. Without that an entity standing
// right in front of the camera loses corners and its 2D box snaps to a
// fraction of the real size every time the view rotates -- the overlay then
// visibly detaches from the entity it belongs to.
//
// `limit` (in pixels) clamps the result so an entity that straddles the camera
// plane yields a huge-but-finite box covering the screen instead of
// coordinates in the tens of thousands. Pass 0 to disable clamping.
inline ScreenBox projectBox(const Camera& cam, const SurfaceProjection& proj,
                            const bedrocktools::sdk::Vec3& boxMin,
                            const bedrocktools::sdk::Vec3& boxMax,
                            float limit = 0.0f) {
    // The eight corners in camera space: (vx = right, vy = up, vz = forward).
    bedrocktools::sdk::Vec3 cs[8];
    for (int i = 0; i < 8; ++i) {
        const bedrocktools::sdk::Vec3 world{
            (i & 1) ? boxMax.x : boxMin.x,
            (i & 2) ? boxMax.y : boxMin.y,
            (i & 4) ? boxMax.z : boxMin.z,
        };
        const bedrocktools::sdk::Vec3 d{world.x - cam.pos.x, world.y - cam.pos.y,
                                        world.z - cam.pos.z};
        cs[i] = {dot(d, cam.right), dot(d, cam.up), dot(d, cam.forward)};
    }

    ScreenBox out;
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    bool any = false;

    const bool clamp = limit > 0.0f;
    auto accumulate = [&](float vx, float vy, float vz) {
        const float ndcX = (vx / vz) / (proj.tanHalfFov * proj.aspect);
        const float ndcY = (vy / vz) / proj.tanHalfFov;
        float sx = (ndcX * 0.5f + 0.5f) * proj.width;
        float sy = (0.5f - ndcY * 0.5f) * proj.height;
        if (clamp) {
            sx = std::clamp(sx, -limit, proj.width + limit);
            sy = std::clamp(sy, -limit, proj.height + limit);
        }
        minX = std::min(minX, sx);
        minY = std::min(minY, sy);
        maxX = std::max(maxX, sx);
        maxY = std::max(maxY, sy);
        any = true;
    };

    for (const bedrocktools::sdk::Vec3& p : cs) {
        if (p.z >= kNearPlane) accumulate(p.x, p.y, p.z);
    }

    // Near-plane intersections of the twelve box edges (corner indices differ
    // in exactly one bit). Only edges that cross the plane contribute.
    static const int kEdges[12][2] = {
        {0, 1}, {0, 2}, {0, 4}, {1, 3}, {1, 5}, {2, 3},
        {2, 6}, {3, 7}, {4, 5}, {4, 6}, {5, 7}, {6, 7},
    };
    for (const auto& edge : kEdges) {
        const bedrocktools::sdk::Vec3& a = cs[edge[0]];
        const bedrocktools::sdk::Vec3& b = cs[edge[1]];
        const float da = a.z - kNearPlane;
        const float db = b.z - kNearPlane;
        if ((da >= 0.0f) == (db >= 0.0f)) continue; // both in front or both behind
        const float t = da / (da - db);
        accumulate(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, kNearPlane);
    }

    if (!any) return out;

    out.minX = minX;
    out.minY = minY;
    out.maxX = maxX;
    out.maxY = maxY;
    out.visible = true;
    return out;
}

} // namespace esp

#pragma once

// Camera basis + perspective projection for the Esp overlay.
//
// Pure functions with no game or preloader dependencies so host tests can
// cover the projection math (see tests/esp_geometry_test.cpp) without
// bringing in the signature table or the HUD overlay.
//
// The camera forward vector matches the one the Hitbox module derives for its
// look line: at yaw = 0 / pitch = 0 the player faces +Z, with +X to the right
// and +Y up. The vertical FOV is passed in explicitly so the caller can source
// it from wherever it likes (the Esp module uses a menu slider).

#include <bedrocktools/sdk/Types.hpp>
#include <algorithm>
#include <cmath>

namespace esp {

inline constexpr float kPi = 3.14159265f;
inline constexpr float kDegToRad = kPi / 180.0f;

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
    cam.right = normalize(cross({0.0f, 1.0f, 0.0f}, cam.forward));
    cam.up = normalize(cross(cam.forward, cam.right));
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
    if (vz <= 0.05f) return false;

    const float vx = dot(d, cam.right);
    const float vy = dot(d, cam.up);

    const float ndcX = (vx / vz) / (proj.tanHalfFov * proj.aspect);
    const float ndcY = (vy / vz) / proj.tanHalfFov;

    outX = (ndcX * 0.5f + 0.5f) * proj.width;
    outY = (0.5f - ndcY * 0.5f) * proj.height;
    return true;
}

} // namespace esp

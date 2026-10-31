#pragma once

#include "panel_geometry.hpp"

#include <array>
#include <cmath>
#include <optional>

namespace bedrocktools::spatialgui {

// OpenGL column-major inverse of projection * view. The caller must supply the
// actual render camera matrix, framebuffer size, and top-left-origin touch
// coordinates, all from the same frame and coordinate space.
struct Camera {
    std::array<float, 16> inverseViewProjection;
    float viewportWidth;
    float viewportHeight;
};
struct Ray { Vec3 origin; Vec3 direction; };

inline std::optional<Vec3> unproject(const Camera& camera, float x, float y, float z) {
    const auto& m = camera.inverseViewProjection;
    for (float v : m) if (!std::isfinite(v)) return std::nullopt;
    const float px = m[0]*x + m[4]*y + m[8]*z + m[12];
    const float py = m[1]*x + m[5]*y + m[9]*z + m[13];
    const float pz = m[2]*x + m[6]*y + m[10]*z + m[14];
    const float w = m[3]*x + m[7]*y + m[11]*z + m[15];
    if (!std::isfinite(w) || std::fabs(w) < 1e-7f) return std::nullopt;
    Vec3 point{px/w, py/w, pz/w};
    return finite(point) ? std::optional<Vec3>{point} : std::nullopt;
}

inline std::optional<Ray> screenRay(const Camera& camera, Vec2 point) {
    if (!std::isfinite(camera.viewportWidth) || !std::isfinite(camera.viewportHeight) ||
        camera.viewportWidth <= 0 || camera.viewportHeight <= 0 ||
        !std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.y < 0 ||
        point.x > camera.viewportWidth || point.y > camera.viewportHeight) return std::nullopt;
    const float x = 2.0f * point.x / camera.viewportWidth - 1.0f;
    const float y = 1.0f - 2.0f * point.y / camera.viewportHeight;
    const auto nearPoint = unproject(camera, x, y, -1);
    const auto farPoint = unproject(camera, x, y, 1);
    if (!nearPoint || !farPoint) return std::nullopt;
    const auto direction = unit(sub(*farPoint, *nearPoint));
    if (!direction) return std::nullopt;
    return Ray{*nearPoint, *direction};
}

// Maps a touch to the panel's logical UI coordinates. Coordinates are
// continuous and may equal width/height on the right/bottom edge; callers
// needing pixel indices must apply their own explicit edge policy.
inline std::optional<Vec2> mapTouch(const Camera& camera, const Panel& panel,
                                    Vec2 point, Vec2 uiSize) {
    if (!std::isfinite(uiSize.x) || !std::isfinite(uiSize.y) || uiSize.x <= 0 || uiSize.y <= 0)
        return std::nullopt;
    const auto ray = screenRay(camera, point);
    if (!ray) return std::nullopt;
    const auto uv = hitTest(panel, ray->origin, ray->direction);
    if (!uv) return std::nullopt;
    return Vec2{uv->x * uiSize.x, uv->y * uiSize.y};
}

} // namespace bedrocktools::spatialgui

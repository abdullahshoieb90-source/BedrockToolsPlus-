#pragma once
#include "panel_geometry.hpp"
#include <array>
#include <cmath>
#include <optional>

namespace bedrocktools::spatialgui {
struct ClipVertex { float x, y, z, w; };
using ClipQuad = std::array<ClipVertex, 4>;
// OpenGL column-major projection * view, matching the camera used to render
// the current frame. Coordinates must be in the same world space as the matrix.
// Panels crossing or behind the near plane are rejected rather than clipped.
inline std::optional<ClipQuad> projectPanel(const Panel& panel, const std::array<float,16>& matrix) {
    for (float x : matrix) if (!std::isfinite(x)) return {};
    const auto c = corners(panel); // TL, TR, BR, BL
    ClipQuad out{};
    constexpr int indices[4] = {0,1,3,2}; // triangle strip
    for (int i=0; i<4; ++i) {
        const auto p=c[indices[i]];
        if (!finite(p)) return {};
        auto& v=out[i];
        v.x=matrix[0]*p.x+matrix[4]*p.y+matrix[8]*p.z+matrix[12];
        v.y=matrix[1]*p.x+matrix[5]*p.y+matrix[9]*p.z+matrix[13];
        v.z=matrix[2]*p.x+matrix[6]*p.y+matrix[10]*p.z+matrix[14];
        v.w=matrix[3]*p.x+matrix[7]*p.y+matrix[11]*p.z+matrix[15];
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
            !std::isfinite(v.w) || v.w<=1e-6f || v.z < -v.w || v.z > v.w) return {};
    }
    return out;
}
}

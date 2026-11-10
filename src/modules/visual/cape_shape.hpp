#pragma once

#include "modules/visual/wings_surface.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace bedrocktools::modules::cape {
using wings::WingQuad;

// Full artwork, or the back face of a conventional 64x32 cape atlas (also
// supports integer-scaled atlases). Nearest-neighbour sampling bounds the
// tessellator budget even for high-resolution images. Alpha is cutout, not blend.
inline std::vector<WingQuad> build(const unsigned char* rgba, int width, int height) {
    std::vector<WingQuad> out;
    if (!rgba || width <= 0 || height <= 0 || width > 2048 || height > 2048) return out;
    int x0 = 0, y0 = 0, w = width, h = height;
    if (width % 64 == 0 && height == width / 2) {
        const int scale = width / 64;
        x0 = scale; y0 = scale; w = 10 * scale; h = 16 * scale;
    }
    const int cols = std::min(w, 32), rows = std::min(h, 48);
    out.reserve(cols * rows);
    auto point = [](float x, float y) {
        // 10 x 16 model pixels, below the shoulders, just behind the torso.
        return wings::Vec3{-5.0f + 10.0f * x, 24.0f - 16.0f * y,
                           2.25f + 1.2f * y};
    };
    for (int y = 0; y < rows; ++y) for (int x = 0; x < cols; ++x) {
        const int sx = x0 + (2 * x + 1) * w / (2 * cols);
        const int sy = y0 + (2 * y + 1) * h / (2 * rows);
        const auto* pixel = rgba + (static_cast<std::size_t>(sy) * width + sx) * 4;
        if (pixel[3] < 128) continue;
        const float l = float(x) / cols, r = float(x + 1) / cols;
        const float t = float(y) / rows, b = float(y + 1) / rows;
        WingQuad q{};
        q.p[0] = point(l, t); q.p[1] = point(r, t);
        q.p[2] = point(r, b); q.p[3] = point(l, b);
        q.n = {0, 0, 1};
        q.front = q.back = {pixel[0], pixel[1], pixel[2]};
        for (int i = 0; i < 4; ++i) q.frontC[i] = q.backC[i] = q.back;
        q.emissive = 1.0f; // Keep the artwork's colours independent of camera angle.
        out.push_back(q);
    }
    return out;
}
} // namespace bedrocktools::modules::cape

#pragma once

// Text preview of a wing style for the mod menu.
//
// The launcher's module page can only render text: the config schema it reads
// carries titles, descriptions and choice labels, and the only image path
// (registerImage + DrawCommandType::Image) belongs to the in-game HUD overlay,
// which sits behind the menu. So the preview is a character silhouette.
//
// It is not hand-drawn art. The cells are filled by rasterizing the same quads
// buildWing() produces for the game, so every style really does show its own
// span, sweep and scalloped trailing edge instead of a canned string, and the
// preview can never drift away from the geometry.
//
// Only printable ASCII is used: the launcher's font is not guaranteed to be
// monospaced or to carry Unicode block glyphs, and a missing glyph renders as a
// tofu box that would destroy the whole picture.

#include "wings_design.hpp"
#include "wings_shape.hpp"
#include "wings_surface.hpp"

#include <string>
#include <vector>

namespace bedrocktools::modules::wings {

// Grid the preview is drawn on. Kept narrow enough for a phone-width module page
// and short enough to leave room for the settings below it.
constexpr int kWingArtCols = 29;
constexpr int kWingArtRows = 9;

// The idle pose the preview is drawn in: the same one the renderer starts from,
// so the menu shows the wings relaxed rather than mid-flap.
inline WingPose artPose() {
    WingPose pose;
    pose.shoulderDeg = 26.0f;
    pose.elbowDeg = 8.0f;
    pose.wristDeg = 9.0f;
    pose.phaseRad = 0.35f;
    pose.flutter = 0.30f;
    return pose;
}

// True when (x, y) is inside the quad, using the even-odd rule. The model-space
// quads are planar and convex, so this is exact.
inline bool insideQuadXY(const WingQuad& quad, float x, float y) {
    bool inside = false;
    for (int a = 0, b = 3; a < 4; b = a++) {
        const float y1 = quad.p[b].y;
        const float y2 = quad.p[a].y;
        if ((y1 <= y && y2 > y) || (y2 <= y && y1 > y)) {
            const float t = (y - y1) / (y2 - y1);
            if (x < quad.p[b].x + t * (quad.p[a].x - quad.p[b].x)) inside = !inside;
        }
    }
    return inside;
}

// Renders one style into `rows` lines of `cols` characters. Returns an empty
// string for an out-of-range style, so callers can fall back to plain text.
inline std::string wingStyleArt(int styleIndex, int cols = kWingArtCols,
                                int rows = kWingArtRows) {
    if (styleIndex < 0 || styleIndex >= kWingDesignCount) return {};
    if (cols < 4 || rows < 3) return {};

    static WingQuad quads[kMaxWingQuads];
    std::vector<WingQuad> all;
    const WingPose pose = artPose();
    for (int side = -1; side <= 1; side += 2) {
        const int count = buildWing(kWingDesigns[styleIndex], pose, side, quads, kMaxWingQuads);
        all.insert(all.end(), quads, quads + count);
    }
    if (all.empty()) return {};

    // Fit the whole wingspan, with a one-cell margin so the tips never touch the
    // frame.
    float minX = all[0].p[0].x, maxX = minX;
    float minY = all[0].p[0].y, maxY = minY;
    for (const WingQuad& quad : all) {
        for (int k = 0; k < 4; ++k) {
            if (quad.p[k].x < minX) minX = quad.p[k].x;
            if (quad.p[k].x > maxX) maxX = quad.p[k].x;
            if (quad.p[k].y < minY) minY = quad.p[k].y;
            if (quad.p[k].y > maxY) maxY = quad.p[k].y;
        }
    }
    if (!(maxX > minX) || !(maxY > minY)) return {};

    std::vector<std::string> grid(static_cast<std::size_t>(rows),
                                  std::string(static_cast<std::size_t>(cols), ' '));

    const float spanX = maxX - minX;
    const float spanY = maxY - minY;
    for (int r = 0; r < rows; ++r) {
        // Model +y is up, the text grid runs down.
        const float y = maxY - (static_cast<float>(r) + 0.5f) / static_cast<float>(rows) * spanY;
        for (int c = 0; c < cols; ++c) {
            const float x =
                minX + (static_cast<float>(c) + 0.5f) / static_cast<float>(cols) * spanX;
            for (const WingQuad& quad : all) {
                if (insideQuadXY(quad, x, y)) {
                    grid[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] = '#';
                    break;
                }
            }
        }
    }

    std::string out;
    out.reserve(static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols + 1));
    for (int r = 0; r < rows; ++r) {
        // Strip trailing blanks: the launcher may center or price-wrap the text,
        // and trailing spaces only make that worse.
        std::string line = grid[static_cast<std::size_t>(r)];
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line;
        if (r + 1 < rows) out += '\n';
    }
    return out;
}

}  // namespace bedrocktools::modules::wings

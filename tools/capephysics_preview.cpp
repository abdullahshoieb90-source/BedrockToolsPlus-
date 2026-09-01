// Offline preview renderer for the Cape Physics module.
//
// Simulates the exact cape cloth the game draws (modules/visual/
// capephysics_sim.hpp: Verlet sheet, gravity/wind/drag forces, constraints,
// body collision, per-cell palette sampling and face shading) and rasterizes
// it into PNG files, so the cape's look and motion can be checked without
// launching Minecraft.
//
// Panels:
//   rest.png    - the cape hanging at rest (view of the player's back)
//   run.png     - 6 phases of the cape while the player sprints
//   wind.png    - the cape in a strong gust
//   sizes.png   - the cape rendered from 22x23 / 64x32 / 128x64 / 704x736
//                 source images through the same any-size pipeline
//   detail.png  - Native (10x16) vs Fine (14x22) cloth grids
//
// Build and run (no game, no NDK):
//     ./scripts/gen_capephysics_preview.sh
// or manually:
//     g++ -std=c++20 -O2 -w -I src -I include -I third_party
//         tools/capephysics_preview.cpp -o build/capephysics_preview
//     ./build/capephysics_preview build/capephysics-preview

#include "modules/visual/capephysics_sim.hpp"
#include "modules/player/customcapes_files.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace cp = bedrocktools::modules::capephysics;
namespace ccc = customcapes;

namespace {

constexpr float kPi = 3.14159265358979323846f;

// ---------------------------------------------------------------------------
// Tiny software rasterizer (perspective camera + z-buffer, flat shading)
// ---------------------------------------------------------------------------

struct Camera {
    cp::Vec3 pos;
    cp::Vec3 target;
    float fovYDeg = 55.0f;
    float aspect = 1.0f;
};

struct FrameBuffer {
    int w = 0;
    int h = 0;
    std::vector<unsigned char> rgb;
    std::vector<float> depth;

    void resize(int width, int height) {
        w = width;
        h = height;
        rgb.assign(static_cast<std::size_t>(w) * h * 3, 0);
        depth.assign(static_cast<std::size_t>(w) * h, 1e30f);
    }

    void clearGradient(unsigned char r, unsigned char g, unsigned char b) {
        for (int y = 0; y < h; ++y) {
            const float t = static_cast<float>(y) / static_cast<float>(h > 1 ? h - 1 : 1);
            for (int x = 0; x < w; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 3;
                rgb[i + 0] = static_cast<unsigned char>(r * (1.0f - 0.45f * t));
                rgb[i + 1] = static_cast<unsigned char>(g * (1.0f - 0.45f * t));
                rgb[i + 2] = static_cast<unsigned char>(b * (1.0f - 0.45f * t));
            }
        }
        std::fill(depth.begin(), depth.end(), 1e30f);
    }
};

struct Projected {
    float sx = 0.0f;
    float sy = 0.0f;
    float depth = 0.0f;
    bool visible = false;
};

Projected project(const Camera& cam, const FrameBuffer& fb, const cp::Vec3& p) {
    const cp::Vec3 f = cp::normalized(cam.target - cam.pos);
    const cp::Vec3 up{0.0f, 1.0f, 0.0f};
    cp::Vec3 r = cp::cross(f, up);
    if (cp::length(r) < 1e-5f) r = cp::Vec3{1.0f, 0.0f, 0.0f};
    r = cp::normalized(r);
    const cp::Vec3 u = cp::cross(r, f);

    const cp::Vec3 d = p - cam.pos;
    const float x = cp::dot(d, r);
    const float y = cp::dot(d, u);
    const float z = cp::dot(d, f);

    Projected out;
    if (z <= 0.05f) return out;
    const float tanY = std::tan(cam.fovYDeg * kPi / 180.0f * 0.5f);
    const float tanX = tanY * cam.aspect;
    out.sx = fb.w * 0.5f * (1.0f + x / (z * tanX));
    out.sy = fb.h * 0.5f * (1.0f - y / (z * tanY));
    out.depth = z;
    out.visible = out.sx >= -64.0f && out.sx <= fb.w + 64.0f && out.sy >= -64.0f && out.sy <= fb.h + 64.0f;
    return out;
}

void fillTriangle(FrameBuffer& fb, const Projected& a, const Projected& b, const Projected& c,
                  const unsigned char rgb[3]) {
    if (!a.visible || !b.visible || !c.visible) return;
    const float minX = std::max(0.0f, std::min({a.sx, b.sx, c.sx}));
    const float maxX = std::min(static_cast<float>(fb.w - 1), std::max({a.sx, b.sx, c.sx}));
    const float minY = std::max(0.0f, std::min({a.sy, b.sy, c.sy}));
    const float maxY = std::min(static_cast<float>(fb.h - 1), std::max({a.sy, b.sy, c.sy}));
    if (minX > maxX || minY > maxY) return;

    const float area = (b.sx - a.sx) * (c.sy - a.sy) - (c.sx - a.sx) * (b.sy - a.sy);
    if (std::fabs(area) < 1e-9f) return;
    const float invArea = 1.0f / area;

    for (int py = static_cast<int>(minY); py <= static_cast<int>(maxY); ++py) {
        for (int px = static_cast<int>(minX); px <= static_cast<int>(maxX); ++px) {
            const float fx = static_cast<float>(px) + 0.5f;
            const float fy = static_cast<float>(py) + 0.5f;
            const float w0 = ((b.sx - fx) * (c.sy - fy) - (c.sx - fx) * (b.sy - fy)) * invArea;
            if (w0 < 0.0f) continue;
            const float w1 = ((c.sx - fx) * (a.sy - fy) - (a.sx - fx) * (c.sy - fy)) * invArea;
            if (w1 < 0.0f) continue;
            const float w2 = 1.0f - w0 - w1;
            if (w2 < 0.0f) continue;
            const float depth = w0 * a.depth + w1 * b.depth + w2 * c.depth;
            const std::size_t idx = static_cast<std::size_t>(py) * fb.w + px;
            if (depth > fb.depth[idx]) continue;
            fb.depth[idx] = depth;
            fb.rgb[idx * 3 + 0] = rgb[0];
            fb.rgb[idx * 3 + 1] = rgb[1];
            fb.rgb[idx * 3 + 2] = rgb[2];
        }
    }
}

// A flat-shaded quad with per-face lighting identical to the module's
// overlay (headlight + sky lift; see capephysics_sim.hpp).
void shadeAndDrawQuad(FrameBuffer& fb, const Camera& cam,
                      const cp::Vec3& a, const cp::Vec3& b,
                      const cp::Vec3& c, const cp::Vec3& d,
                      const unsigned char rgb[3], float brightnessBias = 1.0f) {
    const cp::Vec3 normal = cp::cross(b - a, c - a);
    const cp::Vec3 center = (a + b + c + d) * 0.25f;
    const float brightness =
        cp::faceBrightness(normal, center - cam.pos, cp::kDefaultLight) * brightnessBias;
    float shaded[3];
    unsigned char out[3];
    cp::shadeRgb(rgb, brightness, shaded);
    out[0] = static_cast<unsigned char>(shaded[0]);
    out[1] = static_cast<unsigned char>(shaded[1]);
    out[2] = static_cast<unsigned char>(shaded[2]);

    const Projected pa = project(cam, fb, a);
    const Projected pb = project(cam, fb, b);
    const Projected pc = project(cam, fb, c);
    const Projected pd = project(cam, fb, d);
    fillTriangle(fb, pa, pb, pc, out);
    fillTriangle(fb, pa, pc, pd, out);
}

void drawBox(FrameBuffer& fb, const Camera& cam, const cp::Vec3& min, const cp::Vec3& max,
             const unsigned char rgb[3]) {
    const cp::Vec3 v[8] = {
        {min.x, min.y, min.z}, {max.x, min.y, min.z}, {max.x, max.y, min.z}, {min.x, max.y, min.z},
        {min.x, min.y, max.z}, {max.x, min.y, max.z}, {max.x, max.y, max.z}, {min.x, max.y, max.z},
    };
    static const int faces[6][4] = {
        {0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7}, {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0},
    };
    for (const auto& f : faces) {
        shadeAndDrawQuad(fb, cam, v[f[0]], v[f[1]], v[f[2]], v[f[3]], rgb);
    }
}

// A simplified player mannequin standing at `feet` (same proportions as the
// vanilla model) so the cape reads against a body.
void drawPlayer(FrameBuffer& fb, const Camera& cam, const cp::Vec3& feet) {
    const unsigned char skin[3] = {198, 150, 118};
    const unsigned char shirt[3] = {42, 120, 168};
    const unsigned char pants[3] = {52, 62, 118};
    const float legH = 0.75f, bodyH = 0.75f, headH = 0.25f;

    // Legs.
    drawBox(fb, cam, {feet.x - 0.125f, feet.y, feet.z - 0.125f},
            {feet.x, feet.y + legH, feet.z + 0.125f}, pants);
    drawBox(fb, cam, {feet.x, feet.y, feet.z - 0.125f},
            {feet.x + 0.125f, feet.y + legH, feet.z + 0.125f}, pants);
    // Torso.
    drawBox(fb, cam, {feet.x - 0.25f, feet.y + legH, feet.z - 0.125f},
            {feet.x + 0.25f, feet.y + legH + bodyH, feet.z + 0.125f}, shirt);
    // Arms.
    drawBox(fb, cam, {feet.x - 0.375f, feet.y + legH, feet.z - 0.125f},
            {feet.x - 0.25f, feet.y + legH + bodyH, feet.z + 0.125f}, shirt);
    drawBox(fb, cam, {feet.x + 0.25f, feet.y + legH, feet.z - 0.125f},
            {feet.x + 0.375f, feet.y + legH + bodyH, feet.z + 0.125f}, shirt);
    // Head.
    drawBox(fb, cam, {feet.x - 0.125f, feet.y + legH + bodyH, feet.z - 0.125f},
            {feet.x + 0.125f, feet.y + legH + bodyH + headH, feet.z + 0.125f}, skin);
}

// ---------------------------------------------------------------------------
// Cape rendering (mirrors CapePhysicsModule's overlay emission)
// ---------------------------------------------------------------------------

void drawCape(FrameBuffer& fb, const Camera& cam, const cp::Cloth& cloth,
              const cp::CapePalette& palette, const cp::Vec3& feet) {
    const int cols = cloth.cols();
    const int rows = cloth.rows();
    const float halfThick = cp::kCapeThicknessBlocks * 0.5f;

    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < cols; ++i) {
            const cp::Vec3 p00 = cloth.at(i, j);
            const cp::Vec3 p10 = cloth.at(i + 1, j);
            const cp::Vec3 p11 = cloth.at(i + 1, j + 1);
            const cp::Vec3 p01 = cloth.at(i, j + 1);

            cp::Vec3 normal = cp::cross(p10 - p00, p01 - p00);
            const cp::Vec3 center = (p00 + p10 + p11 + p01) * 0.25f;
            // The outer face points away from the body axis, exactly like
            // the module's overlay emission.
            const cp::Vec3 axisPoint{feet.x, center.y, feet.z};
            if (cp::dot(normal, center - axisPoint) < 0.0f) {
                normal = normal * -1.0f;
            }
            const cp::Vec3 n = cp::normalized(normal);

            const std::size_t cell = (static_cast<std::size_t>(j) * cols + i) * 3u;
            const unsigned char outerCol[3] = {palette.outer[cell + 0], palette.outer[cell + 1], palette.outer[cell + 2]};
            const unsigned char innerCol[3] = {palette.inner[cell + 0], palette.inner[cell + 1], palette.inner[cell + 2]};

            shadeAndDrawQuad(fb, cam, p00 + n * halfThick, p10 + n * halfThick,
                             p11 + n * halfThick, p01 + n * halfThick, outerCol);
            shadeAndDrawQuad(fb, cam, p00 - n * halfThick, p10 - n * halfThick,
                             p11 - n * halfThick, p01 - n * halfThick, innerCol, 0.92f);

            if (i == 0) {
                const std::size_t er = static_cast<std::size_t>(j) * 3u;
                const unsigned char col[3] = {palette.edgeLeft[er + 0], palette.edgeLeft[er + 1], palette.edgeLeft[er + 2]};
                shadeAndDrawQuad(fb, cam, p00 + n * halfThick, p00 - n * halfThick,
                                 p01 - n * halfThick, p01 + n * halfThick, col);
            }
            if (i == cols - 1) {
                const std::size_t er = static_cast<std::size_t>(j) * 3u;
                const unsigned char col[3] = {palette.edgeRight[er + 0], palette.edgeRight[er + 1], palette.edgeRight[er + 2]};
                shadeAndDrawQuad(fb, cam, p10 - n * halfThick, p10 + n * halfThick,
                                 p11 + n * halfThick, p11 - n * halfThick, col);
            }
            if (j == 0) {
                const std::size_t ec = static_cast<std::size_t>(i) * 3u;
                const unsigned char col[3] = {palette.edgeTop[ec + 0], palette.edgeTop[ec + 1], palette.edgeTop[ec + 2]};
                shadeAndDrawQuad(fb, cam, p00 + n * halfThick, p10 + n * halfThick,
                                 p10 - n * halfThick, p00 - n * halfThick, col);
            }
            if (j == rows - 1) {
                const std::size_t ec = static_cast<std::size_t>(i) * 3u;
                const unsigned char col[3] = {palette.edgeBottom[ec + 0], palette.edgeBottom[ec + 1], palette.edgeBottom[ec + 2]};
                shadeAndDrawQuad(fb, cam, p01 - n * halfThick, p11 - n * halfThick,
                                 p11 + n * halfThick, p01 + n * halfThick, col);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Test cape artwork
// ---------------------------------------------------------------------------

// Paints the classic 64x32 cape canvas: rainbow-striped outer face, dark
// lining, darker edge strips.
std::vector<std::uint8_t> stripedCapeCanvas() {
    std::vector<std::uint8_t> canvas(cp::kCanvasBytes, 0);
    const auto put = [&](std::uint32_t x, std::uint32_t y, unsigned char r, unsigned char g, unsigned char b) {
        if (x >= cp::kCanvasWidth || y >= cp::kCanvasHeight) return;
        const std::size_t i = (static_cast<std::size_t>(y) * cp::kCanvasWidth + x) * 4u;
        canvas[i + 0] = r;
        canvas[i + 1] = g;
        canvas[i + 2] = b;
        canvas[i + 3] = 255;
    };
    for (int j = 0; j < 16; ++j) {
        const float t = static_cast<float>(j) / 15.0f;
        const unsigned char r = static_cast<unsigned char>(90 + 165 * t);
        const unsigned char g = static_cast<unsigned char>(40 + 130 * (1.0f - t));
        const unsigned char b = static_cast<unsigned char>(150 + 80 * t);
        for (int i = 0; i < 10; ++i) {
            put(1 + i, 1 + j, r, g, b);        // outer face
            put(12 + i, 1 + j, 60, 55, 70);    // lining
        }
        put(0, 1 + j, 40, 40, 55);             // left strip
        put(11, 1 + j, 40, 40, 55);            // right strip
    }
    for (int i = 0; i < 10; ++i) {
        put(1 + i, 0, 40, 40, 55);             // top strip
        put(11 + i, 0, 30, 30, 45);            // bottom strip
    }
    return canvas;
}

// The same striped design authored at an arbitrary source resolution.
std::vector<std::uint8_t> stripedSource(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> src(static_cast<std::size_t>(w) * h * 4, 0);
    for (std::uint32_t y = 0; y < h; ++y) {
        const float t = static_cast<float>(y) / static_cast<float>(h - 1);
        const unsigned char r = static_cast<unsigned char>(90 + 165 * t);
        const unsigned char g = static_cast<unsigned char>(40 + 130 * (1.0f - t));
        const unsigned char b = static_cast<unsigned char>(150 + 80 * t);
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4u;
            src[i + 0] = r;
            src[i + 1] = g;
            src[i + 2] = b;
            src[i + 3] = 255;
        }
    }
    return src;
}

// ---------------------------------------------------------------------------
// Scene helpers
// ---------------------------------------------------------------------------

struct Panel {
    FrameBuffer fb;
    cp::Vec3 feet{0.0f, 0.0f, 0.0f};
};

void renderScene(Panel& panel, const cp::Cloth& cloth, const cp::CapePalette& palette,
                 const cp::Vec3& camOffset) {
    panel.fb.clearGradient(96, 132, 176);
    Camera cam;
    cam.pos = panel.feet + camOffset;
    cam.target = panel.feet + cp::Vec3{0.0f, 1.1f, 0.0f};
    cam.aspect = static_cast<float>(panel.fb.w) / static_cast<float>(panel.fb.h);
    drawPlayer(panel.fb, cam, panel.feet);
    drawCape(panel.fb, cam, cloth, palette, panel.feet);
}

bool writePng(const std::string& path, const FrameBuffer& fb) {
    const int stride = fb.w * 3;
    return stbi_write_png(path.c_str(), fb.w, fb.h, 3, fb.rgb.data(), stride) != 0;
}

// Simulates `seconds` at 60 Hz with the given body/params and returns the
// cloth (the caller keeps the body walking by mutating feet/yaw per call).
void simulate(cp::Cloth& cloth, cp::BodyFrame& body, const cp::ClothParams& params,
              float seconds, cp::Vec3 anchors[]) {
    const int cols = cloth.cols();
    const int steps = static_cast<int>(seconds * 60.0f);
    for (int s = 0; s < steps; ++s) {
        cp::buildAnchors(body, cols, anchors);
        cloth.step(1.0f / 60.0f, anchors, body, params, static_cast<float>(s) / 60.0f);
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string outdir = argc > 1 ? argv[1] : "build/capephysics-preview";
    std::printf("cape physics preview -> %s\n", outdir.c_str());

    const std::vector<std::uint8_t> canvas = stripedCapeCanvas();
    cp::Vec3 anchors[64];

    // ------------------------------------------------------------------
    // rest.png — hanging at rest.
    // ------------------------------------------------------------------
    {
        cp::Cloth cloth;
        cloth.configure(10, 16);
        cp::BodyFrame body{{0.0f, 0.0f, 0.0f}, 0.0f, 1.8f};
        cp::ClothParams params; // defaults
        simulate(cloth, body, params, 3.0f, anchors);

        cp::CapePalette palette;
        cp::buildCapePalette(canvas.data(), 64, 32, 10, 16, palette);

        Panel panel;
        panel.fb.resize(480, 640);
        renderScene(panel, cloth, palette, cp::Vec3{0.0f, 1.7f, -3.6f});
        writePng(outdir + "/rest.png", panel.fb);
        std::printf("  rest.png\n");
    }

    // ------------------------------------------------------------------
    // run.png — sprinting (6 phases in a 2x3 grid).
    // ------------------------------------------------------------------
    {
        const int pw = 300, ph = 400;
        FrameBuffer sheet;
        sheet.resize(pw * 3, ph * 2);

        cp::Cloth cloth;
        cloth.configure(10, 16);
        cp::BodyFrame body{{0.0f, 0.0f, 0.0f}, 0.0f, 1.8f};
        cp::ClothParams params;
        params.windMul = 0.2f;

        cp::CapePalette palette;
        cp::buildCapePalette(canvas.data(), 64, 32, 10, 16, palette);

        const float speed = 4.3f; // sprint
        for (int phase = 0; phase < 6; ++phase) {
            // One second of sprinting between phases.
            for (int s = 0; s < 60; ++s) {
                body.feet.x += speed / 60.0f;
                cp::buildAnchors(body, 10, anchors);
                cloth.step(1.0f / 60.0f, anchors, body, params, phase * 1.0f + s / 60.0f);
            }

            Panel panel;
            panel.feet = body.feet; // the camera follows the runner
            panel.fb.resize(pw, ph);
            renderScene(panel, cloth, palette, cp::Vec3{0.0f, 1.7f, -3.6f});

            const int ox = (phase % 3) * pw;
            const int oy = (phase / 3) * ph;
            for (int y = 0; y < ph; ++y) {
                for (int x = 0; x < pw; ++x) {
                    const std::size_t src = (static_cast<std::size_t>(y) * pw + x) * 3;
                    const std::size_t dst = (static_cast<std::size_t>(oy + y) * (pw * 3) + ox + x) * 3;
                    sheet.rgb[dst + 0] = panel.fb.rgb[src + 0];
                    sheet.rgb[dst + 1] = panel.fb.rgb[src + 1];
                    sheet.rgb[dst + 2] = panel.fb.rgb[src + 2];
                }
            }
        }
        writePng(outdir + "/run.png", sheet);
        std::printf("  run.png\n");
    }

    // ------------------------------------------------------------------
    // wind.png — strong gust (2x2: 0s / 1s / 2s / 3s).
    // ------------------------------------------------------------------
    {
        const int pw = 320, ph = 420;
        FrameBuffer sheet;
        sheet.resize(pw * 2, ph * 2);

        cp::Cloth cloth;
        cloth.configure(10, 16);
        cp::BodyFrame body{{0.0f, 0.0f, 0.0f}, 0.0f, 1.8f};
        cp::ClothParams params;
        params.windMul = 2.0f; // maximum storm setting

        cp::CapePalette palette;
        cp::buildCapePalette(canvas.data(), 64, 32, 10, 16, palette);

        for (int phase = 0; phase < 4; ++phase) {
            for (int s = 0; s < 60; ++s) {
                cp::buildAnchors(body, 10, anchors);
                cloth.step(1.0f / 60.0f, anchors, body, params, phase * 1.0f + s / 60.0f);
            }
            Panel panel;
            panel.fb.resize(pw, ph);
            renderScene(panel, cloth, palette, cp::Vec3{0.0f, 1.7f, -4.2f});

            const int ox = (phase % 2) * pw;
            const int oy = (phase / 2) * ph;
            for (int y = 0; y < ph; ++y) {
                for (int x = 0; x < pw; ++x) {
                    const std::size_t src = (static_cast<std::size_t>(y) * pw + x) * 3;
                    const std::size_t dst = (static_cast<std::size_t>(oy + y) * (pw * 2) + ox + x) * 3;
                    sheet.rgb[dst + 0] = panel.fb.rgb[src + 0];
                    sheet.rgb[dst + 1] = panel.fb.rgb[src + 1];
                    sheet.rgb[dst + 2] = panel.fb.rgb[src + 2];
                }
            }
        }
        writePng(outdir + "/wind.png", sheet);
        std::printf("  wind.png\n");
    }

    // ------------------------------------------------------------------
    // sizes.png — the any-size pipeline: the same design authored at four
    // different resolutions (22x23 .. 704x736) renders identically.
    // ------------------------------------------------------------------
    {
        const int pw = 320, ph = 420;
        FrameBuffer sheet;
        sheet.resize(pw * 4, ph);

        struct SizeCase { std::uint32_t w, h; };
        const SizeCase sizes[] = {{22, 23}, {64, 32}, {128, 64}, {704, 736}};

        for (int phase = 0; phase < 4; ++phase) {
            const std::vector<std::uint8_t> src = stripedSource(sizes[phase].w, sizes[phase].h);
            const std::vector<std::uint8_t> capeCanvas = ccc::resampleToCape(src.data(), sizes[phase].w, sizes[phase].h);

            cp::CapePalette palette;
            cp::buildCapePalette(capeCanvas.data(), 64, 32, 10, 16, palette);

            cp::Cloth cloth;
            cloth.configure(10, 16);
            cp::BodyFrame body{{0.0f, 0.0f, 0.0f}, 0.0f, 1.8f};
            cp::ClothParams params;
            simulate(cloth, body, params, 3.0f, anchors);

            Panel panel;
            panel.fb.resize(pw, ph);
            renderScene(panel, cloth, palette, cp::Vec3{0.0f, 1.7f, -3.6f});

            const int ox = phase * pw;
            for (int y = 0; y < ph; ++y) {
                for (int x = 0; x < pw; ++x) {
                    const std::size_t srcI = (static_cast<std::size_t>(y) * pw + x) * 3;
                    const std::size_t dst = (static_cast<std::size_t>(y) * (pw * 4) + ox + x) * 3;
                    sheet.rgb[dst + 0] = panel.fb.rgb[srcI + 0];
                    sheet.rgb[dst + 1] = panel.fb.rgb[srcI + 1];
                    sheet.rgb[dst + 2] = panel.fb.rgb[srcI + 2];
                }
            }
        }
        writePng(outdir + "/sizes.png", sheet);
        std::printf("  sizes.png\n");
    }

    // ------------------------------------------------------------------
    // detail.png — Native (10x16) vs Fine (14x22) grids, gently windy so
    // the folds differ.
    // ------------------------------------------------------------------
    {
        const int pw = 320, ph = 420;
        FrameBuffer sheet;
        sheet.resize(pw * 2, ph);

        cp::CapePalette nativePalette, finePalette;
        cp::buildCapePalette(canvas.data(), 64, 32, 10, 16, nativePalette);
        cp::buildCapePalette(canvas.data(), 64, 32, 14, 22, finePalette);

        for (int phase = 0; phase < 2; ++phase) {
            cp::Cloth cloth;
            cloth.configure(phase == 0 ? 10 : 14, phase == 0 ? 16 : 22);
            cp::BodyFrame body{{0.0f, 0.0f, 0.0f}, 0.0f, 1.8f};
            cp::ClothParams params;
            params.windMul = 0.9f;
            simulate(cloth, body, params, 2.5f, anchors);

            Panel panel;
            panel.fb.resize(pw, ph);
            renderScene(panel, cloth, phase == 0 ? nativePalette : finePalette,
                        cp::Vec3{0.0f, 1.7f, -3.6f});

            const int ox = phase * pw;
            for (int y = 0; y < ph; ++y) {
                for (int x = 0; x < pw; ++x) {
                    const std::size_t srcI = (static_cast<std::size_t>(y) * pw + x) * 3;
                    const std::size_t dst = (static_cast<std::size_t>(y) * (pw * 2) + ox + x) * 3;
                    sheet.rgb[dst + 0] = panel.fb.rgb[srcI + 0];
                    sheet.rgb[dst + 1] = panel.fb.rgb[srcI + 1];
                    sheet.rgb[dst + 2] = panel.fb.rgb[srcI + 2];
                }
            }
        }
        writePng(outdir + "/detail.png", sheet);
        std::printf("  detail.png\n");
    }

    std::printf("done\n");
    return 0;
}

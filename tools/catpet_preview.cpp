// Offline preview renderer for the Cat Pet module.
//
// Draws the exact cat geometry the game draws (modules/visual/catpet_shape.hpp:
// part hierarchy, pose solver, palette and face shading) into PNG files, so
// the pet can be judged without launching Minecraft.
//
// Build and run (no game, no NDK):
//     ./scripts/gen_catpet_preview.sh
// or manually:
//     g++ -std=c++20 -O2 -w -I src -I include -I third_party
//         tools/catpet_preview.cpp -o build/catpet_preview
//     ./build/catpet_preview build/catpet-preview
//
// Output: <outdir>/<style>.png (2x3 poses per style) and
// <outdir>/sheet.png (all styles side by side, idle pose).

#include "modules/visual/catpet_shape.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace catpet = bedrocktools::modules::catpet;
namespace wings = bedrocktools::modules::wings;

namespace {

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

    void clear(unsigned char r, unsigned char g, unsigned char b) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 3;
                const float t = static_cast<float>(y) / static_cast<float>(h > 1 ? h - 1 : 1);
                rgb[i + 0] = static_cast<unsigned char>(r * (1.0f - 0.35f * t));
                rgb[i + 1] = static_cast<unsigned char>(g * (1.0f - 0.35f * t));
                rgb[i + 2] = static_cast<unsigned char>(b * (1.0f - 0.25f * t));
                depth[static_cast<std::size_t>(y) * w + x] = 1e30f;
            }
        }
    }

    void set(int x, int y, float z, wings::FaceColor c) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        const std::size_t i = static_cast<std::size_t>(y) * w + x;
        if (z >= depth[i]) return;
        depth[i] = z;
        rgb[i * 3 + 0] = c.r;
        rgb[i * 3 + 1] = c.g;
        rgb[i * 3 + 2] = c.b;
    }
};

struct Camera {
    wings::Vec3 pos;
    wings::Vec3 forward;
    wings::Vec3 right;
    wings::Vec3 up;
    float scale = 1.0f;
};

Camera lookAt(wings::Vec3 eye, wings::Vec3 target, float pixelScale) {
    Camera cam;
    cam.pos = eye;
    cam.forward = wings::normalized(target - eye);
    cam.right = wings::normalized(wings::Vec3{-cam.forward.z, 0.0f, cam.forward.x});
    cam.up = wings::Vec3{
        cam.right.y * cam.forward.z - cam.right.z * cam.forward.y,
        cam.right.z * cam.forward.x - cam.right.x * cam.forward.z,
        cam.right.x * cam.forward.y - cam.right.y * cam.forward.x};
    cam.scale = pixelScale;
    return cam;
}

struct ScreenPoint {
    float x = 0.0f;
    float y = 0.0f;
    float depth = 0.0f;
    bool visible = false;
};

ScreenPoint project(const Camera& cam, wings::Vec3 p, int cx, int cy) {
    const wings::Vec3 d = p - cam.pos;
    const float depth = wings::dot(d, cam.forward);
    ScreenPoint out;
    out.depth = depth;
    if (depth < 0.05f) return out;
    out.x = static_cast<float>(cx) + wings::dot(d, cam.right) * cam.scale / depth;
    out.y = static_cast<float>(cy) - wings::dot(d, cam.up) * cam.scale / depth;
    out.visible = true;
    return out;
}

void fillTriangle(FrameBuffer& fb, const ScreenPoint& a, const ScreenPoint& b, const ScreenPoint& c,
                  wings::FaceColor color) {
    float minX = a.x, maxX = a.x;
    float minY = a.y, maxY = a.y;
    const ScreenPoint* pts[3] = {&a, &b, &c};
    for (const ScreenPoint* p : pts) {
        if (p->x < minX) minX = p->x;
        if (p->x > maxX) maxX = p->x;
        if (p->y < minY) minY = p->y;
        if (p->y > maxY) maxY = p->y;
    }
    const int x0 = static_cast<int>(std::floor(minX));
    const int x1 = static_cast<int>(std::ceil(maxX));
    const int y0 = static_cast<int>(std::floor(minY));
    const int y1 = static_cast<int>(std::ceil(maxY));
    if (x1 < 0 || y1 < 0 || x0 >= fb.w || y0 >= fb.h) return;

    const float denom = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (std::fabs(denom) < 1e-8f) return;

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float w0 = ((b.y - c.y) * (px - c.x) + (c.x - b.x) * (py - c.y)) / denom;
            const float w1 = ((c.y - a.y) * (px - c.x) + (a.x - c.x) * (py - c.y)) / denom;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            const float z = w0 * a.depth + w1 * b.depth + w2 * c.depth;
            fb.set(x, y, z, color);
        }
    }
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------

struct Scene {
    Camera cam;
    int centerX = 0;
    int centerY = 0;
};

void drawGround(FrameBuffer& fb, const Scene& scene) {
    // Checkerboard ground plane so the paw contact reads.
    const unsigned char a[3] = {96, 148, 88};
    const unsigned char b[3] = {84, 132, 78};
    for (int gx = -4; gx < 4; ++gx) {
        for (int gz = -4; gz < 4; ++gz) {
            const unsigned char* c = ((gx + gz) & 1) ? a : b;
            wings::Vec3 p[4] = {
                {static_cast<float>(gx), 0.0f, static_cast<float>(gz)},
                {static_cast<float>(gx + 1), 0.0f, static_cast<float>(gz)},
                {static_cast<float>(gx + 1), 0.0f, static_cast<float>(gz + 1)},
                {static_cast<float>(gx), 0.0f, static_cast<float>(gz + 1)},
            };
            ScreenPoint q[4];
            bool vis = true;
            for (int i = 0; i < 4; ++i) {
                q[i] = project(scene.cam, p[i], scene.centerX, scene.centerY);
                if (!q[i].visible) vis = false;
            }
            if (!vis) continue;
            wings::FaceColor col{c[0], c[1], c[2]};
            fillTriangle(fb, q[0], q[1], q[2], col);
            fillTriangle(fb, q[0], q[2], q[3], col);
        }
    }
}

void drawCat(FrameBuffer& fb, const Scene& scene, int styleIndex, const catpet::CatPose& pose,
             float yawDeg, float scale) {
    if (styleIndex < 0 || styleIndex >= catpet::kCatStyleCount) styleIndex = 0;
    const catpet::CatStyle& style = catpet::kCatStyles[styleIndex];

    wings::Vec3 right, forward;
    catpet::catBasis(yawDeg, right, forward);
    const wings::Vec3 origin{0.0f, 0.0f, 0.0f};

    catpet::Affine xforms[catpet::kCatPartCount];
    catpet::partTransforms(pose, xforms);

    for (int i = 0; i < catpet::kCatPartCount; ++i) {
        wings::Vec3 local[wings::kCornerCount];
        catpet::buildPartCorners(i, xforms[i], pose.blink, local);

        wings::Vec3 world[wings::kCornerCount];
        for (int c = 0; c < wings::kCornerCount; ++c) {
            world[c] = catpet::catPointToWorld(local[c], origin, right, forward, scale);
        }

        ScreenPoint sp[wings::kCornerCount];
        for (int c = 0; c < wings::kCornerCount; ++c) {
            sp[c] = project(scene.cam, world[c], scene.centerX, scene.centerY);
        }

        int slots[wings::kFaceCount];
        catpet::partFaceSlots(i, slots);

        for (int f = 0; f < wings::kFaceCount; ++f) {
            const int* ring = wings::kFaceRings[f];
            ScreenPoint q[4];
            bool allVisible = true;
            for (int k = 0; k < 4; ++k) {
                q[k] = sp[ring[k]];
                if (!q[k].visible) allVisible = false;
            }
            if (!allVisible) continue;

            wings::Vec3 center{0.0f, 0.0f, 0.0f};
            for (int k = 0; k < 4; ++k) center = center + world[ring[k]];
            center = center * 0.25f;

            const wings::Vec3 nLocal = catpet::matApply(xforms[i].r, catpet::faceNormalLocal(f));
            const wings::Vec3 normal = catpet::catDirToWorld(nLocal, right, forward);
            const wings::Vec3 toCamera = scene.cam.pos - center;
            const float brightness =
                wings::faceBrightness(normal, toCamera, wings::kDefaultLight) * catpet::kCatParts[i].tint;
            const wings::FaceColor color = wings::shadeFace(style.rgb[slots[f]], brightness);

            fillTriangle(fb, q[0], q[1], q[2], color);
            fillTriangle(fb, q[0], q[2], q[3], color);
        }
    }
}

Scene makeScene(float orbitDeg, float height, float distance, int panelW, int panelH) {
    Scene scene;
    scene.centerX = panelW / 2;
    scene.centerY = panelH / 2;
    const wings::Vec3 center{0.0f, 0.55f, 0.0f};
    const float orbit = orbitDeg * wings::kDegToRad;
    const wings::Vec3 dir{std::sin(orbit), 0.0f, -std::cos(orbit)};
    const wings::Vec3 eye = center + dir * distance + wings::Vec3{0.0f, height, 0.0f};
    scene.cam = lookAt(eye, center, static_cast<float>(panelH) * 0.9f);
    return scene;
}

void pastePanel(FrameBuffer& dst, const FrameBuffer& panel, int ox, int oy) {
    for (int y = 0; y < panel.h; ++y) {
        for (int x = 0; x < panel.w; ++x) {
            const std::size_t src = (static_cast<std::size_t>(y) * panel.w + x) * 3;
            const std::size_t dstI = (static_cast<std::size_t>(oy + y) * dst.w + (ox + x)) * 3;
            dst.rgb[dstI + 0] = panel.rgb[src + 0];
            dst.rgb[dstI + 1] = panel.rgb[src + 1];
            dst.rgb[dstI + 2] = panel.rgb[src + 2];
        }
    }
}

bool writePng(const std::string& path, const FrameBuffer& fb) {
    if (stbi_write_png(path.c_str(), fb.w, fb.h, 3, fb.rgb.data(), fb.w * 3) != 0) {
        std::printf("wrote %s\n", path.c_str());
        return true;
    }
    std::printf("FAILED to write %s\n", path.c_str());
    return false;
}

struct PosePreset {
    const char* name;
    float t;
    float stridePhase;
    float move;
    float sit;
    float orbitDeg;
};

// 2x3 grid per style: idle front-ish, idle side, mid-stride walk, full run,
// sitting, and a blink close-up moment.
constexpr PosePreset kPoses[] = {
    {"idle",  1.2f, 0.0f, 0.0f, 0.0f, 25.0f},
    {"side",  3.4f, 0.0f, 0.0f, 0.0f, 90.0f},
    {"walk",  5.0f, 1.2f, 0.55f, 0.0f, 55.0f},
    {"run",   6.0f, 2.2f, 1.0f, 0.0f, 65.0f},
    {"sit",   8.0f, 0.0f, 0.0f, 1.0f, 30.0f},
    {"blink", 2.5745f, 0.0f, 0.0f, 0.0f, 5.0f},  // t chosen near a blink spike
};
constexpr int kPoseCount = static_cast<int>(sizeof(kPoses) / sizeof(kPoses[0]));

}  // namespace

int main(int argc, char** argv) {
    const std::string outdir = argc > 1 ? argv[1] : "build/catpet-preview";

    const int panelW = 380;
    const int panelH = 320;

    // Find an actual blink moment for the "blink" pose.
    float blinkT = 2.5745f;
    float bestBlink = 0.0f;
    for (float t = 0.0f; t < 25.0f; t += 0.002f) {
        const catpet::CatPose p = catpet::computeCatPose(t, 0.0f, 0.0f, 0.0f);
        if (p.blink > bestBlink) {
            bestBlink = p.blink;
            blinkT = t;
        }
    }

    for (int s = 0; s < catpet::kCatStyleCount; ++s) {
        FrameBuffer sheet;
        sheet.resize(panelW * 3, panelH * 2);
        for (int p = 0; p < kPoseCount; ++p) {
            PosePreset preset = kPoses[p];
            if (std::string(preset.name) == "blink") preset.t = blinkT;

            FrameBuffer panel;
            panel.resize(panelW, panelH);
            panel.clear(96, 132, 170);

            Scene scene = makeScene(preset.orbitDeg, 0.85f, 2.6f, panelW, panelH);
            drawGround(panel, scene);

            const catpet::CatPose pose =
                catpet::computeCatPose(preset.t, preset.stridePhase, preset.move, preset.sit);
            drawCat(panel, scene, s, pose, 180.0f + preset.orbitDeg * 0.0f, 1.0f);

            pastePanel(sheet, panel, (p % 3) * panelW, (p / 3) * panelH);
        }
        writePng(outdir + "/" + catpet::kCatStyles[s].id + ".png", sheet);
    }

    // One overview sheet: idle pose, every style.
    FrameBuffer overview;
    overview.resize(panelW * catpet::kCatStyleCount, panelH);
    for (int s = 0; s < catpet::kCatStyleCount; ++s) {
        FrameBuffer panel;
        panel.resize(panelW, panelH);
        panel.clear(96, 132, 170);
        Scene scene = makeScene(30.0f, 0.85f, 2.6f, panelW, panelH);
        drawGround(panel, scene);
        const catpet::CatPose pose = catpet::computeCatPose(1.2f, 0.0f, 0.0f, 0.0f);
        drawCat(panel, scene, s, pose, 180.0f, 1.0f);
        pastePanel(overview, panel, s * panelW, 0);
    }
    writePng(outdir + "/sheet.png", overview);

    return 0;
}

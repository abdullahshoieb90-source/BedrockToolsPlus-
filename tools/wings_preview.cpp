// Offline preview renderer for the Wings module.
//
// Draws the exact wing geometry the game draws (modules/visual/wings_styles.hpp
// + wings_shape.hpp, including the rest-pose fan, the taper, the sweep and the
// face shading) into PNG files, so a wing style can be checked without
// launching Minecraft. It is also the visual regression check for the overlay:
// the legacy_* output reproduces the old renderer (broken face rings, flat
// unshaded colors, no fan/taper/sweep) so the improvement is visible at a
// glance.
//
// Build and run (no game, no NDK):
//     ./scripts/gen_wings_preview.sh
// or manually:
//     g++ -std=c++20 -O2 -w -I src -I include -I third_party
//         tools/wings_preview.cpp -o build/wings_preview
//     ./build/wings_preview build/wings-preview
//
// Output: <outdir>/<style>.png for every style (2x2 poses),
// <outdir>/legacy_dragon.png and <outdir>/compare_dragon.png
// (left column: legacy, right column: current).

#include "modules/visual/wings_shape.hpp"
#include "modules/visual/wings_styles.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace wings = bedrocktools::modules::wings;

namespace {

// ---------------------------------------------------------------------------
// Framebuffer with a depth buffer
// ---------------------------------------------------------------------------

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
                // Vertical gradient so the wings read against the background.
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

// ---------------------------------------------------------------------------
// Camera (all rasterization happens in absolute panel pixels)
// ---------------------------------------------------------------------------

struct Camera {
    wings::Vec3 pos;
    wings::Vec3 forward;
    wings::Vec3 right;
    wings::Vec3 up;
    float scale = 1.0f;  // pixels per block at 1 block from the eye
};

Camera lookAt(wings::Vec3 eye, wings::Vec3 target, float pixelScale) {
    Camera cam;
    cam.pos = eye;
    cam.forward = wings::normalized(target - eye);
    // right = forward x up with up = (0, 1, 0)
    cam.right = wings::normalized(wings::Vec3{-cam.forward.z, 0.0f, cam.forward.x});
    cam.up = wings::Vec3{
        cam.right.y * cam.forward.z - cam.right.z * cam.forward.y,
        cam.right.z * cam.forward.x - cam.right.x * cam.forward.z,
        cam.right.x * cam.forward.y - cam.right.y * cam.forward.x};
    cam.scale = pixelScale;
    return cam;
}

struct ScreenPoint {
    float x = 0.0f;  // absolute panel pixels
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

// ---------------------------------------------------------------------------
// Rasterizer
// ---------------------------------------------------------------------------

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
    wings::Vec3 right{0.0f, 0.0f, 0.0f};    // player's right in world space
    wings::Vec3 forward{0.0f, 0.0f, 0.0f};  // player's forward in world space
    wings::Vec3 feet{0.0f, 0.0f, 0.0f};
    int centerX = 0;
    int centerY = 0;
    bool legacy = false;  // old renderer: flat colors, broken rings, plain boxes
};

// Legacy face table (kept only so the preview can reproduce the old look).
constexpr int kLegacyRings[6][4] = {
    {0, 1, 2, 3}, {4, 5, 6, 7}, {0, 3, 7, 4}, {1, 2, 6, 5}, {0, 1, 5, 4}, {3, 2, 6, 7},
};

void drawPrism(FrameBuffer& fb, const Scene& scene, const wings::WingBox& box, const wings::Pose2D& pose,
               const unsigned char* faceColors[6], float tint) {
    wings::Vec3 world[8];
    for (int i = 0; i < 8; ++i) {
        world[i] = wings::modelPointToWorld(box.px[i], box.py[i], box.pz[i], scene.feet, scene.right, scene.forward);
    }

    ScreenPoint sp[8];
    for (int i = 0; i < 8; ++i) sp[i] = project(scene.cam, world[i], scene.centerX, scene.centerY);

    for (int f = 0; f < wings::kFaceCount; ++f) {
        const int* ring = scene.legacy ? kLegacyRings[f] : wings::kFaceRings[f];

        ScreenPoint q[4];
        bool allVisible = true;
        for (int i = 0; i < 4; ++i) {
            q[i] = sp[ring[i]];
            if (!q[i].visible) allVisible = false;
        }
        if (!allVisible) continue;

        wings::FaceColor color;
        if (scene.legacy) {
            color = wings::shadeFace(faceColors[f], 1.0f);
        } else {
            wings::Vec3 center{0.0f, 0.0f, 0.0f};
            for (int i = 0; i < 4; ++i) center = center + world[ring[i]];
            center = center * 0.25f;
            const wings::Vec3 toCamera = scene.cam.pos - center;
            const wings::Vec3 normal =
                wings::modelDirToWorld(wings::faceNormalModel(f, pose), scene.right, scene.forward);
            color = wings::shadeFace(faceColors[f],
                                     wings::faceBrightness(normal, toCamera, wings::kDefaultLight) * tint);
        }

        fillTriangle(fb, q[0], q[1], q[2], color);
        fillTriangle(fb, q[0], q[2], q[3], color);
    }
}

wings::WingBox boxFromRange(float x0, float y0, float z0, float x1, float y1, float z1) {
    wings::WingBox box{};
    for (int i = 0; i < 8; ++i) {
        box.px[i] = (i & 1) ? x1 : x0;
        box.py[i] = ((i >> 1) & 1) ? y1 : y0;
        box.pz[i] = ((i >> 2) & 1) ? z1 : z0;
    }
    return box;
}

// Simple player mannequin so the wings can be judged in context.
void drawPlayer(FrameBuffer& fb, const Scene& scene) {
    const wings::Pose2D identity{};
    const unsigned char body[3] = {86, 96, 112};
    const unsigned char dark[3] = {62, 70, 84};
    const unsigned char* colors[6] = {body, body, dark, dark, dark, body};
    struct Part { float x0, y0, z0, x1, y1, z1; };
    const Part parts[] = {
        {-4.0f, 12.0f, -2.0f, 4.0f, 24.0f, 2.0f},   // torso
        {-4.0f, 24.0f, -4.0f, 4.0f, 32.0f, 4.0f},   // head
        {-8.0f, 12.0f, -2.0f, -4.0f, 24.0f, 2.0f},  // right arm
        {4.0f, 12.0f, -2.0f, 8.0f, 24.0f, 2.0f},    // left arm
        {-3.9f, 0.0f, -2.0f, 0.1f, 12.0f, 2.0f},    // right leg
        {-0.1f, 0.0f, -2.0f, 3.9f, 12.0f, 2.0f},    // left leg
    };
    for (const Part& p : parts) {
        const wings::WingBox box = boxFromRange(p.x0, p.y0, p.z0, p.x1, p.y1, p.z1);
        drawPrism(fb, scene, box, identity, colors, 0.8f);
    }
}

void drawWings(FrameBuffer& fb, const Scene& scene, int styleIndex, const float anglesDeg[7]) {
    if (styleIndex < 0 || styleIndex >= wings::kWingStyleCount) styleIndex = 0;
    const wings::WingStyle& style = wings::kWingStyles[styleIndex];

    wings::WingBone left[wings::kMaxWingBones];
    wings::mirrorWingBones(style.rightBones, style.boneCount, left);

    const float sides[2] = {-1.0f, 1.0f};
    const float roots[2] = {wings::kRightRootPivotX, wings::kLeftRootPivotX};
    for (int side = 0; side < 2; ++side) {
        const wings::WingBone* bones = (side == 0) ? style.rightBones : left;
        wings::Pose2D poses[wings::kMaxWingBones];
        for (int i = 0; i < style.boneCount; ++i) {
            const wings::WingBone& bone = bones[i];
            const float rest = scene.legacy ? 0.0f : bone.restDeg;
            const int ai = (bone.angleIndex >= 0 && bone.angleIndex < 7) ? bone.angleIndex : 0;
            const float angleRad = sides[side] * (anglesDeg[ai] + rest) * wings::kDegToRad;
            poses[i] = (bone.parent < 0 || bone.parent >= i)
                           ? wings::makePose(angleRad, roots[side], wings::kRootPivotY)
                           : wings::composePose(poses[bone.parent], bone.anchorX, bone.anchorY, angleRad);

            wings::WingBone plain = bone;
            if (scene.legacy) {
                plain.taperPx = 0.0f;
                plain.sweepPx = 0.0f;
            }
            const wings::WingBox box = wings::buildWingBox(plain, poses[i]);
            const unsigned char* colors[6] = {
                bone.colInner, bone.colOuter, bone.colEdge, bone.colEdge, bone.colBottom, bone.colEdge,
            };
            drawPrism(fb, scene, box, poses[i], colors,
                      scene.legacy ? 1.0f : wings::spanTint(bone.spanT, bone.tint));
        }
    }
}

// ---------------------------------------------------------------------------
// Poses: [shoulder, upper, tip, feather1..4] in the module's raise-positive
// degrees, sampled from the idle / flap / glide envelopes.
// ---------------------------------------------------------------------------

struct PosePreset {
    const char* name;
    float angles[7];
};

constexpr PosePreset kPoses[] = {
    {"idle", {26.0f, 8.0f, 9.0f, 2.0f, 1.0f, -1.0f, -2.0f}},
    {"flap-up", {58.0f, 12.0f, 16.0f, 9.0f, 7.0f, 5.0f, 3.0f}},
    {"flap-down", {-9.0f, -12.0f, -16.0f, -9.0f, -7.0f, -5.0f, -3.0f}},
    {"glide", {50.0f, -15.0f, -15.0f, 4.0f, 4.0f, 4.0f, 4.0f}},
};
constexpr int kPoseCount = static_cast<int>(sizeof(kPoses) / sizeof(kPoses[0]));

Scene makeScene(float orbitDeg, float height, float distance, int panelW, int panelH) {
    // Player at the origin facing +z (yaw 0): right = (-1, 0, 0), forward = (0, 0, 1).
    Scene scene;
    scene.right = wings::Vec3{-1.0f, 0.0f, 0.0f};
    scene.forward = wings::Vec3{0.0f, 0.0f, 1.0f};
    scene.feet = wings::Vec3{0.0f, 0.0f, 0.0f};
    scene.centerX = panelW / 2;
    scene.centerY = panelH / 2;

    const wings::Vec3 center{0.0f, 1.25f, 0.0f};
    const float orbit = orbitDeg * wings::kDegToRad;
    // Behind the player is -forward; orbit swings the camera around the player.
    const wings::Vec3 dir{
        -scene.forward.x * std::cos(orbit) + scene.right.x * std::sin(orbit),
        0.0f,
        -scene.forward.z * std::cos(orbit) + scene.right.z * std::sin(orbit)};
    const wings::Vec3 eye = center + dir * distance + wings::Vec3{0.0f, height, 0.0f};
    scene.cam = lookAt(eye, center, static_cast<float>(panelH) * 0.62f);
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

void renderStyle(const std::string& outDir, int styleIndex, bool legacy) {
    constexpr int kPanelW = 300;
    constexpr int kPanelH = 300;
    FrameBuffer fb;
    fb.resize(kPanelW * 2, kPanelH * 2);
    fb.clear(28, 32, 40);

    for (int i = 0; i < kPoseCount; ++i) {
        FrameBuffer panel;
        panel.resize(kPanelW, kPanelH);
        panel.clear(28, 32, 40);
        Scene scene = makeScene(32.0f, 0.35f, 3.1f, kPanelW, kPanelH);
        scene.legacy = legacy;
        drawPlayer(panel, scene);
        drawWings(panel, scene, styleIndex, kPoses[i].angles);
        pastePanel(fb, panel, (i % 2) * kPanelW, (i / 2) * kPanelH);
    }

    const std::string name = std::string(legacy ? "legacy_" : "") + wings::kWingStyles[styleIndex].id + ".png";
    writePng(outDir + "/" + name, fb);
}

void renderCompare(const std::string& outDir) {
    constexpr int kPanelW = 320;
    constexpr int kPanelH = 320;
    FrameBuffer fb;
    fb.resize(kPanelW * 2, kPanelH * 2);
    fb.clear(28, 32, 40);

    // Columns: legacy | current. Rows: flap-up | flap-down.
    const int rows[2] = {1, 2};
    for (int col = 0; col < 2; ++col) {
        for (int row = 0; row < 2; ++row) {
            FrameBuffer panel;
            panel.resize(kPanelW, kPanelH);
            panel.clear(28, 32, 40);
            Scene scene = makeScene(32.0f, 0.35f, 3.1f, kPanelW, kPanelH);
            scene.legacy = (col == 0);
            drawPlayer(panel, scene);
            drawWings(panel, scene, 0, kPoses[rows[row]].angles);
            pastePanel(fb, panel, col * kPanelW, row * kPanelH);
        }
    }
    writePng(outDir + "/compare_dragon.png", fb);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string outDir = (argc > 1) ? argv[1] : "build/wings-preview";
    std::printf("wings preview -> %s\n", outDir.c_str());
    for (int s = 0; s < wings::kWingStyleCount; ++s) {
        renderStyle(outDir, s, false);
    }
    renderStyle(outDir, 0, true);
    renderCompare(outDir);
    return 0;
}

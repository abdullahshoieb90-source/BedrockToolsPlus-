// Offline preview renderer for the Wings module.
//
// Draws the exact geometry the game draws (modules/visual/wings_design.hpp +
// wings_surface.hpp, i.e. the same membrane, struts, spar, claw and feather
// layer the overlay submits) into PNG files, so a wing style can be judged
// without launching Minecraft. It is also the visual regression check for the
// overlay geometry.
//
// Build and run (no game, no NDK):
//     ./scripts/gen_wings_preview.sh
// or manually:
//     g++ -std=c++20 -O2 -w -I src -I include -I third_party
//         tools/wings_preview.cpp -o build/wings_preview
//     ./build/wings_preview build/wings-preview
//
// Output:
//     <outdir>/<style>.png      2x2 pose sheet (idle, flap-up, flap-down, glide)
//     <outdir>/<style>_hero.png 3/4 close-up at the idle pose
//     <outdir>/<style>_sides.png behind vs. in front (both palette sides)
//     <outdir>/showcase.png     every style at the idle pose
//     <outdir>/front_gate.png   orbit sweep of the front-view gate
//     <outdir>/front_before_after.png  front view: wings over the body vs. hidden

#include "modules/visual/wings_design.hpp"
#include "modules/visual/wings_shape.hpp"
#include "modules/visual/wings_surface.hpp"

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

    // Overwrite regardless of depth: reproduces a fill material without a depth
    // test (ui_fill_color on current Bedrock builds).
    // Alpha blend against whatever is already there (the overlay submits a
    // vertex alpha for the front-view fade).
    void blend(int x, int y, float z, wings::FaceColor c, float a) {
        if (a >= 0.999f) { set(x, y, z, c); return; }
        if (a <= 0.001f) return;
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        const std::size_t i = static_cast<std::size_t>(y) * w + x;
        if (!(z < depth[i])) return;
        for (int k = 0; k < 3; ++k) {
            const float src = (k == 0 ? c.r : k == 1 ? c.g : c.b) * a;
            const float dst = rgb[i * 3 + k] * (1.0f - a);
            rgb[i * 3 + k] = static_cast<unsigned char>(std::lround(src + dst));
        }
    }

    void setOver(int x, int y, wings::FaceColor c) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        const std::size_t i = static_cast<std::size_t>(y) * w + x;
        rgb[i * 3 + 0] = c.r;
        rgb[i * 3 + 1] = c.g;
        rgb[i * 3 + 2] = c.b;
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

// ---------------------------------------------------------------------------
// Rasterizer
// ---------------------------------------------------------------------------

// Gouraud triangle: each corner carries a colour and the pixels are
// barycentric-blended between them, matching what the tessellator does in game.
void fillTriangle(FrameBuffer& fb, const ScreenPoint& a, const ScreenPoint& b, const ScreenPoint& c,
                  const wings::FaceColor& ca, const wings::FaceColor& cb, const wings::FaceColor& cc,
                  bool ignoreDepth = false, float alpha = 1.0f) {
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
        if (y < 0 || y >= fb.h) continue;
        for (int x = x0; x <= x1; ++x) {
            if (x < 0 || x >= fb.w) continue;
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float w0 = ((b.y - c.y) * (px - c.x) + (c.x - b.x) * (py - c.y)) / denom;
            const float w1 = ((c.y - a.y) * (px - c.x) + (a.x - c.x) * (py - c.y)) / denom;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            const float z = w0 * a.depth + w1 * b.depth + w2 * c.depth;
            const wings::FaceColor blended{
                static_cast<unsigned char>(w0 * ca.r + w1 * cb.r + w2 * cc.r + 0.5f),
                static_cast<unsigned char>(w0 * ca.g + w1 * cb.g + w2 * cc.g + 0.5f),
                static_cast<unsigned char>(w0 * ca.b + w1 * cb.b + w2 * cc.b + 0.5f)};
            if (ignoreDepth) fb.setOver(x, y, blended);
            else fb.blend(x, y, z, blended, alpha);
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
    bool cullBehindBody = true;  // the geometry cull the module applies
    bool frontGate = true;       // skip the wings entirely unless the camera is behind
};

// Shades and rasterizes one double-sided quad exactly like the overlay does:
// pick the palette side from which side of the player the camera is on (not
// from an interpolated normal, which flips across a curved sheet and turns it
// into a front/back patchwork), light it, and fill it with per-corner colours so
// the tessellator/preview interpolates a gradient across the cell.
void drawQuad(FrameBuffer& fb, const Scene& scene, const wings::WingQuad& q, float extraTint = 1.0f,
              bool ignoreDepth = false, float alpha = 1.0f) {
    wings::Vec3 world[4];
    for (int i = 0; i < 4; ++i) {
        world[i] = wings::modelPointToWorld(q.p[i].x, q.p[i].y, q.p[i].z, scene.feet, scene.right,
                                            scene.forward);
    }

    const wings::Vec3 center = (world[0] + world[1] + world[2] + world[3]) * 0.25f;
    const wings::Vec3 toCamera = scene.cam.pos - center;

    // model +z is the player's back; a camera behind the player sees `back`.
    const wings::Vec3 backDir = wings::modelDirToWorld({0.0f, 0.0f, 1.0f}, scene.right, scene.forward);
    const wings::Vec3 bodyCenter = wings::modelPointToWorld(0.0f, 20.0f, 0.0f, scene.feet,
                                                            scene.right, scene.forward);
    const bool facingBack = wings::dot(backDir, scene.cam.pos - bodyCenter) > 0.0f;
    const wings::Vec3 normal = wings::modelDirToWorld(q.n, scene.right, scene.forward);
    const wings::Vec3 litNormal = facingBack ? normal : normal * -1.0f;
    const wings::FaceColor* cornerColors = facingBack ? q.backC : q.frontC;

    float brightness = wings::faceBrightness(litNormal, toCamera, wings::kDefaultLight) *
                       wings::spanTint(q.spanT) * extraTint;
    if (q.emissive > 0.0f && brightness < q.emissive) brightness = q.emissive;

    ScreenPoint sp[4];
    for (int i = 0; i < 4; ++i) sp[i] = project(scene.cam, world[i], scene.centerX, scene.centerY);
    for (const ScreenPoint& p : sp) {
        if (!p.visible) return;
    }

    wings::FaceColor shaded[4];
    for (int k = 0; k < 4; ++k) shaded[k] = wings::shadeFace(cornerColors[k], brightness);
    fillTriangle(fb, sp[0], sp[1], sp[2], shaded[0], shaded[1], shaded[2], ignoreDepth, alpha);
    fillTriangle(fb, sp[0], sp[2], sp[3], shaded[0], shaded[2], shaded[3], ignoreDepth, alpha);
}

// Axis-aligned box -> quads, so the player mannequin goes through the same
// shading path as the wings.
void drawBox(FrameBuffer& fb, const Scene& scene, float x0, float y0, float z0,
             float x1, float y1, float z1, const wings::FaceColor& top,
             const wings::FaceColor& side, const wings::FaceColor& backFace) {
    const wings::Vec3 c[8] = {
        {x0, y0, z0}, {x1, y0, z0}, {x0, y1, z0}, {x1, y1, z0},
        {x0, y0, z1}, {x1, y0, z1}, {x0, y1, z1}, {x1, y1, z1},
    };
    const int faces[6][4] = {
        {0, 1, 3, 2}, {4, 5, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}, {0, 1, 5, 4}, {2, 3, 7, 6},
    };
    for (int f = 0; f < 6; ++f) {
        wings::WingQuad q{};
        for (int i = 0; i < 4; ++i) q.p[i] = c[faces[f][i]];
        q.n = wings::normalized(wings::cross(q.p[1] - q.p[0], q.p[3] - q.p[0]));
        if (q.n.z < 0.0f) q.n = q.n * -1.0f;
        const wings::FaceColor& col = (f == 5) ? top : (f == 1 ? backFace : side);
        q.front = col;
        q.back = col;
        for (int i = 0; i < 4; ++i) {
            q.frontC[i] = col;
            q.backC[i] = col;
        }
        q.spanT = 0.5f;
        q.emissive = 0.0f;
        drawQuad(fb, scene, q, 0.9f);
    }
}

void drawPlayer(FrameBuffer& fb, const Scene& scene) {
    const wings::FaceColor skin{132, 142, 158};
    const wings::FaceColor shade{96, 106, 122};
    const wings::FaceColor dark{78, 86, 100};
    drawBox(fb, scene, -4.0f, 12.0f, -2.0f, 4.0f, 24.0f, 2.0f, skin, shade, dark);    // torso
    drawBox(fb, scene, -4.0f, 24.0f, -4.0f, 4.0f, 32.0f, 4.0f, skin, shade, dark);    // head
    drawBox(fb, scene, -8.0f, 12.0f, -2.0f, -4.0f, 24.0f, 2.0f, skin, shade, dark);   // right arm
    drawBox(fb, scene, 4.0f, 12.0f, -2.0f, 8.0f, 24.0f, 2.0f, skin, shade, dark);     // left arm
    drawBox(fb, scene, -3.9f, 0.0f, -2.0f, 0.1f, 12.0f, 2.0f, shade, dark, dark);     // right leg
    drawBox(fb, scene, -0.1f, 0.0f, -2.0f, 3.9f, 12.0f, 2.0f, shade, dark, dark);     // left leg
}

// Pose presets: [shoulder, elbow, wrist] plus the flutter amount, sampled from
// the module's idle / flap / glide envelopes.
struct PosePreset {
    const char* name;
    float shoulderDeg;
    float elbowDeg;
    float wristDeg;
    float phaseRad;
    float flutter;
};

constexpr PosePreset kPoses[] = {
    {"idle", 26.0f, 8.0f, 9.0f, 0.4f, 0.35f},
    {"flap-up", 58.0f, 12.0f, 16.0f, 1.2f, 0.85f},
    {"flap-down", -9.0f, -12.0f, -16.0f, 3.4f, 0.85f},
    {"glide", 50.0f, -15.0f, -15.0f, 0.8f, 0.20f},
};
constexpr int kPoseCount = static_cast<int>(sizeof(kPoses) / sizeof(kPoses[0]));

wings::WingPose makePose(const PosePreset& preset) {
    wings::WingPose pose;
    pose.shoulderDeg = preset.shoulderDeg;
    pose.elbowDeg = preset.elbowDeg;
    pose.wristDeg = preset.wristDeg;
    pose.phaseRad = preset.phaseRad;
    pose.flutter = preset.flutter;
    return pose;
}

void drawWings(FrameBuffer& fb, const Scene& scene, int styleIndex, const wings::WingPose& pose) {
    if (styleIndex < 0 || styleIndex >= wings::kWingStyleCount) styleIndex = 0;
    const wings::WingDesign& design = wings::kWingDesigns[styleIndex];

    // The module's front-view gate, applied before anything is drawn.
    const wings::Vec3 gateCenter = scene.feet + wings::Vec3{0.0f, 1.25f, 0.0f};
    const float backness = wings::cameraBackness(scene.cam.pos, gateCenter, scene.forward);
    if (scene.frontGate && !(backness > 0.0f)) return;
    const float wingAlpha = scene.frontGate ? wings::wingFadeAlpha(backness) : 1.0f;

    // Occluder the module uses: collision width, model depth.
    wings::Vec3 bodyMin, bodyMax;
    wings::bodyOccluderFromAabb({scene.feet.x - 0.3f, scene.feet.y, scene.feet.z - 0.3f},
                                {scene.feet.x + 0.3f, scene.feet.y + 1.8f, scene.feet.z + 0.3f},
                                bodyMin, bodyMax);

    static wings::WingQuad quads[wings::kMaxWingQuads];
    const int count = wings::buildWing(design, pose, -1, quads, wings::kMaxWingQuads);
    for (int i = 0; i < count; ++i) {
        if (scene.cullBehindBody &&
            wings::quadHiddenByBody(quads[i], bodyMin, bodyMax, scene.feet, scene.right,
                                    scene.forward, scene.cam.pos)) {
            continue;
        }
        drawQuad(fb, scene, quads[i], 1.0f, false, wingAlpha);
    }
    const int countLeft = wings::buildWing(design, pose, 1, quads, wings::kMaxWingQuads);
    for (int i = 0; i < countLeft; ++i) {
        if (scene.cullBehindBody &&
            wings::quadHiddenByBody(quads[i], bodyMin, bodyMax, scene.feet, scene.right,
                                    scene.forward, scene.cam.pos)) {
            continue;
        }
        drawQuad(fb, scene, quads[i], 1.0f, false, wingAlpha);
    }
}

Scene makeScene(float orbitDeg, float height, float distance, float focusY, int panelW, int panelH) {
    // Player at the origin facing +z (yaw 0): right = (-1, 0, 0), forward = (0, 0, 1).
    Scene scene;
    scene.right = wings::Vec3{-1.0f, 0.0f, 0.0f};
    scene.forward = wings::Vec3{0.0f, 0.0f, 1.0f};
    scene.feet = wings::Vec3{0.0f, 0.0f, 0.0f};
    scene.centerX = panelW / 2;
    scene.centerY = panelH / 2;

    const wings::Vec3 center{0.0f, focusY, 0.0f};
    const float orbit = orbitDeg * wings::kDegToRad;
    // Behind the player is -forward; orbit swings the camera around the player.
    const wings::Vec3 dir{
        -scene.forward.x * std::cos(orbit) + scene.right.x * std::sin(orbit),
        0.0f,
        -scene.forward.z * std::cos(orbit) + scene.right.z * std::sin(orbit)};
    const wings::Vec3 eye = center + dir * distance + wings::Vec3{0.0f, height, 0.0f};
    scene.cam = lookAt(eye, center, static_cast<float>(panelH) * 0.70f);
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

void renderStyle(const std::string& outDir, int styleIndex) {
    constexpr int kPanelW = 320;
    constexpr int kPanelH = 320;
    FrameBuffer fb;
    fb.resize(kPanelW * 2, kPanelH * 2);
    fb.clear(30, 34, 44);

    for (int i = 0; i < kPoseCount; ++i) {
        FrameBuffer panel;
        panel.resize(kPanelW, kPanelH);
        panel.clear(30, 34, 44);
        Scene scene = makeScene(32.0f, 0.35f, 3.5f, 1.30f, kPanelW, kPanelH);
        drawPlayer(panel, scene);
        drawWings(panel, scene, styleIndex, makePose(kPoses[i]));
        pastePanel(fb, panel, (i % 2) * kPanelW, (i / 2) * kPanelH);
    }

    writePng(outDir + "/" + wings::kWingDesigns[styleIndex].id + ".png", fb);
}

void renderHero(const std::string& outDir, int styleIndex) {
    constexpr int kSize = 720;
    FrameBuffer fb;
    fb.resize(kSize, kSize);
    fb.clear(30, 34, 44);
    Scene scene = makeScene(38.0f, 0.45f, 3.1f, 1.42f, kSize, kSize);
    drawPlayer(fb, scene);
    drawWings(fb, scene, styleIndex, makePose(kPoses[0]));
    writePng(outDir + "/" + wings::kWingDesigns[styleIndex].id + "_hero.png", fb);
}

// Behind vs. in front of the player: the overlay picks a different palette
// side depending on where the camera is, and the body-facing side is what you
// see while the camera swings around, so it is rendered too.
void renderSides(const std::string& outDir, int styleIndex) {
    constexpr int kPanelW = 380;
    constexpr int kPanelH = 380;
    FrameBuffer fb;
    fb.resize(kPanelW * 2, kPanelH);
    fb.clear(30, 34, 44);

    const float orbits[2] = {36.0f, 180.0f - 36.0f};
    for (int i = 0; i < 2; ++i) {
        FrameBuffer panel;
        panel.resize(kPanelW, kPanelH);
        panel.clear(30, 34, 44);
        Scene scene = makeScene(orbits[i], 0.45f, 3.3f, 1.38f, kPanelW, kPanelH);
        drawPlayer(panel, scene);
        drawWings(panel, scene, styleIndex, makePose(kPoses[0]));
        pastePanel(fb, panel, i * kPanelW, 0);
    }
    writePng(outDir + "/" + wings::kWingDesigns[styleIndex].id + "_sides.png", fb);
}

// Orbit sweep of the front-view gate: the camera walks from directly behind the
// player round to directly in front. The wings must be fully opaque through the
// normal third-person range, fade out as the camera passes side-on, and be gone
// entirely in front.
//
//   0 deg   45 deg   70 deg   90 deg   135 deg   180 deg
//   behind  behind   fading   side-on  front     dead front
void renderFrontGate(const std::string& outDir) {
    constexpr int kPanelW = 300;
    constexpr int kPanelH = 340;
    constexpr int kPanels = 6;
    const float orbits[kPanels] = {0.0f, 45.0f, 70.0f, 90.0f, 135.0f, 180.0f};

    FrameBuffer fb;
    fb.resize(kPanelW * kPanels, kPanelH);
    fb.clear(30, 34, 44);

    for (int i = 0; i < kPanels; ++i) {
        FrameBuffer panel;
        panel.resize(kPanelW, kPanelH);
        panel.clear(30, 34, 44);

        Scene scene = makeScene(orbits[i], 0.30f, 4.4f, 1.30f, kPanelW, kPanelH);
        drawPlayer(panel, scene);
        drawWings(panel, scene, 1 /* Angel */, makePose(kPoses[0]));
        pastePanel(fb, panel, i * kPanelW, 0);
    }
    writePng(outDir + "/front_gate.png", fb);
}

// The defect vs. the fix, same front camera both times: without the gate the
// sails are painted straight over the player (the old behaviour), with the gate
// the overlay is simply not drawn.
void renderFrontBeforeAfter(const std::string& outDir) {
    constexpr int kPanelW = 520;
    constexpr int kPanelH = 520;

    FrameBuffer fb;
    fb.resize(kPanelW * 2, kPanelH);
    fb.clear(30, 34, 44);

    for (int i = 0; i < 2; ++i) {
        FrameBuffer panel;
        panel.resize(kPanelW, kPanelH);
        panel.clear(30, 34, 44);

        Scene scene = makeScene(180.0f, 0.30f, 4.4f, 1.30f, kPanelW, kPanelH);
        scene.frontGate = (i == 1);  // 0 = old behaviour, 1 = fixed
        drawPlayer(panel, scene);
        drawWings(panel, scene, 1 /* Angel */, makePose(kPoses[0]));
        pastePanel(fb, panel, i * kPanelW, 0);
    }
    writePng(outDir + "/front_before_after.png", fb);
}

void renderShowcase(const std::string& outDir) {
    constexpr int kPanelW = 300;
    constexpr int kPanelH = 300;
    const int cols = 5;
    const int rows = (wings::kWingStyleCount + cols - 1) / cols;
    FrameBuffer fb;
    fb.resize(kPanelW * cols, kPanelH * rows);
    fb.clear(30, 34, 44);

    for (int s = 0; s < wings::kWingStyleCount; ++s) {
        FrameBuffer panel;
        panel.resize(kPanelW, kPanelH);
        panel.clear(30, 34, 44);
        Scene scene = makeScene(32.0f, 0.35f, 3.5f, 1.30f, kPanelW, kPanelH);
        drawPlayer(panel, scene);
        drawWings(panel, scene, s, makePose(kPoses[0]));
        pastePanel(fb, panel, (s % cols) * kPanelW, (s / cols) * kPanelH);
    }
    writePng(outDir + "/showcase.png", fb);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string outDir = (argc > 1) ? argv[1] : "build/wings-preview";
    std::printf("wings preview -> %s\n", outDir.c_str());
    for (int s = 0; s < wings::kWingStyleCount; ++s) {
        renderStyle(outDir, s);
        renderHero(outDir, s);
        renderSides(outDir, s);
    }
    renderShowcase(outDir);
    renderFrontGate(outDir);
    renderFrontBeforeAfter(outDir);
    return 0;
}

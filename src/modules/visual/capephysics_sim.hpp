#pragma once

// Pure helpers for the Cape Physics module.
//
// Everything in this header is plain C++ with no Minecraft, launcher or
// mod-menu dependencies so it can be unit-tested on the host (see
// tests/capephysics_sim_test.cpp) and reused by the offline preview tool
// (tools/capephysics_preview.cpp). It covers the two "dumb data" halves of
// the module:
//
//   * turning ANY cape canvas (22x23, 64x32, 128x64 HD, 704x736, ...) into
//     the per-cell colors the cloth is painted with — every region of the
//     classic cape layout is addressed proportionally, so any source size
//     resolves to the same 10x16 face the game renders, and each cloth cell
//     box-filters exactly the texels it covers (the native 10x16 grid is
//     therefore pixel-identical to the cape texture);
//   * the cape cloth itself: a Verlet particle sheet pinned along the
//     shoulder line, pushed by gravity, a deterministic wind field and the
//     relative wind of the player's own movement, held together by
//     structural + shear distance constraints and kept out of the player's
//     body by an elliptic cylinder collision.
//
// The module (capephysics.cpp) only contributes the plumbing: player
// tracking, the live cape pixel source and the tessellator rendering.

#include "modules/player/customcapes_files.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace bedrocktools::modules::capephysics {

// ---------------------------------------------------------------------------
// Small vector math (kept local so the header stays dependency-free, exactly
// like wings_shape.hpp does).
// ---------------------------------------------------------------------------

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3& operator+=(Vec3& a, const Vec3& b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
inline Vec3& operator-=(Vec3& a, const Vec3& b) { a.x -= b.x; a.y -= b.y; a.z -= b.z; return a; }

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float lengthSquared(const Vec3& a) { return dot(a, a); }
inline float length(const Vec3& a) { return std::sqrt(lengthSquared(a)); }

inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline Vec3 normalized(const Vec3& a) {
    const float len = length(a);
    if (!(len > 0.000001f)) return {0.0f, 0.0f, 0.0f};
    return a * (1.0f / len);
}

inline bool isFinite(const Vec3& a) {
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}

// ---------------------------------------------------------------------------
// Cape dimensions (Bedrock model space, 16 px = 1 block — same conventions as
// wings_shape.hpp: +x the player's left, +y up, +z backwards/cape side).
//
// The classic cape cuboid is 10x16x1 px hung from the shoulder line (model
// y = 24) down to y = 8, its front face resting on the torso's back plane
// (model z = 2). The cloth replaces that rigid cuboid with a sheet of the
// same size, so at rest it is indistinguishable from the vanilla cape.
// ---------------------------------------------------------------------------

inline constexpr float kPxToBlocks = 1.0f / 16.0f;
inline constexpr float kCapeWidthBlocks = 10.0f * kPxToBlocks;   // 0.625
inline constexpr float kCapeHeightBlocks = 16.0f * kPxToBlocks;  // 1.0
inline constexpr float kCapeThicknessBlocks = 1.0f * kPxToBlocks;
inline constexpr float kCapeTopModelY = 24.0f;  // px, shoulder/neck line
inline constexpr float kCapeBackModelZ = 2.5f;  // px, mid-plane of the cape box

// ---------------------------------------------------------------------------
// Cloth detail presets. "Native" is one cloth cell per cape pixel (10x16 =
// exactly the visible face), which is both the cheapest and the
// pixel-faithful grid; "Fine" subdivides further for softer folds.
// ---------------------------------------------------------------------------

struct DetailPreset {
    int cols;
    int rows;
    const char* id;
    const char* label;
};

inline constexpr DetailPreset kDetailPresets[] = {
    {10, 16, "native", "Native"},
    {14, 22, "fine", "Fine"},
};
inline constexpr int kDetailCount = static_cast<int>(sizeof(kDetailPresets) / sizeof(kDetailPresets[0]));

inline int clampDetailIndex(int index) {
    return (index < 0 || index >= kDetailCount) ? 0 : index;
}

inline int detailIndexFromId(const std::string& id) {
    for (int i = 0; i < kDetailCount; ++i) {
        if (id == kDetailPresets[i].id) return i;
    }
    return -1;
}

inline int detailIndexFromLabel(const std::string& label) {
    for (int i = 0; i < kDetailCount; ++i) {
        if (label.size() != std::strlen(kDetailPresets[i].label)) continue;
        bool same = true;
        for (std::size_t c = 0; c < label.size(); ++c) {
            if (std::tolower(static_cast<unsigned char>(label[c])) !=
                std::tolower(static_cast<unsigned char>(kDetailPresets[i].label[c]))) {
                same = false;
                break;
            }
        }
        if (same) return i;
    }
    return -1;
}

inline std::vector<std::string> detailLabelList() {
    std::vector<std::string> labels;
    labels.reserve(static_cast<std::size_t>(kDetailCount));
    for (int i = 0; i < kDetailCount; ++i) labels.push_back(kDetailPresets[i].label);
    return labels;
}

// Radio value in the launcher's format ("<index>,<label>,<label>,...").
inline std::string detailRadioValue(int index) {
    return customcapes::makeLabelRadioValue(clampDetailIndex(index), detailLabelList());
}

// ---------------------------------------------------------------------------
// Canvas sampling — the "any size" half.
//
// The live cape image in the skin and the PNG files in the capes folder come
// in every size (22x23, 64x32, 128x64, 704x736, ...). The classic-cape face
// regions are fractions of the canvas, so sampling in normalized (u, v)
// space keeps the mapping identical for every size, and a box filter over
// each cloth cell averages exactly the texels that cell covers: a 704x736
// source collapses to the same 10x16 cell colors a 64x32 source produces,
// while the native grid stays pixel-identical to a 64x32 cape texture.
// ---------------------------------------------------------------------------

inline constexpr std::uint32_t kCanvasWidth = 64;
inline constexpr std::uint32_t kCanvasHeight = 32;
inline constexpr std::size_t kCanvasBytes = static_cast<std::size_t>(kCanvasWidth) * kCanvasHeight * 4u;

// Box-filters the RGBA rectangle [u0,u1)x[v0,v1) (normalized coordinates) of
// a canvas into outRgb. Every texel contributes exactly by its covered area
// fraction, so sub-pixel rectangles resolve to the single texel they sit on
// (the native cloth grid is therefore pixel-identical to the cape texture)
// and larger rectangles are true area averages. Degenerate rectangles fall
// back to the nearest texel at their center.
inline void sampleCanvasRect(const std::uint8_t* canvas, std::uint32_t w, std::uint32_t h,
                             float u0, float v0, float u1, float v1, std::uint8_t outRgb[3]) {
    outRgb[0] = outRgb[1] = outRgb[2] = 0;
    if (!canvas || w == 0 || h == 0) return;

    if (u1 < u0) std::swap(u0, u1);
    if (v1 < v0) std::swap(v0, v1);

    const float x0f = u0 * static_cast<float>(w);
    const float x1f = u1 * static_cast<float>(w);
    const float y0f = v0 * static_cast<float>(h);
    const float y1f = v1 * static_cast<float>(h);

    float acc[3] = {0.0f, 0.0f, 0.0f};
    float total = 0.0f;
    const int y0 = static_cast<int>(std::floor(y0f));
    const int y1 = static_cast<int>(std::ceil(y1f));
    for (int y = y0; y < y1; ++y) {
        const float ay = std::max(0.0f, std::min(static_cast<float>(y + 1), y1f) - std::max(static_cast<float>(y), y0f));
        if (ay <= 0.0f) continue;
        const int cy = std::clamp(y, 0, static_cast<int>(h) - 1);
        const int x0 = static_cast<int>(std::floor(x0f));
        const int x1 = static_cast<int>(std::ceil(x1f));
        for (int x = x0; x < x1; ++x) {
            const float ax = std::max(0.0f, std::min(static_cast<float>(x + 1), x1f) - std::max(static_cast<float>(x), x0f));
            if (ax <= 0.0f) continue;
            const int cx = std::clamp(x, 0, static_cast<int>(w) - 1);
            const std::uint8_t* px =
                canvas + (static_cast<std::size_t>(cy) * w + static_cast<std::size_t>(cx)) * 4u;
            const float a = ax * ay;
            acc[0] += px[0] * a;
            acc[1] += px[1] * a;
            acc[2] += px[2] * a;
            total += a;
        }
    }
    if (total <= 0.0f) {
        // Degenerate (zero-area) rectangle: nearest texel at the center.
        const int cx = std::clamp(static_cast<int>((u0 + u1) * 0.5f * static_cast<float>(w)), 0, static_cast<int>(w) - 1);
        const int cy = std::clamp(static_cast<int>((v0 + v1) * 0.5f * static_cast<float>(h)), 0, static_cast<int>(h) - 1);
        const std::uint8_t* px = canvas + (static_cast<std::size_t>(cy) * w + static_cast<std::size_t>(cx)) * 4u;
        outRgb[0] = px[0];
        outRgb[1] = px[1];
        outRgb[2] = px[2];
        return;
    }
    for (int c = 0; c < 3; ++c) {
        outRgb[c] = static_cast<std::uint8_t>(std::clamp(acc[c] / total + 0.5f, 0.0f, 255.0f));
    }
}

// Resamples an arbitrary RGBA cape canvas onto the classic 64x32 canvas the
// palette sampler expects, preserving the proportional layout. A 64x32 input
// is copied verbatim.
inline void normalizeCapeCanvas(const std::uint8_t* src, std::uint32_t srcW, std::uint32_t srcH,
                                std::vector<std::uint8_t>& out) {
    out.assign(kCanvasBytes, 0);
    if (!src || srcW == 0 || srcH == 0) return;
    if (srcW == kCanvasWidth && srcH == kCanvasHeight) {
        std::memcpy(out.data(), src, kCanvasBytes);
        return;
    }
    for (std::uint32_t y = 0; y < kCanvasHeight; ++y) {
        for (std::uint32_t x = 0; x < kCanvasWidth; ++x) {
            std::uint8_t rgb[3];
            sampleCanvasRect(src, srcW, srcH,
                             static_cast<float>(x) / kCanvasWidth,
                             static_cast<float>(y) / kCanvasHeight,
                             static_cast<float>(x + 1) / kCanvasWidth,
                             static_cast<float>(y + 1) / kCanvasHeight, rgb);
            const std::size_t i = (static_cast<std::size_t>(y) * kCanvasWidth + x) * 4u;
            out[i + 0] = rgb[0];
            out[i + 1] = rgb[1];
            out[i + 2] = rgb[2];
            out[i + 3] = 255;
        }
    }
}

// Per-cell colors for one cloth grid: the outer (worn) face sampled from the
// cape's back region, the inner (lining) face from the front region, and the
// four 1-px edge strips of the cape cuboid for the cloth's side borders, so
// the sheet keeps the vanilla cape's visible thickness. Region coordinates
// mirror customcapes_files.hpp (classic box unwrap of the 64x32 canvas).
struct CapePalette {
    std::vector<std::uint8_t> outer;    // cols*rows*3
    std::vector<std::uint8_t> inner;    // cols*rows*3
    std::vector<std::uint8_t> edgeLeft;   // rows*3
    std::vector<std::uint8_t> edgeRight;  // rows*3
    std::vector<std::uint8_t> edgeTop;    // cols*3
    std::vector<std::uint8_t> edgeBottom; // cols*3

    bool empty() const { return outer.empty(); }
};

inline void buildCapePalette(const std::uint8_t* canvas, std::uint32_t w, std::uint32_t h,
                             int cols, int rows, CapePalette& out) {
    if (cols <= 0 || rows <= 0) {
        out = CapePalette{};
        return;
    }
    const float cw = static_cast<float>(w);
    const float ch = static_cast<float>(h);

    out.outer.assign(static_cast<std::size_t>(cols) * rows * 3u, 0);
    out.inner.assign(static_cast<std::size_t>(cols) * rows * 3u, 0);
    out.edgeLeft.assign(static_cast<std::size_t>(rows) * 3u, 0);
    out.edgeRight.assign(static_cast<std::size_t>(rows) * 3u, 0);
    out.edgeTop.assign(static_cast<std::size_t>(cols) * 3u, 0);
    out.edgeBottom.assign(static_cast<std::size_t>(cols) * 3u, 0);

    const float faceW = static_cast<float>(customcapes::kCapeBackWidth);
    const float faceH = static_cast<float>(customcapes::kCapeBackHeight);
    const float backX = static_cast<float>(customcapes::kCapeBackX);
    const float backY = static_cast<float>(customcapes::kCapeBackY);
    const float frontX = static_cast<float>(customcapes::kCapeFrontX);

    for (int j = 0; j < rows; ++j) {
        const float v0 = (backY + faceH * static_cast<float>(j) / rows) / ch;
        const float v1 = (backY + faceH * static_cast<float>(j + 1) / rows) / ch;
        for (int i = 0; i < cols; ++i) {
            const float u0 = (backX + faceW * static_cast<float>(i) / cols) / cw;
            const float u1 = (backX + faceW * static_cast<float>(i + 1) / cols) / cw;
            std::uint8_t rgb[3];
            const std::size_t cell = (static_cast<std::size_t>(j) * cols + i) * 3u;

            sampleCanvasRect(canvas, w, h, u0, v0, u1, v1, rgb);
            out.outer[cell + 0] = rgb[0];
            out.outer[cell + 1] = rgb[1];
            out.outer[cell + 2] = rgb[2];

            sampleCanvasRect(canvas, w, h,
                             (frontX + faceW * static_cast<float>(i) / cols) / cw, v0,
                             (frontX + faceW * static_cast<float>(i + 1) / cols) / cw, v1, rgb);
            out.inner[cell + 0] = rgb[0];
            out.inner[cell + 1] = rgb[1];
            out.inner[cell + 2] = rgb[2];
        }

        // Side edge strips (1 px wide, full cape height).
        const std::size_t er = static_cast<std::size_t>(j) * 3u;
        std::uint8_t rgb[3];
        sampleCanvasRect(canvas, w, h,
                         static_cast<float>(customcapes::kCapeSideRightX) / cw, v0,
                         (customcapes::kCapeSideRightX + 1.0f) / cw, v1, rgb);
        out.edgeLeft[er + 0] = rgb[0];
        out.edgeLeft[er + 1] = rgb[1];
        out.edgeLeft[er + 2] = rgb[2];
        sampleCanvasRect(canvas, w, h,
                         static_cast<float>(customcapes::kCapeSideLeftX) / cw, v0,
                         (customcapes::kCapeSideLeftX + 1.0f) / cw, v1, rgb);
        out.edgeRight[er + 0] = rgb[0];
        out.edgeRight[er + 1] = rgb[1];
        out.edgeRight[er + 2] = rgb[2];
    }

    for (int i = 0; i < cols; ++i) {
        const float u0 = (backX + faceW * static_cast<float>(i) / cols) / cw;
        const float u1 = (backX + faceW * static_cast<float>(i + 1) / cols) / cw;
        std::uint8_t rgb[3];
        const std::size_t ec = static_cast<std::size_t>(i) * 3u;

        sampleCanvasRect(canvas, w, h, u0, 0.0f, u1, 1.0f / ch, rgb);
        out.edgeTop[ec + 0] = rgb[0];
        out.edgeTop[ec + 1] = rgb[1];
        out.edgeTop[ec + 2] = rgb[2];

        sampleCanvasRect(canvas, w, h,
                         (customcapes::kCapeBottomX + faceW * static_cast<float>(i) / cols) / cw,
                         0.0f,
                         (customcapes::kCapeBottomX + faceW * static_cast<float>(i + 1) / cols) / cw,
                         1.0f / ch, rgb);
        out.edgeBottom[ec + 0] = rgb[0];
        out.edgeBottom[ec + 1] = rgb[1];
        out.edgeBottom[ec + 2] = rgb[2];
    }
}

// ---------------------------------------------------------------------------
// Shading (same cosmetic-overlay model as wings_shape.hpp: a soft headlight
// so the cape stays readable from every angle plus a sky lift).
// ---------------------------------------------------------------------------

struct ClothLight {
    float ambient;
    float head;
    float sky;
};

inline constexpr ClothLight kDefaultLight{0.60f, 0.26f, 0.18f};

inline float faceBrightness(const Vec3& normal, const Vec3& toCamera, const ClothLight& light) {
    const Vec3 n = normalized(normal);
    const Vec3 v = normalized(toCamera);
    const float headTerm = dot(n, v);
    const float skyTerm = n.y;
    return light.ambient
         + light.head * (headTerm > 0.0f ? headTerm : 0.0f)
         + light.sky * (skyTerm > 0.0f ? skyTerm : 0.0f);
}

inline std::uint8_t shadeChannel(float channel, float factor) {
    const float v = channel * factor;
    if (!(v > 0.0f)) return 0;
    if (v >= 255.0f) return 255;
    return static_cast<std::uint8_t>(v + 0.5f);
}

inline void shadeRgb(const std::uint8_t rgb[3], float factor, float outRgb[3]) {
    outRgb[0] = shadeChannel(static_cast<float>(rgb[0]), factor);
    outRgb[1] = shadeChannel(static_cast<float>(rgb[1]), factor);
    outRgb[2] = shadeChannel(static_cast<float>(rgb[2]), factor);
}

// ---------------------------------------------------------------------------
// The cloth simulation.
//
// A sheet of (cols+1) x (rows+1) Verlet particles. The top row is pinned to
// the shoulder line the caller supplies each step (world space, interpolated
// exactly like the game interpolates the player mesh); everything below is
// free and moves under:
//
//   gravity      kBaseGravity * gravityMul, straight down;
//   wind         a deterministic sum-of-sines field, scaled by windMul;
//   drag         a force toward the local air velocity, so the player's own
//                movement streams the cape backwards (running, falling,
//                elytra gliding) without any special cases.
//
// Distance constraints (horizontal + vertical structural edges, both shear
// diagonals at reduced stiffness) keep the sheet cape-shaped. An elliptic
// cylinder around the player's body pushes the cloth out of the torso and
// legs, and a final stretch clamp bounds every structural edge so no force
// spike can ever explode the sheet. A teleport of the anchor row (> 3
// blocks) re-hangs the cape instead of slingshotting it across the world.
// ---------------------------------------------------------------------------

inline constexpr float kBaseGravity = 10.0f;   // blocks/s^2 at gravityMul = 1
inline constexpr float kWindBase = 2.2f;       // blocks/s gust scale at windMul = 1
inline constexpr float kDrag = 2.6f;           // 1/s relative-wind drag
inline constexpr float kDamping = 0.985f;      // velocity kept per step
inline constexpr float kMaxSpeed = 24.0f;      // blocks/s particle speed clamp
inline constexpr float kMaxStretch = 1.55f;    // structural edge stretch clamp
inline constexpr int kConstraintIterations = 4;
inline constexpr float kTeleportDistance = 3.0f;  // blocks
inline constexpr float kRestLean = 0.18f;      // ~10 deg backward rest lean

// Elliptic collision cross-section around the body axis (blocks): covers the
// torso plus the arms sideways, and sits just behind the cape plane in z.
inline constexpr float kBodyRadiusX = 0.30f;
inline constexpr float kBodyRadiusZ = 0.16f;

struct ClothParams {
    float gravityMul = 1.0f;  // 0..2 (menu: Gravity)
    float windMul = 0.35f;    // 0..2 (menu: Wind)
    float stiffness = 0.85f;  // 0.05..1 (menu: Stiffness)

    void clamp() {
        gravityMul = std::clamp(gravityMul, 0.0f, 2.0f);
        windMul = std::clamp(windMul, 0.0f, 2.0f);
        stiffness = std::clamp(stiffness, 0.05f, 1.0f);
    }
};

// Player body the cape collides against. height is the AABB height (1.8 for
// a standing player, 1.5 sneaking, 0.6 swimming) in blocks.
struct BodyFrame {
    Vec3 feet{};
    float yawDeg = 0.0f;
    float height = 1.8f;
};

inline Vec3 windAt(float t, const Vec3& p, float windMul) {
    if (windMul <= 0.0f) return {0.0f, 0.0f, 0.0f};
    const float w = kWindBase * windMul;
    return {
        w * (0.7f * std::sin(t * 0.9f + p.y * 0.7f + p.x * 0.35f) +
             0.3f * std::sin(t * 2.3f + p.z * 0.9f)),
        w * 0.30f * std::sin(t * 1.7f + p.x * 0.5f + p.y * 0.8f),
        w * (0.7f * std::cos(t * 1.1f + p.x * 0.6f + p.y * 0.4f) +
             0.3f * std::cos(t * 2.7f + p.y * 0.7f)),
    };
}

class Cloth {
public:
    void configure(int cols, int rows) {
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;
        if (cols > 62) cols = 62; // m_anchor holds cols+1 entries
        if (rows > 64) rows = 64;
        m_cols = cols;
        m_rows = rows;
        const std::size_t count = static_cast<std::size_t>(cols + 1) * (rows + 1);
        m_pos.assign(count, Vec3{});
        m_prev.assign(count, Vec3{});
        m_restW = kCapeWidthBlocks / static_cast<float>(cols);
        m_restH = kCapeHeightBlocks / static_cast<float>(rows);
        m_restD = std::sqrt(m_restW * m_restW + m_restH * m_restH);
        m_hasLastAnchors = false;
        m_resetPending = true;
    }

    // Re-hangs the cape from the given shoulder line: every particle rests
    // straight below its anchor with a slight backward lean (the vanilla
    // cape's resting tilt), zero velocity.
    void reset(const Vec3 anchors[], const BodyFrame& body) {
        if (!anchors) {
            m_resetPending = true;
            return;
        }
        const float yawRad = body.yawDeg * 3.14159265358979323846f / 180.0f;
        // "backwards" in world space, matching the module's model->world
        // mapping (model +z is the cape side): forward = (-sin, cos), so
        // back = (sin, -cos).
        const Vec3 back{std::sin(yawRad), 0.0f, -std::cos(yawRad)};
        for (int j = 0; j <= m_rows; ++j) {
            // Straight down with a slight backward lean (~10 deg at the
            // hem): the vanilla cape's resting tilt.
            const Vec3 lean = back * (kRestLean * m_restH * static_cast<float>(j));
            for (int i = 0; i <= m_cols; ++i) {
                const Vec3& a = anchors[std::clamp(i, 0, m_cols)];
                const std::size_t idx = index(i, j);
                m_pos[idx] = a + Vec3{0.0f, -m_restH * static_cast<float>(j), 0.0f} + lean;
                m_prev[idx] = m_pos[idx];
            }
        }
        for (int i = 0; i <= m_cols; ++i) m_anchor[i] = anchors[i];
        m_hasLastAnchors = true;
        m_resetPending = false;
    }

    void step(float dt, const Vec3 anchors[], const BodyFrame& body,
              const ClothParams& params, float timeSeconds) {
        if (m_cols <= 0 || m_rows <= 0 || !anchors) return;
        if (!(dt > 0.0f)) return;
        if (dt > 1.0f / 20.0f) dt = 1.0f / 20.0f;
        if (dt < 0.0002f) dt = 0.0002f;

        ClothParams p = params;
        p.clamp();

        // Teleport / first step: re-hang instead of slinging the sheet
        // across the world.
        if (!m_hasLastAnchors || m_resetPending ||
            lengthSquared(anchors[0] - m_anchor[0]) > kTeleportDistance * kTeleportDistance) {
            reset(anchors, body);
            return;
        }
        for (int i = 0; i <= m_cols; ++i) m_anchor[i] = anchors[i];

        const float gravity = kBaseGravity * p.gravityMul;
        const float dtSq = dt * dt;

        // --- Pin the shoulder row (before and after integration, so the
        // pinned row carries no momentum of its own).
        for (int i = 0; i <= m_cols; ++i) {
            const std::size_t idx = index(i, 0);
            m_pos[idx] = anchors[i];
            m_prev[idx] = anchors[i];
        }

        // --- Verlet integration with gravity, wind and relative-wind drag.
        const float maxDisp = kMaxSpeed * dt;
        for (int j = 1; j <= m_rows; ++j) {
            for (int i = 0; i <= m_cols; ++i) {
                const std::size_t idx = index(i, j);
                Vec3& pos = m_pos[idx];
                Vec3& prev = m_prev[idx];
                if (!isFinite(pos) || !isFinite(prev)) {
                    // A particle went non-finite (should not happen with the
                    // clamps below, but never let one NaN poison the sheet):
                    // re-hang the whole cape.
                    m_resetPending = true;
                    return;
                }

                Vec3 vel = (pos - prev) * (1.0f / dt);
                const float speed = length(vel);
                if (speed > kMaxSpeed) vel = vel * (kMaxSpeed / speed);

                Vec3 accel{0.0f, -gravity, 0.0f};
                const Vec3 air = windAt(timeSeconds, pos, p.windMul);
                accel -= (vel - air) * kDrag;

                const float accelLen = length(accel);
                if (accelLen > 200.0f) accel = accel * (200.0f / accelLen);

                Vec3 disp = vel * kDamping * dt + accel * dtSq;
                const float dispLen = length(disp);
                if (dispLen > maxDisp) disp = disp * (maxDisp / dispLen);

                prev = pos;
                pos += disp;
            }
        }

        // --- Distance constraints.
        for (int iter = 0; iter < kConstraintIterations; ++iter) {
            solveEdges(p.stiffness);
            // Re-pin after every pass.
            for (int i = 0; i <= m_cols; ++i) {
                const std::size_t idx = index(i, 0);
                m_pos[idx] = anchors[i];
            }
        }

        // --- Body collision.
        collide(body);

        // --- Final hard stretch clamp on the structural edges.
        clampStretch();

        for (int i = 0; i <= m_cols; ++i) {
            const std::size_t idx = index(i, 0);
            m_pos[idx] = anchors[i];
            m_prev[idx] = anchors[i];
        }
    }

    int cols() const { return m_cols; }
    int rows() const { return m_rows; }
    bool configured() const { return m_cols > 0 && m_rows > 0; }
    bool settled() const { return m_hasLastAnchors && !m_resetPending; }

    const Vec3& at(int i, int j) const { return m_pos[index(i, j)]; }

    // Sum of all structural constraint violations (blocks) — the host tests
    // use it to prove the sheet stays cape-shaped.
    float constraintError() const {
        float err = 0.0f;
        for (int j = 0; j <= m_rows; ++j) {
            for (int i = 0; i < m_cols; ++i) {
                err += std::fabs(length(at(i, j) - at(i + 1, j)) - m_restW);
            }
        }
        for (int j = 0; j < m_rows; ++j) {
            for (int i = 0; i <= m_cols; ++i) {
                err += std::fabs(length(at(i, j) - at(i, j + 1)) - m_restH);
            }
        }
        return err;
    }

private:
    std::size_t index(int i, int j) const {
        return static_cast<std::size_t>(j) * (m_cols + 1) + static_cast<std::size_t>(i);
    }

    void solveEdge(std::size_t a, std::size_t b, float rest, float stiffness) {
        Vec3 d = m_pos[b] - m_pos[a];
        float len = length(d);
        if (len < 0.000001f) return;
        const float diff = (len - rest) / len * stiffness;
        const bool pinnedA = a < static_cast<std::size_t>(m_cols + 1); // row 0
        const bool pinnedB = b < static_cast<std::size_t>(m_cols + 1);
        const float wa = pinnedA ? 0.0f : 1.0f;
        const float wb = pinnedB ? 0.0f : 1.0f;
        const float wsum = wa + wb;
        if (wsum <= 0.0f) return;
        const Vec3 corr = d * (diff / wsum);
        if (wa > 0.0f) m_pos[a] += corr * wa;
        if (wb > 0.0f) m_pos[b] -= corr * wb;
    }

    void solveEdges(float stiffness) {
        const float shearK = stiffness * 0.6f;
        for (int j = 0; j <= m_rows; ++j) {
            for (int i = 0; i < m_cols; ++i) {
                solveEdge(index(i, j), index(i + 1, j), m_restW, stiffness);
            }
        }
        for (int j = 0; j < m_rows; ++j) {
            for (int i = 0; i <= m_cols; ++i) {
                solveEdge(index(i, j), index(i, j + 1), m_restH, stiffness);
            }
        }
        for (int j = 0; j < m_rows; ++j) {
            for (int i = 0; i < m_cols; ++i) {
                solveEdge(index(i, j), index(i + 1, j + 1), m_restD, shearK);
                solveEdge(index(i + 1, j), index(i, j + 1), m_restD, shearK);
            }
        }
    }

    void clampStretch() {
        const auto clampEdge = [&](std::size_t a, std::size_t b, float rest) {
            Vec3 d = m_pos[b] - m_pos[a];
            float len = length(d);
            const float limit = rest * kMaxStretch;
            if (len <= limit || len < 0.000001f) return;
            const bool pinnedA = a < static_cast<std::size_t>(m_cols + 1);
            const bool pinnedB = b < static_cast<std::size_t>(m_cols + 1);
            if (pinnedA && pinnedB) return;
            // d becomes the excess that must be removed so the edge is
            // exactly limit long.
            d = d * ((len - limit) / len);
            if (pinnedA) {
                m_pos[b] -= d;
            } else if (pinnedB) {
                m_pos[a] += d;
            } else {
                m_pos[a] += d * 0.5f;
                m_pos[b] -= d * 0.5f;
            }
        };
        for (int j = 0; j <= m_rows; ++j) {
            for (int i = 0; i < m_cols; ++i) {
                clampEdge(index(i, j), index(i + 1, j), m_restW);
            }
        }
        for (int j = 0; j < m_rows; ++j) {
            for (int i = 0; i <= m_cols; ++i) {
                clampEdge(index(i, j), index(i, j + 1), m_restH);
            }
        }
    }

    // Pushes every particle out of the elliptic cylinder around the body
    // axis. prev is moved by the same delta so the projection neither adds
    // nor removes momentum — the cape slides along the body instead of
    // bouncing off it.
    void collide(const BodyFrame& body) {
        const float yawRad = body.yawDeg * 3.14159265358979323846f / 180.0f;
        const float sinYaw = std::sin(yawRad);
        const float cosYaw = std::cos(yawRad);
        // right = (-cos, -sin), forward = (-sin, cos) — same mapping the
        // render side uses (see wings.cpp).
        const float rightX = -cosYaw, rightZ = -sinYaw;
        const float fwdX = -sinYaw, fwdZ = cosYaw;

        for (int j = 1; j <= m_rows; ++j) {
            for (int i = 0; i <= m_cols; ++i) {
                const std::size_t idx = index(i, j);
                Vec3& pos = m_pos[idx];
                const float dy = pos.y - body.feet.y;
                if (dy < -0.10f || dy > body.height + 0.25f) continue;

                const float dx = pos.x - body.feet.x;
                const float dz = pos.z - body.feet.z;
                const float lx = dx * rightX + dz * rightZ;
                const float lz = dx * fwdX + dz * fwdZ;

                const float ex = lx / kBodyRadiusX;
                const float ez = lz / kBodyRadiusZ;
                const float e2 = ex * ex + ez * ez;
                if (e2 >= 1.0f) continue;

                float nx = ex;
                float nz = ez;
                float len = std::sqrt(e2);
                if (len < 0.0001f) {
                    // Dead center: push straight back out of the body.
                    nx = 0.0f;
                    nz = 1.0f;
                    len = 1.0f;
                }
                const float scale = 1.0f / len;
                const float pushedLx = nx * scale * kBodyRadiusX;
                const float pushedLz = nz * scale * kBodyRadiusZ;

                const float newX = body.feet.x + rightX * pushedLx + fwdX * pushedLz;
                const float newZ = body.feet.z + rightZ * pushedLx + fwdZ * pushedLz;
                const Vec3 delta{newX - pos.x, 0.0f, newZ - pos.z};
                pos += delta;
                m_prev[idx] += delta;
            }
        }
    }

    int m_cols = 0;
    int m_rows = 0;
    std::vector<Vec3> m_pos;
    std::vector<Vec3> m_prev;
    Vec3 m_anchor[64]{};  // last shoulder line (max supported cols+1)
    bool m_hasLastAnchors = false;
    bool m_resetPending = true;
    float m_restW = 0.0f;
    float m_restH = 0.0f;
    float m_restD = 0.0f;
};

// Builds the shoulder anchor line in world space from the body frame: a
// horizontal line across the back, (cols+1) points spanning the cape's 10 px
// width at the shoulder height, sitting on the cape plane behind the torso.
// Model -> world matches wings.cpp: world = feet + right*(-x) - forward*(z),
// y = feet.y + y/16.
inline void buildAnchors(const BodyFrame& body, int cols, Vec3 outAnchors[]) {
    const float yawRad = body.yawDeg * 3.14159265358979323846f / 180.0f;
    const float cosYaw = std::cos(yawRad);
    const float sinYaw = std::sin(yawRad);
    const float rightX = -cosYaw, rightZ = -sinYaw;
    const float fwdX = -sinYaw, fwdZ = cosYaw;

    const float topY = body.feet.y + kCapeTopModelY * kPxToBlocks;
    const float back = kCapeBackModelZ * kPxToBlocks;

    for (int i = 0; i <= cols; ++i) {
        const float along = (static_cast<float>(i) / static_cast<float>(cols) - 0.5f) * kCapeWidthBlocks;
        const float px = -along; // model x -> -right
        outAnchors[i] = {
            body.feet.x + rightX * px - fwdX * back,
            topY,
            body.feet.z + rightZ * px - fwdZ * back,
        };
    }
}

} // namespace bedrocktools::modules::capephysics

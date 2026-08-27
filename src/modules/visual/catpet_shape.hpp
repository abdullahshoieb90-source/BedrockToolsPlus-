#pragma once

// Pure geometry + animation helpers for the Cat Pet module.
//
// The Cat Pet is a chibi voxel cat drawn as a world-space overlay with the
// Bedrock tessellator (same RenderLevel + tessellator pattern as the Wings
// module). Everything that decides *where* the cat's boxes sit, *how* the cat
// follows its owner and *what pose* it is in lives here, so it compiles and
// can be unit-tested on the host without Minecraft
// (tests/catpet_shape_test.cpp) and so tools/catpet_preview.cpp can render
// the exact same cat offline.
//
//   CatPart / kCatParts   the part hierarchy (body, head, ears, legs, tail...)
//   CatPose               per-channel rotations produced by the animation
//   computeCatPose        idle / walk / run / sit pose solver
//   partTransforms        resolves the part hierarchy into affine transforms
//   buildPartCorners      the 8 box corners of a part in cat-local px
//   stepCatFollow         the "walk to the owner's heel" follow solver
//
// Cat-local coordinate conventions (16 px = 1 block):
//   +x  the cat's right side
//   +y  up (feet at y = 0)
//   +z  the cat's forward (nose direction)
//
// Reuses the Vec3 math and face-shading helpers from wings_shape.hpp so the
// cat and the wings are lit consistently.

#include "modules/visual/wings_shape.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace bedrocktools::modules::catpet {

using wings::Vec3;
using wings::kDegToRad;
using wings::kPi;
using wings::kPxToBlocks;

// ---------------------------------------------------------------------------
// Small 3D affine math (the cat is a real 3D hierarchy, so the 2D wing poses
// are not enough here).
// ---------------------------------------------------------------------------

struct Mat3 {
    // Row-major.
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

inline Mat3 matMul(const Mat3& a, const Mat3& b) {
    Mat3 out;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out.m[r * 3 + c] = a.m[r * 3 + 0] * b.m[0 * 3 + c]
                             + a.m[r * 3 + 1] * b.m[1 * 3 + c]
                             + a.m[r * 3 + 2] * b.m[2 * 3 + c];
        }
    }
    return out;
}

inline Vec3 matApply(const Mat3& a, const Vec3& p) {
    return {
        a.m[0] * p.x + a.m[1] * p.y + a.m[2] * p.z,
        a.m[3] * p.x + a.m[4] * p.y + a.m[5] * p.z,
        a.m[6] * p.x + a.m[7] * p.y + a.m[8] * p.z,
    };
}

inline Mat3 rotX(float rad) {
    const float c = std::cos(rad), s = std::sin(rad);
    Mat3 out;
    out.m[0] = 1; out.m[1] = 0; out.m[2] = 0;
    out.m[3] = 0; out.m[4] = c; out.m[5] = -s;
    out.m[6] = 0; out.m[7] = s; out.m[8] = c;
    return out;
}

inline Mat3 rotY(float rad) {
    const float c = std::cos(rad), s = std::sin(rad);
    Mat3 out;
    out.m[0] = c;  out.m[1] = 0; out.m[2] = s;
    out.m[3] = 0;  out.m[4] = 1; out.m[5] = 0;
    out.m[6] = -s; out.m[7] = 0; out.m[8] = c;
    return out;
}

inline Mat3 rotZ(float rad) {
    const float c = std::cos(rad), s = std::sin(rad);
    Mat3 out;
    out.m[0] = c; out.m[1] = -s; out.m[2] = 0;
    out.m[3] = s; out.m[4] = c;  out.m[5] = 0;
    out.m[6] = 0; out.m[7] = 0;  out.m[8] = 1;
    return out;
}

// p' = r * p + t
struct Affine {
    Mat3 r;
    Vec3 t{0.0f, 0.0f, 0.0f};
};

inline Vec3 affineApply(const Affine& a, const Vec3& p) {
    const Vec3 rp = matApply(a.r, p);
    return {rp.x + a.t.x, rp.y + a.t.y, rp.z + a.t.z};
}

// a after b: (a o b)(p) = a(b(p))
inline Affine affineCompose(const Affine& a, const Affine& b) {
    Affine out;
    out.r = matMul(a.r, b.r);
    const Vec3 abt = matApply(a.r, b.t);
    out.t = {abt.x + a.t.x, abt.y + a.t.y, abt.z + a.t.z};
    return out;
}

// Rotation about a pivot plus an extra translation:
// p' = R * (p - pivot) + pivot + extra
inline Affine affineAboutPivot(const Mat3& r, const Vec3& pivot, const Vec3& extra) {
    Affine out;
    out.r = r;
    const Vec3 rp = matApply(r, pivot);
    out.t = {pivot.x - rp.x + extra.x, pivot.y - rp.y + extra.y, pivot.z - rp.z + extra.z};
    return out;
}

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

enum PaletteSlot {
    kSlotBody = 0,
    kSlotBelly,
    kSlotHead,
    kSlotEar,
    kSlotEarInner,
    kSlotMuzzle,
    kSlotNose,
    kSlotEye,
    kSlotLeg,
    kSlotPaw,
    kSlotTail,
    kSlotTailTip,
    kSlotBell,
    kPaletteSlotCount,
};

struct CatStyle {
    const char* id;
    const char* label;
    unsigned char rgb[kPaletteSlotCount][3];
};

// The selectable cat coats. Party colors are stylized (single color per box),
// so "calico" and "siamese" are impressions, not pixel-perfect breeds.
constexpr CatStyle kCatStyles[] = {
    {"orange", "Orange Tabby", {
        {235, 145, 60},   // body
        {248, 230, 196},  // belly
        {235, 145, 60},   // head
        {214, 122, 46},   // ear
        {244, 168, 176},  // ear inner
        {250, 240, 222},  // muzzle
        {228, 110, 130},  // nose
        {88, 205, 120},   // eye
        {228, 136, 54},   // leg
        {248, 230, 196},  // paw
        {214, 122, 46},   // tail
        {250, 240, 222},  // tail tip
        {255, 204, 74},   // bell
    }},
    {"black", "Tuxedo Black", {
        {40, 40, 47},
        {236, 236, 240},
        {40, 40, 47},
        {30, 30, 37},
        {240, 152, 162},
        {240, 240, 244},
        {235, 122, 142},
        {255, 214, 74},
        {40, 40, 47},
        {240, 240, 244},
        {30, 30, 37},
        {240, 240, 244},
        {255, 204, 74},
    }},
    {"white", "Snow White", {
        {244, 244, 240},
        {255, 255, 252},
        {244, 244, 240},
        {234, 230, 224},
        {248, 172, 182},
        {255, 252, 248},
        {241, 132, 152},
        {110, 180, 255},
        {247, 245, 240},
        {252, 250, 246},
        {238, 234, 228},
        {255, 255, 252},
        {255, 204, 74},
    }},
    {"gray", "Gray Tabby", {
        {129, 134, 144},
        {218, 222, 228},
        {129, 134, 144},
        {108, 112, 122},
        {242, 160, 170},
        {226, 229, 234},
        {206, 108, 126},
        {255, 190, 82},
        {122, 127, 137},
        {218, 222, 228},
        {108, 112, 122},
        {226, 229, 234},
        {255, 204, 74},
    }},
    {"siamese", "Siamese", {
        {238, 224, 200},
        {247, 239, 222},
        {231, 215, 189},
        {88, 68, 58},
        {202, 142, 152},
        {96, 74, 62},
        {70, 52, 46},
        {96, 160, 255},
        {223, 207, 181},
        {88, 68, 58},
        {152, 122, 98},
        {88, 68, 58},
        {255, 204, 74},
    }},
    {"calico", "Calico", {
        {243, 239, 231},
        {252, 250, 244},
        {236, 152, 72},
        {62, 58, 62},
        {246, 166, 176},
        {252, 248, 240},
        {230, 116, 136},
        {92, 190, 130},
        {243, 239, 231},
        {252, 250, 244},
        {62, 58, 62},
        {236, 152, 72},
        {255, 204, 74},
    }},
};

constexpr int kCatStyleCount = static_cast<int>(sizeof(kCatStyles) / sizeof(kCatStyles[0]));

inline int catStyleIndexForId(const std::string& id) {
    for (int i = 0; i < kCatStyleCount; ++i) {
        if (id == kCatStyles[i].id) return i;
    }
    return 0;  // orange tabby default
}

// Serializes the picker value in the launcher's radio format:
// "<selectedIndex>,<id1>,<id2>,..." (same convention as Wings/Custom Capes).
inline std::string catStyleRadioValue(int index) {
    if (index < 0 || index >= kCatStyleCount) index = 0;
    std::string value = std::to_string(index);
    for (int i = 0; i < kCatStyleCount; ++i) {
        value += ',';
        value += kCatStyles[i].id;
    }
    return value;
}

// Parses a value coming from the config file (full radio value), from the
// launcher (just the numeric index) or a bare style id.
inline int resolveCatStyleIndex(const std::string& value) {
    if (value.empty()) return 0;
    const std::size_t comma = value.find(',');
    const std::string head = value.substr(0, comma);
    bool numeric = !head.empty();
    for (char c : head) {
        if (c < '0' || c > '9') { numeric = false; break; }
    }
    if (numeric) {
        int idx = 0;
        for (char c : head) {
            idx = idx * 10 + (c - '0');
            if (idx > 1000) break;
        }
        if (idx >= 0 && idx < kCatStyleCount) return idx;
        return 0;
    }
    return catStyleIndexForId(head);
}

// ---------------------------------------------------------------------------
// Part hierarchy
// ---------------------------------------------------------------------------

// Animation channels; each part is driven by exactly one (kChanNone = rigid).
enum Channel {
    kChanNone = -1,
    kChanBody = 0,
    kChanHead,
    kChanEarL,
    kChanEarR,
    kChanLegFL,
    kChanLegFR,
    kChanLegBL,
    kChanLegBR,
    kChanTail1,
    kChanTail2,
    kChanTail3,
    kChannelCount,
};

struct CatPart {
    const char* name;
    int parent;        // index into kCatParts, -1 for the body root
    int channel;       // Channel driving this part's own rotation
    float pivot[3];    // rotation pivot in cat-local px (rest space)
    float o[3];        // box origin relative to the pivot (px)
    float s[3];        // box size (px)
    int slotMain;      // PaletteSlot for all faces by default
    int slotBottom;    // y-min face override (-1 = main)
    int slotFront;     // z-max (forward) face override (-1 = main)
    float tint;        // per-part brightness multiplier
    bool isEye;        // eyes shrink vertically while blinking
};

// Chibi voxel cat: big head, chunky body, short legs, expressive tail.
// Feet on y = 0, nose towards +z. Sizes are model px (16 px = 1 block).
constexpr CatPart kCatParts[] = {
    //  name       parent channel     pivot                box origin              box size            main            bottom        front          tint   eye
    {"body",       -1, kChanBody,  {0.0f, 5.0f, 3.5f},   {-4.0f, -2.0f, -10.5f}, {8.0f, 7.0f, 12.0f}, kSlotBody,      kSlotBelly,   kSlotBelly,    1.00f, false},
    {"head",        0, kChanHead,  {0.0f, 9.0f, 4.0f},   {-4.5f, -2.0f, -0.5f},  {9.0f, 8.0f, 8.0f},  kSlotHead,      kSlotMuzzle,  kSlotHead,     1.04f, false},
    {"muzzle",      1, kChanNone,  {0.0f, 9.5f, 11.5f},  {-2.0f, -1.7f, -0.3f},  {4.0f, 3.0f, 1.6f},  kSlotMuzzle,    -1,           -1,            1.02f, false},
    {"nose",        1, kChanNone,  {0.0f, 10.9f, 12.8f}, {-0.7f, -0.7f, -0.4f},  {1.4f, 1.1f, 1.0f},  kSlotNose,      -1,           -1,            1.05f, false},
    {"eye_left",    1, kChanNone,  {-2.3f, 11.9f, 11.5f},{-0.9f, -1.0f, -0.2f},  {1.8f, 2.0f, 0.8f},  kSlotEye,       -1,           -1,            1.15f, true},
    {"eye_right",   1, kChanNone,  {2.3f, 11.9f, 11.5f}, {-0.9f, -1.0f, -0.2f},  {1.8f, 2.0f, 0.8f},  kSlotEye,       -1,           -1,            1.15f, true},
    {"ear_left",    1, kChanEarL,  {-2.9f, 14.9f, 7.5f}, {-1.4f, -0.4f, -0.9f},  {2.8f, 3.2f, 1.7f},  kSlotEar,       -1,           kSlotEarInner, 1.00f, false},
    {"ear_right",   1, kChanEarR,  {2.9f, 14.9f, 7.5f},  {-1.4f, -0.4f, -0.9f},  {2.8f, 3.2f, 1.7f},  kSlotEar,       -1,           kSlotEarInner, 1.00f, false},
    {"bell",        1, kChanNone,  {0.0f, 7.2f, 10.6f},  {-0.8f, -1.5f, -0.7f},  {1.6f, 1.6f, 1.5f},  kSlotBell,      -1,           -1,            1.10f, false},
    {"leg_fl",      0, kChanLegFL, {-2.6f, 4.8f, 3.4f},  {-1.1f, -4.8f, -1.1f},  {2.2f, 4.9f, 2.2f},  kSlotLeg,       kSlotPaw,     -1,            0.96f, false},
    {"leg_fr",      0, kChanLegFR, {2.6f, 4.8f, 3.4f},   {-1.1f, -4.8f, -1.1f},  {2.2f, 4.9f, 2.2f},  kSlotLeg,       kSlotPaw,     -1,            0.96f, false},
    {"leg_bl",      0, kChanLegBL, {-2.7f, 4.8f, -5.4f}, {-1.2f, -4.8f, -1.3f},  {2.4f, 4.9f, 2.6f},  kSlotLeg,       kSlotPaw,     -1,            0.94f, false},
    {"leg_br",      0, kChanLegBR, {2.7f, 4.8f, -5.4f},  {-1.2f, -4.8f, -1.3f},  {2.4f, 4.9f, 2.6f},  kSlotLeg,       kSlotPaw,     -1,            0.94f, false},
    {"tail_1",      0, kChanTail1, {0.0f, 8.6f, -6.8f},  {-0.9f, -0.9f, -4.6f},  {1.8f, 1.8f, 4.8f},  kSlotTail,      -1,           -1,            0.98f, false},
    {"tail_2",     13, kChanTail2, {0.0f, 8.6f, -11.2f}, {-0.8f, -0.8f, -4.4f},  {1.6f, 1.6f, 4.6f},  kSlotTail,      -1,           -1,            1.00f, false},
    {"tail_3",     14, kChanTail3, {0.0f, 8.6f, -15.4f}, {-0.7f, -0.7f, -4.2f},  {1.4f, 1.4f, 4.4f},  kSlotTailTip,   -1,           -1,            1.06f, false},
};

constexpr int kCatPartCount = static_cast<int>(sizeof(kCatParts) / sizeof(kCatParts[0]));

// ---------------------------------------------------------------------------
// Pose
// ---------------------------------------------------------------------------

// Per-channel rotations (degrees) plus whole-cat offsets produced by the
// animation solver. Angle conventions:
//   pitch: rotation about +x; positive lifts whatever lies behind (-z) and
//          dips what lies ahead (+z) - so positive tail pitch raises the tail
//          and positive leg pitch swings the paw backwards.
//   yaw:   rotation about +y; positive turns the part towards the cat's left.
//   roll:  rotation about +z.
struct CatPose {
    float hopPx = 0.0f;           // whole-cat vertical offset (gallop bounce)
    float bodyBobPx = 0.0f;       // body-only breathing offset
    float bodyPitchDeg = 0.0f;
    float bodyRollDeg = 0.0f;
    float headYawDeg = 0.0f;
    float headPitchDeg = 0.0f;
    float headRollDeg = 0.0f;
    float earPitchDeg[2] = {0, 0};        // L, R; negative flattens back
    float legPitchDeg[4] = {0, 0, 0, 0};  // FL, FR, BL, BR
    float tailPitchDeg[3] = {0, 0, 0};
    float tailYawDeg[3] = {0, 0, 0};
    float blink = 0.0f;           // 0 open .. 1 closed
    float sit = 0.0f;             // resolved sit blend (for tests/preview)
    float move = 0.0f;            // resolved move blend (for tests/preview)
};

inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// Sharp periodic spike in [0, 1]: ~zero most of the cycle, briefly ~1.
// Used for ear twitches and blinks.
inline float spike(float t, float rate, float phase, float sharpness) {
    const float s = std::sin(t * rate + phase);
    const float positive = s > 0.0f ? s : 0.0f;
    return std::pow(positive, sharpness);
}

// The pose solver. All inputs are continuous, so the pose is too:
//   t            idle clock in seconds (breathing, tail sway, blinks...)
//   stridePhase  accumulated walk phase in radians (owned by the module so
//                the stride frequency can follow the cat's speed smoothly)
//   move         0 = standing .. 1 = running, already smoothed by the caller
//   sit          0 = standing .. 1 = sitting, already smoothed by the caller
inline CatPose computeCatPose(float t, float stridePhase, float move, float sit) {
    move = std::clamp(move, 0.0f, 1.0f);
    sit = std::clamp(sit, 0.0f, 1.0f);
    // Sitting and moving are mutually exclusive; trust movement first.
    sit = std::min(sit, 1.0f - move);
    const float idle = 1.0f - move;

    CatPose p;
    p.sit = sit;
    p.move = move;

    // --- Breathing & gallop bounce -------------------------------------
    p.bodyBobPx = 0.35f * std::sin(t * 1.7f) * idle;
    p.hopPx = move * 1.35f * std::fabs(std::sin(stridePhase));
    p.bodyPitchDeg = move * 2.6f * std::sin(2.0f * stridePhase);
    p.bodyRollDeg = move * 1.4f * std::sin(stridePhase);

    // --- Legs: diagonal pairs (trot), amplitude grows with speed --------
    const float swing = (26.0f + 16.0f * move) * std::sin(stridePhase) * move;
    p.legPitchDeg[0] = swing;    // FL
    p.legPitchDeg[3] = swing;    // BR
    p.legPitchDeg[1] = -swing;   // FR
    p.legPitchDeg[2] = -swing;   // BL

    // --- Head: idle look-around, tiny bob while moving ------------------
    p.headYawDeg = idle * (12.0f * std::sin(t * 0.37f) + 5.0f * std::sin(t * 1.13f));
    p.headPitchDeg = idle * 3.0f * std::sin(t * 0.53f) + move * 2.5f * std::sin(2.0f * stridePhase);
    p.headRollDeg = idle * 4.0f * std::sin(t * 0.23f);

    // --- Ears: occasional twitch, pinned back a little when running -----
    const float twitchL = spike(t, 0.53f, 0.0f, 30.0f);
    const float twitchR = spike(t, 0.47f, 2.3f, 30.0f);
    p.earPitchDeg[0] = -30.0f * twitchL - 8.0f * move;
    p.earPitchDeg[1] = -30.0f * twitchR - 8.0f * move;

    // --- Tail: slow S-sway when idle, excited swish when moving ---------
    for (int i = 0; i < 3; ++i) {
        const float fi = static_cast<float>(i);
        const float basePitch = lerpf(50.0f - 17.0f * fi, 60.0f - 26.0f * fi, move);
        p.tailPitchDeg[i] = basePitch + 6.0f * std::sin(t * 0.9f - fi * 0.7f) * idle
                          + 5.0f * std::sin(stridePhase - fi * 0.8f) * move;
        p.tailYawDeg[i] = idle * 14.0f * std::sin(t * 1.0f - fi * 0.8f)
                        + move * 16.0f * std::sin(stridePhase - fi * 0.9f);
    }

    // --- Blink -----------------------------------------------------------
    p.blink = spike(t, 0.61f, 1.3f, 60.0f);

    // --- Sit: chest up, haunches down, hind legs tucked, tail wrapped ----
    if (sit > 0.0f) {
        const float sitPitch = -26.0f * sit;   // rear (-z) sinks, chest rises
        p.bodyPitchDeg += sitPitch;
        p.hopPx += 1.6f * sit;                 // keep the haunches near ground
        p.bodyBobPx *= (1.0f - 0.5f * sit);
        // Front legs stay vertical (counter the body pitch); hind legs tuck.
        p.legPitchDeg[0] += -sitPitch;
        p.legPitchDeg[1] += -sitPitch;
        p.legPitchDeg[2] += -sitPitch + 44.0f * sit;
        p.legPitchDeg[3] += -sitPitch + 44.0f * sit;
        // Tail wraps around the haunches with a lazy tip flick.
        for (int i = 0; i < 3; ++i) {
            const float fi = static_cast<float>(i);
            p.tailPitchDeg[i] = lerpf(p.tailPitchDeg[i], 16.0f - 9.0f * fi, sit);
            p.tailYawDeg[i] = lerpf(p.tailYawDeg[i], 26.0f + 16.0f * fi, sit);
        }
        p.tailYawDeg[2] += 10.0f * std::sin(t * 1.2f) * sit;
    }

    return p;
}

// Stride frequency (rad/s) for a given horizontal speed (blocks/s). Feet move
// faster when the cat runs; clamped so the legs never blur into a fan.
inline float strideRateForSpeed(float speed) {
    if (speed < 0.0f) speed = 0.0f;
    const float rate = 5.0f + 2.4f * speed;
    return rate > 26.0f ? 26.0f : rate;
}

// ---------------------------------------------------------------------------
// Hierarchy resolution + box building
// ---------------------------------------------------------------------------

// Local rotation matrix for a channel given a pose.
inline Mat3 channelRotation(int channel, const CatPose& pose) {
    switch (channel) {
        case kChanBody:
            return matMul(rotZ(pose.bodyRollDeg * kDegToRad), rotX(pose.bodyPitchDeg * kDegToRad));
        case kChanHead:
            return matMul(rotY(pose.headYawDeg * kDegToRad),
                          matMul(rotX(pose.headPitchDeg * kDegToRad), rotZ(pose.headRollDeg * kDegToRad)));
        case kChanEarL: return rotX(pose.earPitchDeg[0] * kDegToRad);
        case kChanEarR: return rotX(pose.earPitchDeg[1] * kDegToRad);
        case kChanLegFL: return rotX(pose.legPitchDeg[0] * kDegToRad);
        case kChanLegFR: return rotX(pose.legPitchDeg[1] * kDegToRad);
        case kChanLegBL: return rotX(pose.legPitchDeg[2] * kDegToRad);
        case kChanLegBR: return rotX(pose.legPitchDeg[3] * kDegToRad);
        case kChanTail1:
            return matMul(rotY(pose.tailYawDeg[0] * kDegToRad), rotX(pose.tailPitchDeg[0] * kDegToRad));
        case kChanTail2:
            return matMul(rotY(pose.tailYawDeg[1] * kDegToRad), rotX(pose.tailPitchDeg[1] * kDegToRad));
        case kChanTail3:
            return matMul(rotY(pose.tailYawDeg[2] * kDegToRad), rotX(pose.tailPitchDeg[2] * kDegToRad));
        default: return Mat3{};
    }
}

// Resolves the hierarchy: out[i] maps part i's rest-space points into posed
// cat-local space. kCatParts is ordered parents-first, so one pass suffices.
inline void partTransforms(const CatPose& pose, Affine out[kCatPartCount]) {
    for (int i = 0; i < kCatPartCount; ++i) {
        const CatPart& part = kCatParts[i];
        Vec3 extra{0.0f, 0.0f, 0.0f};
        if (part.channel == kChanBody) {
            extra.y = pose.hopPx + pose.bodyBobPx;
        }
        const Mat3 rot = channelRotation(part.channel, pose);
        const Vec3 pivot{part.pivot[0], part.pivot[1], part.pivot[2]};
        const Affine local = affineAboutPivot(rot, pivot, extra);
        if (part.parent < 0 || part.parent >= i) {
            out[i] = local;
        } else {
            out[i] = affineCompose(out[part.parent], local);
        }
    }
}

// Corner index bits match wings_shape.hpp: bit0 = x side, bit1 = y side,
// bit2 = z side, so wings::kFaceRings and the FaceSlot enum apply unchanged.
inline void buildPartCorners(int partIndex, const Affine& xform, float blink,
                             Vec3 corners[wings::kCornerCount]) {
    const CatPart& part = kCatParts[partIndex];
    float sy = part.s[1];
    float oy = part.o[1];
    if (part.isEye && blink > 0.0f) {
        // Blink: squash the eye towards its vertical center.
        const float keep = 1.0f - 0.85f * std::clamp(blink, 0.0f, 1.0f);
        const float cy = oy + sy * 0.5f;
        sy *= keep;
        oy = cy - sy * 0.5f;
    }
    for (int i = 0; i < wings::kCornerCount; ++i) {
        const float lx = part.pivot[0] + part.o[0] + ((i & 1) ? part.s[0] : 0.0f);
        const float ly = part.pivot[1] + oy + (((i >> 1) & 1) ? sy : 0.0f);
        const float lz = part.pivot[2] + part.o[2] + (((i >> 2) & 1) ? part.s[2] : 0.0f);
        corners[i] = affineApply(xform, Vec3{lx, ly, lz});
    }
}

// Face colors for a part: main everywhere, bottom/front overrides.
inline void partFaceSlots(int partIndex, int slots[wings::kFaceCount]) {
    const CatPart& part = kCatParts[partIndex];
    for (int f = 0; f < wings::kFaceCount; ++f) slots[f] = part.slotMain;
    if (part.slotBottom >= 0) slots[wings::kFaceBottom] = part.slotBottom;
    if (part.slotFront >= 0) slots[wings::kFaceOuter] = part.slotFront;  // z-max = forward
}

// Outward model-space normal of a face before the part transform.
inline Vec3 faceNormalLocal(int face) {
    switch (face) {
        case wings::kFaceInner: return {0.0f, 0.0f, -1.0f};   // z min (back)
        case wings::kFaceOuter: return {0.0f, 0.0f, 1.0f};    // z max (front)
        case wings::kFaceSpanMin: return {-1.0f, 0.0f, 0.0f};
        case wings::kFaceSpanMax: return {1.0f, 0.0f, 0.0f};
        case wings::kFaceBottom: return {0.0f, -1.0f, 0.0f};
        default: return {0.0f, 1.0f, 0.0f};                   // top
    }
}

// Maps a posed cat-local point (px) into world space.
//   origin  the cat's feet center in world blocks
//   right / forward  the cat's horizontal basis vectors (unit)
//   scale   size multiplier (1.0 = a 16 px = 1 block cat)
inline Vec3 catPointToWorld(const Vec3& p, const Vec3& origin, const Vec3& right,
                            const Vec3& forward, float scale) {
    const float s = kPxToBlocks * scale;
    return {
        origin.x + right.x * p.x * s + forward.x * p.z * s,
        origin.y + p.y * s,
        origin.z + right.z * p.x * s + forward.z * p.z * s,
    };
}

// Maps a cat-local direction into world space (no translation, no scale).
inline Vec3 catDirToWorld(const Vec3& d, const Vec3& right, const Vec3& forward) {
    return {
        right.x * d.x + forward.x * d.z,
        d.y,
        right.z * d.x + forward.z * d.z,
    };
}

// Basis from the cat's yaw (degrees). Yaw 0 faces +z; right/forward follow
// the same handedness the renderer and the preview use.
inline void catBasis(float yawDeg, Vec3& right, Vec3& forward) {
    const float r = yawDeg * kDegToRad;
    forward = {std::sin(r), 0.0f, std::cos(r)};
    right = {std::cos(r), 0.0f, -std::sin(r)};
}

inline float catYawFromDir(float dx, float dz) {
    return std::atan2(dx, dz) / kDegToRad;
}

// ---------------------------------------------------------------------------
// Follow solver ("come to heel")
// ---------------------------------------------------------------------------

struct FollowState {
    bool hasPos = false;
    float x = 0.0f, y = 0.0f, z = 0.0f;   // cat feet center, world blocks
    float yawDeg = 0.0f;
    float smoothedSpeed = 0.0f;           // blocks/s, for the animation blend
};

struct FollowParams {
    float deadzone = 0.45f;       // blocks: stop when this close to the heel spot
    float accel = 2.6f;           // speed per block of distance
    float minChaseSpeed = 0.6f;   // blocks/s once outside the deadzone
    float maxSpeed = 9.0f;        // blocks/s sprint cap
    float teleportDist = 12.0f;   // blocks: snap when left too far behind
    float yLerpRate = 8.0f;       // 1/s vertical follow
    float yawRate = 10.0f;        // 1/s yaw approach
    float speedSmoothRate = 12.0f;
};

struct FollowResult {
    float speed = 0.0f;       // instantaneous chase speed this step (blocks/s)
    bool teleported = false;
};

inline float approachAngleDeg(float current, float target, float t) {
    float diff = target - current;
    while (diff > 180.0f) diff -= 360.0f;
    while (diff < -180.0f) diff += 360.0f;
    float out = current + diff * t;
    while (out > 180.0f) out -= 360.0f;
    while (out < -180.0f) out += 360.0f;
    return out;
}

// One follow step. `target` is the heel spot next to the owner; `owner` is
// the owner's feet center (the cat looks at the owner while idle).
inline FollowResult stepCatFollow(FollowState& st, const Vec3& target, const Vec3& owner,
                                  float ownerYawDeg, float dt, const FollowParams& prm = {}) {
    FollowResult res;
    if (dt <= 0.0f) return res;

    const float dxT = target.x - st.x;
    const float dyT = target.y - st.y;
    const float dzT = target.z - st.z;
    const float dist3dSq = dxT * dxT + dyT * dyT + dzT * dzT;

    if (!st.hasPos || dist3dSq > prm.teleportDist * prm.teleportDist) {
        st.x = target.x;
        st.y = target.y;
        st.z = target.z;
        st.yawDeg = ownerYawDeg;
        st.smoothedSpeed = 0.0f;
        st.hasPos = true;
        res.teleported = true;
        return res;
    }

    const float dist2d = std::sqrt(dxT * dxT + dzT * dzT);
    float speed = 0.0f;
    if (dist2d > prm.deadzone) {
        speed = std::clamp(prm.minChaseSpeed + (dist2d - prm.deadzone) * prm.accel,
                           0.0f, prm.maxSpeed);
        const float step = std::min(dist2d - prm.deadzone * 0.5f, speed * dt);
        if (dist2d > 1e-5f) {
            st.x += dxT / dist2d * step;
            st.z += dzT / dist2d * step;
        }
    }

    // Vertical follow is a plain smooth chase (the overlay has no collision).
    st.y += dyT * std::min(1.0f, dt * prm.yLerpRate);

    // Face the walk direction while moving, otherwise turn towards the owner.
    float desiredYaw = st.yawDeg;
    if (speed > 0.05f) {
        desiredYaw = catYawFromDir(dxT, dzT);
    } else {
        const float lx = owner.x - st.x;
        const float lz = owner.z - st.z;
        if (lx * lx + lz * lz > 0.05f) desiredYaw = catYawFromDir(lx, lz);
    }
    st.yawDeg = approachAngleDeg(st.yawDeg, desiredYaw, std::min(1.0f, dt * prm.yawRate));

    st.smoothedSpeed += (speed - st.smoothedSpeed) * std::min(1.0f, dt * prm.speedSmoothRate);
    res.speed = speed;
    return res;
}

// Heel spot: behind-left of the owner, scaled a little with the pet size so a
// giant cat does not stand inside its owner.
inline Vec3 heelTarget(const Vec3& ownerFeet, float ownerYawDeg, float petScale) {
    Vec3 right, forward;
    catBasis(ownerYawDeg, right, forward);
    const float pad = 0.75f + 0.45f * std::max(petScale, 0.1f);
    return {
        ownerFeet.x - forward.x * pad + right.x * (0.55f + 0.35f * petScale),
        ownerFeet.y,
        ownerFeet.z - forward.z * pad + right.z * (0.55f + 0.35f * petScale),
    };
}

}  // namespace bedrocktools::modules::catpet

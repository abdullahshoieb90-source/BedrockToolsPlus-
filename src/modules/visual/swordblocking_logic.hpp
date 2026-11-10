#pragma once

// Pure, host-testable pieces of the Sword Blocking module.
//
// The module itself (swordblocking.cpp) only does the Minecraft plumbing:
// reading the carried item, tracking input, hooking the first-person item
// transform and submitting the HUD overlay. Everything that decides *what*
// the module should look like lives here, so it can be unit-tested without the
// game, and so the numbers stay in one place:
//
//   * which item names count as a sword,
//   * how the hold-mode radio is encoded in the config,
//   * how the block weight eases in and out over time,
//   * the 4x4 pose matrix applied to the held item, and
//   * the shield primitives drawn under the crosshair.
//
// The matrix helpers are a small column-major 4x4 math library instead of glm
// so this header (and the module) stays free of third-party includes.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace swordblocking {

// A weight of 0 means "not blocking", 1 means "fully raised". Values in
// between are the blend of the visual pose, so the sword glides into and out
// of the guard instead of snapping.
inline constexpr float activeThreshold = 0.02f;

// ---------------------------------------------------------------------------
// Item names
// ---------------------------------------------------------------------------

// ItemStackBase::getRawNameId() of every vanilla sword ends in "_sword"
// (wooden_sword ... netherite_sword), and that suffix is what the game itself
// keys the sword item class on, so modded swords that keep the vanilla naming
// are picked up too.
inline bool isSwordName(std::string_view nameId) {
    constexpr std::string_view suffix = "_sword";
    if (nameId.size() <= suffix.size()) return false;
    if (nameId.substr(nameId.size() - suffix.size()) != suffix) return false;
    return nameId.find_first_of(" \t\r\n/#") == std::string_view::npos;
}

// ---------------------------------------------------------------------------
// Hold source
// ---------------------------------------------------------------------------

// The order of this enum is persisted (the radio stores an index), so new
// entries must only ever be appended.
enum class HoldMode : int {
    Auto = 0,      // any of the mouse / on-screen button / key is held
    RightClick,    // only the configured mouse button
    ScreenButton,  // only the launcher overlay hold button (touch devices)
    HoldKey,       // only the configured keybind
    Always,        // block whenever the sword is held, no input needed
    Count
};

inline constexpr std::array<std::string_view, static_cast<std::size_t>(HoldMode::Count)> holdModeNames = {
    "Auto", "Right Click", "Screen Button", "Hold Key", "Always"
};

inline int holdModeIndex(std::string_view value) {
    value = value.substr(0, value.find(','));
    if (value.empty()) return 0;
    // Accept the stored index first, then a label, so a hand-edited config
    // works either way.
    if (value.size() == 1 && value[0] >= '0' && value[0] <= '9') {
        const int index = value[0] - '0';
        if (index >= 0 && index < static_cast<int>(HoldMode::Count)) return index;
    }
    for (std::size_t i = 0; i < holdModeNames.size(); ++i) {
        if (value == holdModeNames[i]) return static_cast<int>(i);
    }
    return 0;
}

// Launcher radio format: "<selectedIndex>,<label>,<label>...".
inline std::string holdModeRadioValue(int index) {
    if (index < 0 || index >= static_cast<int>(HoldMode::Count)) index = 0;
    std::string value = std::to_string(index);
    for (const auto name : holdModeNames) {
        value += ',';
        value += name;
    }
    return value;
}

// Whether the currently held inputs should block for `mode`.
// `mouse`, `button` and `key` are the three live hold sources.
inline bool holdActive(HoldMode mode, bool mouse, bool button, bool key) {
    switch (mode) {
        case HoldMode::Auto: return mouse || button || key;
        case HoldMode::RightClick: return mouse;
        case HoldMode::ScreenButton: return button;
        case HoldMode::HoldKey: return key;
        case HoldMode::Always: return true;
        case HoldMode::Count: break;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Weight animation
// ---------------------------------------------------------------------------

inline constexpr float minFadeSpeed = 0.05f;
inline constexpr float maxFadeSpeed = 1.0f;
inline constexpr float defaultFadeSpeed = 0.45f;

// Full weight per second at Fade Speed 1. A 0.45 setting therefore raises the
// sword in roughly 0.45 s, which reads as a deliberate guard without feeling
// sluggish.
inline constexpr float fadeRateScale = 5.0f;

// Longest frame delta that still counts. A pause (singleplayer menu, app
// switch) can hand the renderer a multi-second delta; without this cap the
// weight would jump straight to its target on resume.
inline constexpr float maxFrameDelta = 0.25f;

inline float clampFadeSpeed(float value) {
    if (!std::isfinite(value)) return defaultFadeSpeed;
    return std::clamp(value, minFadeSpeed, maxFadeSpeed);
}

// Moves `weight` one frame toward 1 (rising) or 0 (falling), linearly at the
// configured speed. Always clamped to [0, 1] and never overshoots.
inline float advanceWeight(float weight, bool rising, float fadeSpeed, float deltaSeconds) {
    if (!std::isfinite(weight)) weight = 0.0f;
    weight = std::clamp(weight, 0.0f, 1.0f);
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f) return weight;
    const float delta = std::min(deltaSeconds, maxFrameDelta);
    const float step = clampFadeSpeed(fadeSpeed) * fadeRateScale * delta;
    const float target = rising ? 1.0f : 0.0f;
    if (weight < target) return std::min(target, weight + step);
    if (weight > target) return std::max(target, weight - step);
    return target;
}

// ---------------------------------------------------------------------------
// Pose math (column-major 4x4, same memory layout as the game's Matrix)
// ---------------------------------------------------------------------------

struct Mat4 {
    // Identity. Index = column * 4 + row.
    std::array<float, 16> m{1.0f, 0.0f, 0.0f, 0.0f,
                            0.0f, 1.0f, 0.0f, 0.0f,
                            0.0f, 0.0f, 1.0f, 0.0f,
                            0.0f, 0.0f, 0.0f, 1.0f};

    const float* data() const { return m.data(); }
    float* data() { return m.data(); }
};

inline bool isFinite(const Mat4& matrix) {
    for (const float value : matrix.m) {
        if (!std::isfinite(value)) return false;
    }
    return true;
}

// a * b, so `multiply(parent, local)` applies `local` inside `parent` space.
inline Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 result;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[static_cast<std::size_t>(k * 4 + row)] *
                       b.m[static_cast<std::size_t>(column * 4 + k)];
            }
            result.m[static_cast<std::size_t>(column * 4 + row)] = sum;
        }
    }
    return result;
}

inline Mat4 translation(float x, float y, float z) {
    Mat4 result;
    result.m[12] = x;
    result.m[13] = y;
    result.m[14] = z;
    return result;
}

inline Mat4 scaling(float factor) {
    Mat4 result;
    result.m[0] = factor;
    result.m[5] = factor;
    result.m[10] = factor;
    return result;
}

inline Mat4 rotationXDegrees(float degrees) {
    if (!std::isfinite(degrees)) return Mat4{};
    const float radians = degrees * 3.14159265358979323846f / 180.0f;
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    Mat4 result;
    result.m[5] = c;
    result.m[6] = s;
    result.m[9] = -s;
    result.m[10] = c;
    return result;
}

inline Mat4 rotationZDegrees(float degrees) {
    if (!std::isfinite(degrees)) return Mat4{};
    const float radians = degrees * 3.14159265358979323846f / 180.0f;
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    Mat4 result;
    result.m[0] = c;
    result.m[1] = s;
    result.m[4] = -s;
    result.m[5] = c;
    return result;
}

// Applies `matrix` to a point (w = 1). Used by the tests and by callers that
// want to know where a pose puts the blade tip.
inline std::array<float, 3> transformPoint(const Mat4& matrix, float x, float y, float z) {
    return {matrix.m[0] * x + matrix.m[4] * y + matrix.m[8] * z + matrix.m[12],
            matrix.m[1] * x + matrix.m[5] * y + matrix.m[9] * z + matrix.m[13],
            matrix.m[2] * x + matrix.m[6] * y + matrix.m[10] * z + matrix.m[14]};
}

// Tunable pose. Distances are in Bedrock model units (16 per block), measured
// from the item's own origin, which is the grip.
struct PoseParams {
    float tiltDegrees = 65.0f; // how far the blade swings across the face
    float lift = 5.0f;         // moves the grip up toward the eye line
    float across = 4.0f;       // moves it toward the other side of the body
    float forward = 1.5f;      // moves it toward the camera
    float scale = 1.15f;       // slight enlargement so the guard reads
};

inline PoseParams clampedPose(PoseParams pose) {
    const auto finiteOr = [](float value, float fallback) {
        return std::isfinite(value) ? value : fallback;
    };
    pose.tiltDegrees = std::clamp(finiteOr(pose.tiltDegrees, 65.0f), 0.0f, 120.0f);
    pose.lift = std::clamp(finiteOr(pose.lift, 5.0f), -16.0f, 16.0f);
    pose.across = std::clamp(finiteOr(pose.across, 4.0f), -16.0f, 16.0f);
    pose.forward = std::clamp(finiteOr(pose.forward, 1.5f), -16.0f, 16.0f);
    pose.scale = std::clamp(finiteOr(pose.scale, 1.15f), 0.25f, 4.0f);
    return pose;
}

// Local transform for the held item at blend `weight`:
//
//   T(lift/across/forward * weight) * Rz(-tilt * weight) * Rx(-0.35 * tilt * weight) * S(...)
//
// The extra X rotation tips the blade toward the camera the way the Java
// blocking pose does, without needing a second user-facing slider. A weight of
// 0 returns the identity, so nothing is touched when the module is idle.
inline Mat4 poseMatrix(const PoseParams& params, float weight) {
    const float w = std::isfinite(weight) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
    if (w <= 0.0f) return Mat4{};

    const PoseParams pose = clampedPose(params);
    Mat4 result = translation(pose.across * w, pose.lift * w, pose.forward * w);
    result = multiply(result, rotationZDegrees(-pose.tiltDegrees * w));
    result = multiply(result, rotationXDegrees(-pose.tiltDegrees * 0.35f * w));
    const float factor = 1.0f + (pose.scale - 1.0f) * w;
    result = multiply(result, scaling(factor));
    return result;
}

// Composes `local` onto the live matrix in place: target = target * local.
// Returns false (leaving the matrix untouched) when either side is not a
// finite transform, so a malformed pose can never poison the game's matrix
// stack.
inline bool composeInto(float* target, const Mat4& local) {
    if (!target || !isFinite(local)) return false;
    Mat4 base;
    for (std::size_t i = 0; i < base.m.size(); ++i) base.m[i] = target[i];
    if (!isFinite(base)) return false;
    const Mat4 combined = multiply(base, local);
    if (!isFinite(combined)) return false;
    for (std::size_t i = 0; i < combined.m.size(); ++i) target[i] = combined.m[i];
    return true;
}

// ---------------------------------------------------------------------------
// Shield indicator
// ---------------------------------------------------------------------------

struct Line {
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    float thickness = 1.0f;
    std::uint32_t color = 0xFFFFFFFFu;
};

struct IndicatorStyle {
    float scale = 1.0f;    // 0.25 .. 4
    float opacity = 0.85f; // 0 .. 1
    std::uint32_t rgb = 0x7FE3FFu;
};

inline std::uint32_t withAlpha(std::uint32_t rgb, float opacity) {
    const float clamped = std::isfinite(opacity) ? std::clamp(opacity, 0.0f, 1.0f) : 0.0f;
    const auto alpha = static_cast<std::uint32_t>(clamped * 255.0f + 0.5f);
    return (alpha << 24) | (rgb & 0x00FFFFFFu);
}

// Shield outline drawn just below the crosshair: a flat top edge, two sides
// and a point, plus a dim inner accent. Coordinates are absolute HUD units;
// the caller passes the centre of the screen (the launcher's -20000 marker is
// centre-relative, so the module always stays centred on any resolution).
inline std::vector<Line> indicatorLines(float centerX, float centerY, float weight,
                                        const IndicatorStyle& style) {
    std::vector<Line> lines;
    const float w = std::isfinite(weight) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
    if (w <= activeThreshold) return lines;

    const float scale = std::isfinite(style.scale) ? std::clamp(style.scale, 0.25f, 4.0f) : 1.0f;
    const float alpha = (std::isfinite(style.opacity) ? std::clamp(style.opacity, 0.0f, 1.0f) : 0.0f) * w;
    const std::uint32_t color = withAlpha(style.rgb, alpha);
    if (((color >> 24) & 0xFFu) == 0u) return lines;

    const float halfWidth = 20.0f * scale;
    const float top = 16.0f * scale;      // below the crosshair
    const float side = 12.0f * scale;
    const float tip = 12.0f * scale;
    const float thickness = std::max(1.0f, 2.0f * scale);
    const float x = centerX;
    const float y = centerY;

    // Top edge.
    lines.push_back({x - halfWidth, y + top, x + halfWidth, y + top, thickness, color});
    // Left side + half of the point.
    lines.push_back({x - halfWidth, y + top, x - halfWidth, y + top + side, thickness, color});
    lines.push_back({x - halfWidth, y + top + side, x, y + top + side + tip, thickness, color});
    // Right side + half of the point.
    lines.push_back({x + halfWidth, y + top, x + halfWidth, y + top + side, thickness, color});
    lines.push_back({x + halfWidth, y + top + side, x, y + top + side + tip, thickness, color});
    // Inner accent, dimmer so the outline stays dominant.
    lines.push_back({x - halfWidth * 0.55f, y + top + side * 0.45f,
                     x + halfWidth * 0.55f, y + top + side * 0.45f,
                     std::max(1.0f, thickness * 0.75f),
                     withAlpha(style.rgb, alpha * 0.45f)});
    return lines;
}

} // namespace swordblocking

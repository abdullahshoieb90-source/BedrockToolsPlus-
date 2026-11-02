#pragma once

// Java Animations - the animation model, kept free of game dependencies.
//
// Everything in this header is plain math: it knows nothing about hooks,
// matrices, the SDK or JSON. src/modules/player/javaanimations.cpp feeds it
// wall-clock time and pushes the resulting pose into the first-person matrix
// stack, and tests/javaanimations_test.cpp drives the same code frame by frame
// on the host, so the curves are covered without a device.
//
// Conventions (degrees, Bedrock model space):
//
//   rotX > 0  lifts the hand/item towards the head (the vanilla attack
//             animations peak at a positive X rotation on the swinging arm)
//   rotY > 0  yaws the item towards the player's left
//   rotZ > 0  rolls the item counter-clockwise as seen by the player
//   posY > 0  moves the item up on screen
//   posZ > 0  pushes the item away from the camera
//
// What the Java 1.7 profile below is trying to reproduce, versus stock
// Bedrock:
//
//   * the held item hangs lower and flatter instead of being canted towards
//     the camera (Bedrock holds it high and tilted)
//   * one swing is a single large overhead arc rather than Bedrock's short
//     diagonal chop, and it lasts six ticks - the 1.7 attack animation length
//   * eating and drinking lifts the whole arm straight to the mouth and
//     nibbles, instead of tilting the item in place
//
// Java 1.8 is offered as a second profile: the same ideas with a shorter,
// more symmetric arc and a slightly raised rest pose.

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace javaanim {

enum class SwingStyle : int {
    Java17 = 0,
    Java18 = 1,
    Vanilla = 2
};

inline constexpr int kSwingStyleCount = 3;
inline constexpr const char* kSwingStyleIds[kSwingStyleCount] = {"Java 1.7", "Java 1.8", "Vanilla"};

// One game tick is 50 ms; the Java 1.7 swing animation runs for six of them.
inline constexpr float kTickSeconds = 0.05f;
inline constexpr int kDefaultSwingTicks = 6;
inline constexpr int kMinSwingTicks = 2;
inline constexpr int kMaxSwingTicks = 20;

// A use tick is only seen while the item is actually in use, so the raise is
// held for a few missed ticks before it decays (see UseState::advance).
inline constexpr float kUseHoldWindow = 0.2f;
inline constexpr float kRaiseTimeConstant = 0.08f;
inline constexpr float kLowerTimeConstant = 0.12f;

// Hand-relative offsets applied to the rendered item/arm.
struct HandPose {
    float posX = 0.0f;
    float posY = 0.0f;
    float posZ = 0.0f;
    float rotX = 0.0f;
    float rotY = 0.0f;
    float rotZ = 0.0f;
    float scale = 1.0f;
};

struct Settings {
    SwingStyle style = SwingStyle::Java17;
    bool itemPose = true;     // 1.7 held-item stance instead of Bedrock's
    bool useRaise = true;     // 1.7 eat/drink raise
    float swingScale = 1.0f;  // multiplier on the whole swing arc
};

inline float finiteOr(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
}

inline float clamp01(float value) {
    if (!std::isfinite(value)) return 0.0f;
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

inline float clampRange(float value, float low, float high) {
    if (!std::isfinite(value)) return low;
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// Classic smoothstep and its closed-form inverse. The inverse is what lets a
// swing be re-armed from the height it is already at (see SwingState::start),
// so attacking again mid-swing does not snap the hand back to the rest pose.
inline float smoothstep(float x) {
    const float t = clamp01(x);
    return t * t * (3.0f - 2.0f * t);
}

inline float inverseSmoothstep(float s) {
    const float v = clamp01(s);
    return 0.5f - std::sin(std::asin(1.0f - 2.0f * v) / 3.0f);
}

inline int clampSwingTicks(int ticks) {
    if (ticks < kMinSwingTicks) return kMinSwingTicks;
    if (ticks > kMaxSwingTicks) return kMaxSwingTicks;
    return ticks;
}

// ---------------------------------------------------------------------------
// Swing
// ---------------------------------------------------------------------------

// Where in the swing the arm is highest. Java 1.7 lifts quickly and chops down
// over the rest of the swing, Java 1.8 is the later, symmetric arc.
inline float swingPeak(SwingStyle style) {
    return style == SwingStyle::Java18 ? 0.50f : 0.35f;
}

// Shape of one swing: 0 at the start, 1 at the peak, 0 again at the end.
//
// Both halves are smoothsteps, so the arc leaves and re-enters the rest pose at
// zero velocity: the first and the last frame of a swing are almost stationary
// instead of snapping, which is what made the old step-based pose pop.
inline float swingArc(float progress, SwingStyle style) {
    const float p = clamp01(progress);
    if (style == SwingStyle::Vanilla) return 0.0f;
    if (p <= 0.0f || p >= 1.0f) return 0.0f;
    const float peak = swingPeak(style);
    if (p < peak) return smoothstep(p / peak);
    const float falling = (p - peak) / (1.0f - peak);
    return smoothstep(1.0f - falling);
}

// A swing is a one-shot timer: `start` arms it with the duration the game
// reported for that swing, `advance` walks it forward and retires it as soon
// as it is over, and `progress` reads 0..1 while it runs (0 when idle).
struct SwingState {
    float elapsed = 0.0f;  // seconds since the swing started
    int ticks = kDefaultSwingTicks;
    bool active = false;
    SwingStyle style = SwingStyle::Java17;  // shape of the swing in flight

    // Re-arming a running swing keeps the height it is already at: the new arc
    // resumes from the same amplitude on its rising branch instead of dropping
    // back to zero, which is what makes attack-spam read as one continuous
    // motion instead of a stutter.
    void start(int durationTicks, SwingStyle swingStyle) {
        const float carry = active ? swingArc(progress(), style) : 0.0f;
        style = swingStyle;
        ticks = clampSwingTicks(durationTicks);
        if (carry > 0.0f) {
            const float resumed = swingPeak(style) * inverseSmoothstep(carry);
            elapsed = resumed * durationSeconds();
        } else {
            elapsed = 0.0f;
        }
        active = true;
    }

    void advance(float dtSeconds) {
        if (!active) return;
        if (!(dtSeconds > 0.0f)) return;
        elapsed += dtSeconds;
        if (elapsed >= durationSeconds()) {
            active = false;
            elapsed = 0.0f;
        }
    }

    void reset() {
        elapsed = 0.0f;
        ticks = kDefaultSwingTicks;
        active = false;
    }

    float durationSeconds() const {
        return static_cast<float>(clampSwingTicks(ticks)) * kTickSeconds;
    }

    float progress() const {
        if (!active) return 0.0f;
        const float duration = durationSeconds();
        if (!(duration > 0.0f)) return 0.0f;
        return clamp01(elapsed / duration);
    }
};

// Peak values of one swing, per style.
struct SwingProfile {
    float liftDeg = 0.0f;  // peak rotation X (arm up)
    float rollDeg = 0.0f;  // peak rotation Z
    float yawDeg = 0.0f;   // peak rotation Y
    float pushZ = 0.0f;    // peak forward push, blocks
    float liftY = 0.0f;    // peak vertical offset, blocks
};

inline SwingProfile swingProfile(SwingStyle style) {
    switch (style) {
        case SwingStyle::Java17:
            return SwingProfile{72.0f, -14.0f, -8.0f, 0.12f, 0.05f};
        case SwingStyle::Java18:
            return SwingProfile{55.0f, -10.0f, -6.0f, 0.08f, 0.03f};
        case SwingStyle::Vanilla:
        default:
            return SwingProfile{};
    }
}

// ---------------------------------------------------------------------------
// Use (eat / drink)
// ---------------------------------------------------------------------------

// The game calls GameMode::useItem every tick while an item is in use, so a
// use is a heartbeat rather than an edge: `noteUse` refreshes it and
// `advance` decays the raise once the heartbeat stops. The raise itself eases
// towards its target with a real time constant, so it is frame-rate
// independent and never pops.
struct UseState {
    float raise = 0.0f;      // eased 0..1
    float sinceUse = 1.0e6f; // seconds since the last use heartbeat
    float held = 0.0f;       // seconds the item has been in use (nibble clock)

    void noteUse() { sinceUse = 0.0f; }

    void advance(float dtSeconds, bool enabled) {
        if (!(dtSeconds > 0.0f)) return;
        sinceUse += dtSeconds;

        const bool inUse = enabled && sinceUse < kUseHoldWindow;
        const float target = inUse ? 1.0f : 0.0f;
        const float tau = target > raise ? kRaiseTimeConstant : kLowerTimeConstant;
        raise += (target - raise) * (1.0f - std::exp(-dtSeconds / tau));

        if (inUse) {
            held += dtSeconds;
        } else {
            held = 0.0f;
        }
        if (raise < 0.001f && !inUse) raise = 0.0f;
        if (raise > 0.999f && inUse) raise = 1.0f;
    }

    void reset() {
        raise = 0.0f;
        sinceUse = 1.0e6f;
        held = 0.0f;
    }

    bool inUse() const { return sinceUse < kUseHoldWindow; }
};

// ---------------------------------------------------------------------------
// Poses
// ---------------------------------------------------------------------------

// Java 1.7 rest stance: the item hangs lower and flatter than Bedrock's.
inline HandPose restPose(const Settings& settings) {
    HandPose pose{};
    if (!settings.itemPose || settings.style == SwingStyle::Vanilla) return pose;
    const bool java18 = settings.style == SwingStyle::Java18;
    pose.posY = java18 ? -0.06f : -0.10f;
    pose.posZ = java18 ? 0.03f : 0.05f;
    pose.rotX = java18 ? -8.0f : -12.0f;
    pose.rotZ = java18 ? -4.0f : -6.0f;
    return pose;
}

inline HandPose swingPose(const Settings& settings, float progress, SwingStyle shape) {
    HandPose pose{};
    if (settings.style == SwingStyle::Vanilla) return pose;
    if (shape == SwingStyle::Vanilla) return pose;
    const float arc = swingArc(progress, shape);
    if (arc <= 0.0f) return pose;
    const float scale = clampRange(settings.swingScale, 0.0f, 5.0f);
    const SwingProfile profile = swingProfile(shape);
    pose.rotX = profile.liftDeg * arc * scale;
    pose.rotZ = profile.rollDeg * arc * scale;
    pose.rotY = profile.yawDeg * arc * scale;
    pose.posZ = profile.pushZ * arc * scale;
    pose.posY = profile.liftY * arc * scale;
    return pose;
}

inline HandPose usePose(const Settings& settings, const UseState& use) {
    HandPose pose{};
    if (!settings.useRaise || settings.style == SwingStyle::Vanilla) return pose;
    if (use.raise <= 0.0f) return pose;
    const float raise = clamp01(use.raise);
    pose.rotX = 34.0f * raise;
    pose.posY = 0.12f * raise;
    pose.posZ = -0.10f * raise;
    // Java 1.7 nibbles: a small bite-sized oscillation while the item is up.
    pose.rotX += std::sin(use.held * 18.0f) * 3.5f * raise;
    return pose;
}

inline HandPose addPose(const HandPose& left, const HandPose& right) {
    HandPose pose;
    pose.posX = left.posX + right.posX;
    pose.posY = left.posY + right.posY;
    pose.posZ = left.posZ + right.posZ;
    pose.rotX = left.rotX + right.rotX;
    pose.rotY = left.rotY + right.rotY;
    pose.rotZ = left.rotZ + right.rotZ;
    pose.scale = left.scale * right.scale;
    return pose;
}

// The left hand mirrors the right one around the vertical axis.
inline HandPose mirrored(const HandPose& pose) {
    HandPose out = pose;
    out.posX = -pose.posX;
    out.rotY = -pose.rotY;
    out.rotZ = -pose.rotZ;
    return out;
}

inline HandPose sanitizePose(const HandPose& pose) {
    HandPose out;
    out.posX = clampRange(finiteOr(pose.posX, 0.0f), -2.0f, 2.0f);
    out.posY = clampRange(finiteOr(pose.posY, 0.0f), -2.0f, 2.0f);
    out.posZ = clampRange(finiteOr(pose.posZ, 0.0f), -2.0f, 2.0f);
    out.rotX = clampRange(finiteOr(pose.rotX, 0.0f), -180.0f, 180.0f);
    out.rotY = clampRange(finiteOr(pose.rotY, 0.0f), -180.0f, 180.0f);
    out.rotZ = clampRange(finiteOr(pose.rotZ, 0.0f), -180.0f, 180.0f);
    out.scale = clampRange(finiteOr(pose.scale, 1.0f), 0.05f, 4.0f);
    return out;
}

inline bool isNeutral(const HandPose& pose) {
    return std::fabs(pose.posX) < 0.0005f && std::fabs(pose.posY) < 0.0005f &&
           std::fabs(pose.posZ) < 0.0005f && std::fabs(pose.rotX) < 0.05f &&
           std::fabs(pose.rotY) < 0.05f && std::fabs(pose.rotZ) < 0.05f &&
           std::fabs(pose.scale - 1.0f) < 0.0005f;
}

// The pose that should be on screen right now: the rest stance, plus whatever
// the swing and the use animation contribute, mirrored for the offhand.
inline HandPose currentPose(const Settings& settings, const SwingState& swing,
                            const UseState& use, bool mainHand) {
    HandPose pose = restPose(settings);
    pose = addPose(pose, swingPose(settings, swing.progress(), swing.style));
    pose = addPose(pose, usePose(settings, use));
    if (!mainHand) pose = mirrored(pose);
    return sanitizePose(pose);
}

// ---------------------------------------------------------------------------
// Serialization helpers (launcher radio format: "<selectedIndex>,<id>,...")
// ---------------------------------------------------------------------------

inline int styleIndexForId(const std::string_view id) {
    for (int i = 0; i < kSwingStyleCount; ++i) {
        if (id == kSwingStyleIds[i]) return i;
    }
    return 0;
}

inline std::string styleRadioValue(int index) {
    if (index < 0 || index >= kSwingStyleCount) index = 0;
    std::string value = std::to_string(index);
    for (int i = 0; i < kSwingStyleCount; ++i) {
        value += ',';
        value += kSwingStyleIds[i];
    }
    return value;
}

inline int resolveStyleIndex(const std::string_view value) {
    if (value.empty()) return 0;
    const std::size_t comma = value.find(',');
    const std::string_view head = value.substr(0, comma);
    bool numeric = !head.empty();
    for (char c : head) {
        if (c < '0' || c > '9') {
            numeric = false;
            break;
        }
    }
    if (numeric) {
        int parsed = 0;
        for (char c : head) parsed = parsed * 10 + (c - '0');
        if (parsed >= 0 && parsed < kSwingStyleCount) return parsed;
        return 0;
    }
    return styleIndexForId(head);
}

inline SwingStyle styleForIndex(int index) {
    switch (index) {
        case 1: return SwingStyle::Java18;
        case 2: return SwingStyle::Vanilla;
        default: return SwingStyle::Java17;
    }
}

}  // namespace javaanim

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace damagecircle {

constexpr float defaultDuration = 0.65f;
constexpr float minDuration = 0.1f;
constexpr float maxDuration = 10.0f;
constexpr float minSize = 0.5f;
constexpr float maxSize = 2.5f;
constexpr float defaultSize = 1.0f;
constexpr int segments = 120;
constexpr int bands = 4;
constexpr float pi = 3.14159265358979323846f;
constexpr float outerBandOffset = 0.035f;
// Jump Circle's maximum world-space radius is 1.08 blocks; use a fixed radius
// just above that so the damage ring reads slightly larger without pulsing.
constexpr float fixedOuterRadius = 1.15f;

inline float safeDuration(float seconds) {
    return std::isfinite(seconds) ? std::clamp(seconds, minDuration, maxDuration) : defaultDuration;
}

inline float safeSize(float size) {
    return std::isfinite(size) ? std::clamp(size, minSize, maxSize) : defaultSize;
}

// Actor hurt-time counts down; a fresh hit bumps it upward. The first sample
// only establishes a baseline so enabling the module during an existing flash
// doesn't create a false damage effect.
struct HurtTimeTracker {
    int previous = -1;

    bool update(int current) {
        if (current < 0 || current > 100) {
            previous = -1;
            return false;
        }
        if (previous < 0) {
            previous = current;
            return false;
        }
        const bool damaged = current > previous;
        previous = current;
        return damaged;
    }

    void reset() { previous = -1; }
};

struct Point { float x, y, z; };
struct RingState { float radius, lift, alpha; };

// The damage ring stays at one fixed, user-adjustable radius slightly larger
// than Jump Circle by default. Its vertical path travels up and then back down.
inline RingState animate(float age, float duration, float playerHeight, float size = defaultSize) {
    const float t = std::clamp(age / safeDuration(duration), 0.0f, 1.0f);
    const float verticalPhase = t <= 0.5f ? t * 2.0f : (1.0f - t) * 2.0f;
    const float easedHeight = verticalPhase * verticalPhase * (3.0f - 2.0f * verticalPhase);
    return {fixedOuterRadius * safeSize(size) - outerBandOffset, playerHeight * easedHeight,
            (1.0f - t) * (1.0f - t)};
}

// Four shaded strips create a narrow white band. Its radius stays constant
// during a cycle; Effect Size controls the fixed radius.
template <class Emit>
void emitVerticalRing(Point feet, float playerHeight, float age, float duration,
                    float size, Emit emit) {
    const auto state = animate(age, duration, playerHeight, size);
    constexpr std::array<float, bands + 1> offsets{-0.025f, -0.012f, 0.012f, 0.025f, outerBandOffset};
    constexpr std::array<float, bands + 1> strengths{0.0f, 0.65f, 1.0f, 0.65f, 0.0f};
    for (int i = 0; i < segments; ++i) {
        const float a0 = 2.0f * pi * static_cast<float>(i) / segments;
        const float a1 = 2.0f * pi * static_cast<float>(i + 1) / segments;
        const Point direction0{std::cos(a0), 0.0f, std::sin(a0)};
        const Point direction1{std::cos(a1), 0.0f, std::sin(a1)};
        for (int band = 0; band < bands; ++band) {
            const float inner = std::max(0.0f, state.radius + offsets[band]);
            const float outer = state.radius + offsets[band + 1];
            const float alphaInner = strengths[band] * state.alpha;
            const float alphaOuter = strengths[band + 1] * state.alpha;
            emit(Point{feet.x + direction0.x * inner, feet.y + state.lift, feet.z + direction0.z * inner}, alphaInner);
            emit(Point{feet.x + direction1.x * inner, feet.y + state.lift, feet.z + direction1.z * inner}, alphaInner);
            emit(Point{feet.x + direction1.x * outer, feet.y + state.lift, feet.z + direction1.z * outer}, alphaOuter);
            emit(Point{feet.x + direction0.x * outer, feet.y + state.lift, feet.z + direction0.z * outer}, alphaOuter);
        }
    }
}

} // namespace damagecircle

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include "jumpcircle_geometry.hpp"

namespace damagecircle {

constexpr float defaultDuration = 0.65f;
constexpr float minDuration = 0.1f;
constexpr float maxDuration = 10.0f;
constexpr float minSize = 0.5f;
constexpr float maxSize = 2.5f;
constexpr float defaultSize = 1.0f;
constexpr float minSpeed = 0.25f;
constexpr float maxSpeed = 4.0f;
constexpr float defaultSpeed = 1.0f;
constexpr float defaultCycleDuration = 0.65f;
constexpr int segments = 120;
constexpr int bands = 4;
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

inline float safeSpeed(float speed) {
    return std::isfinite(speed) ? std::clamp(speed, minSpeed, maxSpeed) : defaultSpeed;
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

// Duration controls only how long the ring remains visible and fades. Speed
// controls the repeated up/down cycle independently; size stays fixed mid-cycle.
inline RingState animate(float age, float duration, float playerHeight,
                         float size = defaultSize, float speed = defaultSpeed) {
    const float life = std::clamp(age / safeDuration(duration), 0.0f, 1.0f);
    const float cycleDuration = defaultCycleDuration / safeSpeed(speed);
    const float cycle = std::fmod(std::max(0.0f, age), cycleDuration) / cycleDuration;
    const float verticalPhase = cycle <= 0.5f ? cycle * 2.0f : (1.0f - cycle) * 2.0f;
    const float easedHeight = verticalPhase * verticalPhase * (3.0f - 2.0f * verticalPhase);
    return {fixedOuterRadius * safeSize(size) - outerBandOffset, playerHeight * easedHeight,
            (1.0f - life) * (1.0f - life)};
}

// Four shaded strips create a narrow white band. Effect Size controls its
// fixed radius; Effect Speed controls vertical cycles, not its visible lifetime.
template <class Emit>
void emitVerticalRing(Point feet, float playerHeight, float age, float duration,
                      float size, float speed, jumpcircle::Style style, Emit emit) {
    const auto state = animate(age, duration, playerHeight, size, speed);
    constexpr std::array<float, bands + 1> offsets{-0.025f, -0.012f, 0.012f, 0.025f, outerBandOffset};
    constexpr std::array<float, bands + 1> strengths{0.0f, 0.65f, 1.0f, 0.65f, 0.0f};
    for (int i = 0; i < segments; ++i) {
        const auto contour0 = jumpcircle::contour(style, i);
        const auto contour1 = jumpcircle::contour(style, i + 1);
        const Point direction0{contour0.x, 0.0f, contour0.z};
        const Point direction1{contour1.x, 0.0f, contour1.z};
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

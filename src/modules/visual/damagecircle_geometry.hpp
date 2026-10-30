#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace damagecircle {

constexpr float defaultDuration = 0.65f;
constexpr float minDuration = 0.1f;
constexpr float maxDuration = 10.0f;
constexpr int segments = 120;
constexpr int bands = 4;
constexpr float pi = 3.14159265358979323846f;
constexpr float outerBandOffset = 0.035f;
constexpr float playerWidthScale = 1.35f;

inline float safeDuration(float seconds) {
    return std::isfinite(seconds) ? std::clamp(seconds, minDuration, maxDuration) : defaultDuration;
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

// The damage ring keeps a fixed width based on the player's measured hitbox;
// it is slightly wider for visibility. Only its height changes, moving from
// the feet to the top of the hitbox.
inline RingState animate(float age, float duration, float playerHalfWidth, float playerHeight) {
    const float t = std::clamp(age / safeDuration(duration), 0.0f, 1.0f);
    const float ease = t * t * (3.0f - 2.0f * t);
    const float outerRadius = std::max(0.08f, playerHalfWidth * playerWidthScale);
    return {std::max(0.04f, outerRadius - outerBandOffset), playerHeight * ease,
            (1.0f - t) * (1.0f - t)};
}

// Four shaded strips create a narrow white band. Its outside edge matches the
// scaled player width, so it does not expand like the Jump Circle effect.
template <class Emit>
void emitRisingRing(Point feet, float playerHalfWidth, float playerHeight,
                    float age, float duration, Emit emit) {
    const auto state = animate(age, duration, playerHalfWidth, playerHeight);
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

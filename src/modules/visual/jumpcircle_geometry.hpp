#pragma once

#include <algorithm>
#include <cmath>

// Pure animation and takeoff detection, independent of Minecraft's renderer.
namespace jumpcircle {
constexpr float duration = 0.65f;
constexpr int segments = 64;
constexpr int bands = 4;
constexpr float pi = 3.14159265358979323846f;

struct Point { float x, y, z; };
struct RingState { float radius, alpha; };

inline RingState animate(float age) {
    const float t = std::clamp(age / duration, 0.0f, 1.0f);
    const float ease = 1.0f - (1.0f - t) * (1.0f - t);
    return {0.38f + 0.70f * ease, (1.0f - t) * (1.0f - t)};
}

// The tick supplies feet positions and jump input. Requiring both a jump input
// and the start of upward motion avoids effects while merely holding the button
// against a ceiling or walking down a slope. Continuous ascent emits only once.
inline bool takeoff(float previousDy, float dy, bool jumpDown) {
    return jumpDown && previousDy <= 0.05f && dy > 0.16f && dy < 1.2f;
}

// Four concentric strips make a soft white halo without a texture. Vertex
// colours fall off at the inner/outer edges and interpolate across each quad.
template <class Emit>
void emitRing(Point center, float age, Emit emit) {
    const auto state = animate(age);
    constexpr float offsets[] = {-0.13f, -0.045f, 0.0f, 0.045f, 0.13f};
    constexpr float strengths[] = {0.0f, 0.30f, 0.90f, 0.30f, 0.0f};
    auto vertex = [&](float radius, float angle, float alpha) {
        emit(Point{center.x + std::cos(angle) * radius, center.y,
                   center.z + std::sin(angle) * radius}, alpha * state.alpha);
    };
    for (int i = 0; i < segments; ++i) {
        const float a = 2.0f * pi * i / segments;
        const float b = 2.0f * pi * (i + 1) / segments;
        for (int j = 0; j < bands; ++j) {
            const float inner = std::max(0.0f, state.radius + offsets[j]);
            const float outer = state.radius + offsets[j + 1];
            vertex(inner, a, strengths[j]);
            vertex(inner, b, strengths[j]);
            vertex(outer, b, strengths[j + 1]);
            vertex(outer, a, strengths[j + 1]);
        }
    }
}
}

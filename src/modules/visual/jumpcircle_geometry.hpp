#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

namespace jumpcircle {
constexpr float defaultDuration = 0.65f;
constexpr float minDuration = 0.1f;
constexpr float maxDuration = 10.0f;
constexpr std::size_t maxEffects = 24;
inline float safeDuration(float seconds) {
    return std::isfinite(seconds) ? std::clamp(seconds, minDuration, maxDuration) : defaultDuration;
}
// Divisible by four and ten, so square corners and all five star tips land on
// vertices. All styles use the same vertex budget.
constexpr int segments = 80;
constexpr int bands = 4;
constexpr float pi = 3.14159265358979323846f;
constexpr int styleCount = 3;
enum class Style { Circle, Square, Star };

inline int styleIndex(std::string_view value) {
    value = value.substr(0, value.find(','));
    if (value == "1" || value == "Square") return 1;
    if (value == "2" || value == "Star") return 2;
    return 0;
}
inline std::string styleRadioValue(int index) {
    if (index < 0 || index >= styleCount) index = 0;
    return std::to_string(index) + ",Circle,Square,Star";
}

struct Point { float x, y, z; };
struct RingState { float radius, alpha; };

inline RingState animate(float age, float duration, bool persistent) {
    const float t = std::clamp(age / safeDuration(duration), 0.0f, 1.0f);
    const float ease = 1.0f - (1.0f - t) * (1.0f - t);
    return {0.38f + 0.70f * ease, persistent ? 1.0f : (1.0f - t) * (1.0f - t)};
}

// Without a verified ground/jump event in this game's SDK, infer takeoff and
// landing from consecutive tick positions. No button input is required, so
// touch auto-jump and held jump work as well. Landings are armed only by a
// downward movement whether it started with a jump or walking off a block;
// pauses at the apex do not count. The step exclusions avoid common half/full block step-ups.
struct MotionEvents { bool takeoff = false, landing = false; };
struct MotionTracker {
    float previousDy = 0;
    bool airborne = false;
    bool descending = false;

    MotionEvents update(float dy) {
        MotionEvents result;
        if (!std::isfinite(dy) || std::abs(dy) > 4.0f) { *this = {}; return result; }
        if (airborne && descending && dy >= -0.01f) {
            result.landing = true;
            airborne = false;
            descending = false;
        }
        // Most jumps begin with ~0.42 blocks of upward displacement. A
        // half/full-block instantaneous step is not a jump.
        const bool step = std::abs(dy - 0.5f) < 0.015f || std::abs(dy - 1.0f) < 0.015f;
        if (!airborne && previousDy <= 0.05f && dy > 0.16f && dy < 1.2f && !step) {
            result.takeoff = true;
            airborne = true;
        }
        if (dy < -0.01f) { airborne = true; descending = true; }
        previousDy = dy;
        return result;
    }
};

// A closed normalized contour. Polygon vertices are interpolated directly so
// the square has straight sides and the star has crisp, connected tips.
inline Point contour(Style style, int segment) {
    if (style == Style::Circle) {
        const float angle = 2.0f * pi * segment / segments;
        return {std::cos(angle), 0, std::sin(angle)};
    }
    const int corners = style == Style::Star ? 10 : 4;
    const float position = static_cast<float>(segment) * corners / segments;
    const int first = static_cast<int>(position);
    const float fraction = position - first;
    auto corner = [style, corners](int index) {
        const float angle = 2.0f * pi * index / corners + (style == Style::Star ? pi / 2.0f : pi / 4.0f);
        const float radius = style == Style::Star && (index % 2) ? 0.48f : 1.0f;
        return Point{std::cos(angle) * radius, 0, std::sin(angle) * radius};
    };
    const Point a = corner(first), b = corner(first + 1);
    return {a.x + (b.x - a.x) * fraction, 0, a.z + (b.z - a.z) * fraction};
}

// Four concentric strips produce the soft halo without a texture.
template <class Emit>
void emitRing(Point center, float age, float duration, bool persistent, Style style, Emit emit) {
    const auto state = animate(age, duration, persistent);
    constexpr float offsets[] = {-0.13f, -0.045f, 0.0f, 0.045f, 0.13f};
    constexpr float strengths[] = {0.0f, 0.30f, 0.90f, 0.30f, 0.0f};
    auto vertex = [&](Point direction, float radius, float alpha) {
        emit(Point{center.x + direction.x * radius, center.y,
                   center.z + direction.z * radius}, alpha * state.alpha);
    };
    for (int i = 0; i < segments; ++i) {
        const Point a = contour(style, i), b = contour(style, i + 1);
        for (int j = 0; j < bands; ++j) {
            const float inner = std::max(0.0f, state.radius + offsets[j]);
            const float outer = state.radius + offsets[j + 1];
            vertex(a, inner, strengths[j]);
            vertex(b, inner, strengths[j]);
            vertex(b, outer, strengths[j + 1]);
            vertex(a, outer, strengths[j + 1]);
        }
    }
}
}

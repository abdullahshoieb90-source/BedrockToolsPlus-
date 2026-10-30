#pragma once

#include <algorithm>
#include <array>
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
// A multiple of every polygon's vertex count (including the gear), preserving
// exact corners and tips while keeping the same vertex budget for each style.
constexpr int segments = 120;
constexpr int bands = 4;
constexpr float pi = 3.14159265358979323846f;
// Append new styles only: saved radio indices 0, 1 and 2 must stay compatible.
enum class Style {
    Circle, Square, Star, Triangle, Diamond, Pentagon, Hexagon, Octagon,
    FourPointStar, SixPointStar, Heart, Flower, Gear
};
constexpr std::array<std::string_view, 13> styleNames = {
    "Circle", "Square", "Star", "Triangle", "Diamond", "Pentagon", "Hexagon", "Octagon",
    "Four Point Star", "Six Point Star", "Heart", "Flower", "Gear"
};
constexpr int styleCount = static_cast<int>(styleNames.size());

inline int styleIndex(std::string_view value) {
    value = value.substr(0, value.find(','));
    for (int i = 0; i < styleCount; ++i) {
        if (value == styleNames[i] || value == std::to_string(i)) return i;
    }
    return 0;
}
inline std::string styleRadioValue(int index) {
    if (index < 0 || index >= styleCount) index = 0;
    std::string value = std::to_string(index);
    for (const auto name : styleNames) {
        value += ',';
        value += name;
    }
    return value;
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

// Closed, counter-clockwise contours. Polygon edges are interpolated directly
// so corners remain crisp; heart and flower use smooth parametric curves.
inline Point contour(Style style, int segment) {
    const float angle = 2.0f * pi * segment / segments;
    if (style == Style::Circle) return {std::cos(angle), 0, std::sin(angle)};
    if (style == Style::Heart) {
        const float sn = std::sin(angle);
        const float x = -16.0f * sn * sn * sn / 18.0f;
        const float z = (13.0f * std::cos(angle) - 5.0f * std::cos(2.0f * angle)
                       - 2.0f * std::cos(3.0f * angle) - std::cos(4.0f * angle) + 2.5f) / 18.0f;
        return {x, 0, z};
    }
    if (style == Style::Flower) {
        const float r = 0.77f + 0.23f * std::cos(6.0f * angle);
        return {r * std::cos(angle), 0, r * std::sin(angle)};
    }
    int corners = 4;
    float rotation = pi / 4.0f;
    float innerRadius = 1.0f;
    switch (style) {
        case Style::Square: break;
        case Style::Star: corners = 10; rotation = pi / 2.0f; innerRadius = 0.48f; break;
        case Style::Triangle: corners = 3; rotation = pi / 2.0f; break;
        case Style::Diamond: rotation = 0.0f; break;
        case Style::Pentagon: corners = 5; rotation = pi / 2.0f; break;
        case Style::Hexagon: corners = 6; rotation = 0.0f; break;
        case Style::Octagon: corners = 8; rotation = pi / 8.0f; break;
        case Style::FourPointStar: corners = 8; rotation = 0.0f; innerRadius = 0.42f; break;
        case Style::SixPointStar: corners = 12; rotation = pi / 2.0f; innerRadius = 0.55f; break;
        case Style::Gear: corners = 24; rotation = 0.0f; break;
        default: break;
    }
    const float position = static_cast<float>(segment) * corners / segments;
    const int first = static_cast<int>(position);
    const float fraction = position - first;
    auto corner = [style, corners, rotation, innerRadius](int index) {
        const float theta = 2.0f * pi * index / corners + rotation;
        // A gear tooth has two outer vertices and two inner vertices.
        const float radius = style == Style::Gear ? ((index % 4 < 2) ? 1.0f : 0.72f)
                          : ((index % 2) ? innerRadius : 1.0f);
        return Point{std::cos(theta) * radius, 0, std::sin(theta) * radius};
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

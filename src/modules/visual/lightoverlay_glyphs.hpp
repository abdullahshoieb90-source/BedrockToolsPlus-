#pragma once

#include <bedrocktools/sdk/Types.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

// Pure drawing data for the Light Overlay module: the stroke font the light
// levels are written with, and the six face orientations a block can be
// labelled on. Neither touches Minecraft, so both are host-testable.
namespace lightoverlay {

using bedrocktools::sdk::BlockPos;
using bedrocktools::sdk::Vec2;
using bedrocktools::sdk::Vec3;

// Bedrock light levels run from 0 (dark) to 15 (full sunlight).
inline constexpr int kMaxLightLevel = 15;

// The scan radius is bounded so a stray config value cannot turn the overlay
// into a whole-dimension scan: the cost grows with the cube of the radius.
inline constexpr int kMinRadius = 0;
inline constexpr int kMaxRadius = 32;

inline constexpr int clampRadius(int value) {
    return value < kMinRadius ? kMinRadius : (value > kMaxRadius ? kMaxRadius : value);
}

// getBrightness() returns a 0..1 fraction of the light scale.
inline constexpr int lightLevelFromBrightness(float brightness) {
    return static_cast<int>(std::lround(brightness * static_cast<float>(kMaxLightLevel)));
}

// At or below the threshold the light is low enough for hostile mobs to spawn.
inline constexpr bool isDangerous(int lightLevel, int dangerThreshold) {
    return lightLevel <= dangerThreshold;
}

// Glyph cell, measured in units of the face's own basis vectors.
inline constexpr float kGlyphHalfWidth = 0.3f;
inline constexpr float kGlyphHalfHeight = 0.4f;
// Two-digit numbers push each digit this far from the centre of the face.
inline constexpr float kDigitSpacing = 0.45f;
// Overall size of the label on a block face.
inline constexpr float kLabelScale = 0.4f;

struct Stroke {
    Vec2 from;
    Vec2 to;
};

inline constexpr Stroke stroke(float x1, float y1, float x2, float y2) {
    return Stroke{{x1, y1}, {x2, y2}};
}

inline constexpr std::size_t kMaxStrokes = 5;

struct Glyph {
    std::array<Stroke, kMaxStrokes> strokes{};
    std::size_t count = 0;
};

// Stroke font for the digits 0-9. Every digit is drawn as a handful of line
// segments because the overlay renders with the Tessellator, which has no text
// path of its own. Anything outside 0-9 draws nothing.
inline constexpr Glyph digitGlyph(int digit) {
    Glyph glyph{};
    const float w = kGlyphHalfWidth;
    const float h = kGlyphHalfHeight;
    const auto add = [&glyph](float x1, float y1, float x2, float y2) {
        glyph.strokes[glyph.count] = stroke(x1, y1, x2, y2);
        ++glyph.count;
    };

    switch (digit) {
        case 0:
            add(-w, -h, w, -h);
            add(w, -h, w, h);
            add(w, h, -w, h);
            add(-w, h, -w, -h);
            add(-w, -h, w, h);
            break;
        case 1:
            add(0.0f, -h, 0.0f, h);
            add(-w, -h, w, -h);
            add(0.0f, h, -w, 0.5f * h);
            break;
        case 2:
            add(-w, h, w, h);
            add(w, h, w, 0.0f);
            add(w, 0.0f, -w, 0.0f);
            add(-w, 0.0f, -w, -h);
            add(-w, -h, w, -h);
            break;
        case 3:
            add(-w, h, w, h);
            add(w, h, w, -h);
            add(-w, -h, w, -h);
            add(-w, 0.0f, w, 0.0f);
            break;
        case 4:
            add(-w, h, -w, 0.0f);
            add(-w, 0.0f, w, 0.0f);
            add(w, h, w, -h);
            break;
        case 5:
            add(w, h, -w, h);
            add(-w, h, -w, 0.0f);
            add(-w, 0.0f, w, 0.0f);
            add(w, 0.0f, w, -h);
            add(w, -h, -w, -h);
            break;
        case 6:
            add(w, h, -w, h);
            add(-w, h, -w, -h);
            add(-w, -h, w, -h);
            add(w, -h, w, 0.0f);
            add(w, 0.0f, -w, 0.0f);
            break;
        case 7:
            add(-w, h, w, h);
            add(w, h, -w, -h);
            break;
        case 8:
            add(-w, -h, w, -h);
            add(w, -h, w, h);
            add(w, h, -w, h);
            add(-w, h, -w, -h);
            add(-w, 0.0f, w, 0.0f);
            break;
        case 9:
            add(w, -h, w, h);
            add(w, h, -w, h);
            add(-w, h, -w, 0.0f);
            add(-w, 0.0f, w, 0.0f);
            add(-w, -h, w, -h);
            break;
        default:
            break;
    }
    return glyph;
}

inline Stroke shifted(const Stroke& source, float dx, float dy) {
    return stroke(source.from.x + dx, source.from.y + dy, source.to.x + dx, source.to.y + dy);
}

// Strokes of a whole light level (0..99) in glyph space. Two digits are pushed
// apart around the origin so the number stays centred on the face.
inline std::vector<Stroke> numberStrokes(int value) {
    const int clamped = value < 0 ? 0 : (value > 99 ? 99 : value);
    std::vector<Stroke> strokes;

    if (clamped < 10) {
        const Glyph glyph = digitGlyph(clamped);
        strokes.insert(strokes.end(), glyph.strokes.begin(), glyph.strokes.begin() +
                                                               static_cast<std::ptrdiff_t>(glyph.count));
        return strokes;
    }

    const Glyph tens = digitGlyph(clamped / 10);
    const Glyph ones = digitGlyph(clamped % 10);
    for (std::size_t i = 0; i < tens.count; ++i) {
        strokes.push_back(shifted(tens.strokes[i], -kDigitSpacing, 0.0f));
    }
    for (std::size_t i = 0; i < ones.count; ++i) {
        strokes.push_back(shifted(ones.strokes[i], kDigitSpacing, 0.0f));
    }
    return strokes;
}

// One face of a block: which neighbour's light is read, where the label sits,
// and the two basis vectors the label is drawn in. `right` and `up` are always
// perpendicular to the face normal, so the number lies flat on the face. The
// label centre is pushed 0.05 blocks out of the face to avoid z-fighting.
struct Face {
    BlockPos neighbour;
    Vec3 label;
    Vec3 right;
    Vec3 up;
};

inline constexpr std::array<Face, 6> kFaces = {{
    // up, down, east, west, south, north
    {{0, 1, 0}, {0.5f, 1.05f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
    {{0, -1, 0}, {0.5f, -0.05f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{1, 0, 0}, {1.05f, 0.5f, 0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},
    {{-1, 0, 0}, {-0.05f, 0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
    {{0, 0, 1}, {0.5f, 0.5f, 1.05f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
    {{0, 0, -1}, {0.5f, 0.5f, -0.05f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
}};

// The top face is the only one labelled when "only top face" is on.
inline constexpr std::size_t kTopFace = 0;

inline constexpr bool faceVisible(std::size_t index, bool onlyTopFace) {
    return !onlyTopFace || index == kTopFace;
}

inline Vec3 labelPosition(const BlockPos& block, const Vec3& offset) {
    return {static_cast<float>(block.x) + offset.x, static_cast<float>(block.y) + offset.y,
            static_cast<float>(block.z) + offset.z};
}

}  // namespace lightoverlay

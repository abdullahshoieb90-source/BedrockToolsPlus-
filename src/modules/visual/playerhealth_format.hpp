#pragma once

// ---------------------------------------------------------------------------
// Pure formatting helpers for the Player Health module: the second nametag
// line that is drawn under the player's name (hearts / bar / numbers), the
// style radio serialization and the max-health scaling.
//
// Everything here is dependency-free so it can be unit tested without the
// game (see tests/playerhealth_test.cpp).
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace bedrocktools::visual::playerhealth {

enum Style : int {
    StyleHearts = 0,
    StyleBar = 1,
    StyleNumbers = 2,
};

inline constexpr int kStyleCount = 3;
inline const char* const kStyleIds[kStyleCount] = {"hearts", "bar", "numbers"};

// Fixed amount of glyphs used by the hearts and bar styles. Ten keeps the
// second nametag line compact even for boosted health pools.
inline constexpr int kGlyphCount = 10;

// Bedrock formats chat/nametag text with a section sign, which is two bytes
// ("\xC2\xA7") in UTF-8, followed by a one byte formatting code.
inline std::string cc(char code) {
    std::string out;
    out.push_back(static_cast<char>(0xC2));
    out.push_back(static_cast<char>(0xA7));
    out.push_back(code);
    return out;
}

// U+2764 HEAVY BLACK HEART, the glyph every Bedrock server font renders.
inline constexpr const char* kHeart = "\xE2\x9D\xA4";

inline float clampRatio(float ratio) {
    if (std::isnan(ratio)) return 0.0f;
    return std::clamp(ratio, 0.0f, 1.0f);
}

// Green while healthy, yellow below half, red below a quarter, dark gray
// once dead. Same thresholds the Effect Display countdown uses.
inline char ratioColorCode(float ratio) {
    if (ratio > 0.5f) return 'a';
    if (ratio > 0.25f) return 'e';
    if (ratio > 0.0f) return 'c';
    return '8';
}

// "§c❤❤❤§8❤❤❤❤❤❤❤" — filled hearts in red, remaining hearts dark gray.
// Health pools above the vanilla 20 points are squeezed onto the same ten
// glyphs (each heart then covers maxHp / 10 points), so boosted players
// (absorption, health boost) keep a readable one-line indicator.
inline std::string composeHearts(float health, float maxHealth) {
    if (!(maxHealth > 0.0f)) maxHealth = 20.0f;
    const float ratio = clampRatio(health / maxHealth);

    int filled = static_cast<int>(std::lround(ratio * kGlyphCount));
    // Any positive health shows at least one filled heart, and rounding can
    // never push a dead player above zero.
    if (health > 0.0f) filled = std::max(filled, 1);
    filled = std::clamp(filled, 0, kGlyphCount);

    std::string out;
    if (filled > 0) {
        out = cc('c');
        for (int i = 0; i < filled; ++i) out += kHeart;
    }
    if (filled < kGlyphCount) {
        out += cc('8');
        for (int i = filled; i < kGlyphCount; ++i) out += kHeart;
    }
    return out;
}

// "§a||||||||§7||" — a segmented bar, green for the filled part, light gray
// for the missing part.
inline std::string composeBar(float health, float maxHealth, int segments = kGlyphCount) {
    if (!(maxHealth > 0.0f)) maxHealth = 20.0f;
    if (segments < 1) segments = 1;
    const float ratio = clampRatio(health / maxHealth);

    int filled = static_cast<int>(std::lround(ratio * segments));
    if (health > 0.0f) filled = std::max(filled, 1);
    filled = std::clamp(filled, 0, segments);

    std::string out;
    if (filled > 0) {
        out = cc('a');
        out.append(static_cast<std::size_t>(filled), '|');
    }
    if (filled < segments) {
        out += cc('7');
        out.append(static_cast<std::size_t>(segments - filled), '|');
    }
    return out;
}

// "§a❤ 17/20" — current over max, colored by the same ratio thresholds.
inline std::string composeNumbers(float health, float maxHealth) {
    if (!(maxHealth > 0.0f)) maxHealth = 20.0f;
    const float ratio = clampRatio(health / maxHealth);

    const int shown = static_cast<int>(std::ceil(std::max(health, 0.0f)));
    const int shownMax = static_cast<int>(std::ceil(maxHealth));

    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%s%s %d/%d", cc(ratioColorCode(ratio)).c_str(), kHeart, shown, shownMax);
    return buffer;
}

inline std::string composeHealthLine(int style, float health, float maxHealth) {
    switch (style) {
        case StyleBar: return composeBar(health, maxHealth);
        case StyleNumbers: return composeNumbers(health, maxHealth);
        case StyleHearts:
        default: return composeHearts(health, maxHealth);
    }
}

// Serializes the picker value in the launcher's radio format:
// "<selectedIndex>,<id1>,<id2>,..." (same convention as Wings / Custom Capes).
inline std::string styleRadioValue(int index) {
    if (index < 0 || index >= kStyleCount) index = 0;
    std::string value = std::to_string(index);
    for (int i = 0; i < kStyleCount; ++i) {
        value += ',';
        value += kStyleIds[i];
    }
    return value;
}

// Parses a value coming from the config file (full radio value), from the
// launcher (just the numeric index) or a bare style id.
inline int resolveStyleIndex(const std::string& value) {
    if (value.empty()) return 0;

    const std::size_t comma = value.find(',');
    const std::string head = value.substr(0, comma);

    bool numeric = !head.empty();
    for (char c : head) {
        if (c < '0' || c > '9') {
            numeric = false;
            break;
        }
    }
    if (numeric) {
        const int index = std::atoi(head.c_str());
        return (index >= 0 && index < kStyleCount) ? index : 0;
    }

    for (int i = 0; i < kStyleCount; ++i) {
        if (head == kStyleIds[i]) return i;
    }
    if (comma == std::string::npos) {
        for (int i = 0; i < kStyleCount; ++i) {
            if (value == kStyleIds[i]) return i;
        }
    }
    return 0;
}

}  // namespace bedrocktools::visual::playerhealth

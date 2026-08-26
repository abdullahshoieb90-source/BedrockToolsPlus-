#pragma once

// Selectable wing styles for the Wings module.
//
// Each style is a right-side bone table (the wing spans towards -x from the
// shoulder pivot at [-3, 21]); the left-side table is generated at runtime by
// mirroring it, so the two sides can never drift apart. The bone layout -
// pivots, anchors and box sizes - mirrors the hierarchy embedded in
// wings_default::GeometryJson (resources/wings/wings_geometry.json) one to
// one. Three shape details are overlay-only because Bedrock's box geometry
// cannot express them: the rest-pose fan (restDeg), the backwards sweep
// (sweepPx) and the tapered far edge (taperPx). They are documented on
// WingBone in wings_shape.hpp.
//
// Kept in a header (like blockoutline_geometry.hpp) so the host tests and
// tools/wings_preview.cpp build the exact same wings the game draws.

#include "modules/visual/wings_shape.hpp"

#include <bedrocktools/modules/visual/wings_default.hpp>

#include <cctype>
#include <cstddef>
#include <string>

namespace bedrocktools::modules::wings {

// ---------------------------------------------------------------------------
// Palettes
// ---------------------------------------------------------------------------

// ---- Dragon (default) reuses the generated texture palette so the overlay
// keeps matching wings.png / wings_geometry.json. ----
// (wings_default::kColorFrame / kColorMembraneOuter / ... )

// ---- Angel: white feathers with soft gold tips ----
inline constexpr unsigned char kAngelFrame[3]         = {244, 240, 232};
inline constexpr unsigned char kAngelMembraneOuter[3] = {252, 250, 246};
inline constexpr unsigned char kAngelMembraneInner[3] = {226, 222, 210};
inline constexpr unsigned char kAngelFeatherTip[3]    = {255, 236, 190};
inline constexpr unsigned char kAngelJointInner[3]    = {214, 205, 188};

// ---- Demon: deep red membrane on a near-black frame ----
inline constexpr unsigned char kDemonFrame[3]         = {42, 12, 12};
inline constexpr unsigned char kDemonMembraneOuter[3] = {96, 16, 22};
inline constexpr unsigned char kDemonMembraneInner[3] = {150, 26, 32};
inline constexpr unsigned char kDemonFeatherTip[3]    = {208, 44, 48};
inline constexpr unsigned char kDemonJointInner[3]    = {64, 14, 16};

// ---- Bat: small, very dark membrane ----
inline constexpr unsigned char kBatFrame[3]         = {34, 34, 40};
inline constexpr unsigned char kBatMembraneOuter[3] = {14, 14, 18};
inline constexpr unsigned char kBatMembraneInner[3] = {26, 26, 32};
inline constexpr unsigned char kBatFeatherTip[3]    = {20, 20, 26};
inline constexpr unsigned char kBatJointInner[3]    = {24, 24, 30};

// ---- Butterfly: pink/orange panels with blue accents ----
inline constexpr unsigned char kButterflyFrame[3]         = {96, 44, 148};
inline constexpr unsigned char kButterflyMembraneOuter[3] = {250, 118, 178};
inline constexpr unsigned char kButterflyMembraneInner[3] = {255, 196, 92};
inline constexpr unsigned char kButterflyFeatherTip[3]    = {118, 200, 255};
inline constexpr unsigned char kButterflyJointInner[3]    = {150, 82, 192};

// ---- Phoenix: fiery orange/red with gold highlights ----
inline constexpr unsigned char kPhoenixFrame[3]         = {176, 92, 22};
inline constexpr unsigned char kPhoenixMembraneOuter[3] = {255, 62, 22};
inline constexpr unsigned char kPhoenixMembraneInner[3] = {255, 168, 30};
inline constexpr unsigned char kPhoenixFeatherTip[3]    = {255, 232, 120};
inline constexpr unsigned char kPhoenixJointInner[3]    = {140, 60, 16};

// ---- Fairy: small translucent cyan/pink wings ----
inline constexpr unsigned char kFairyFrame[3]         = {120, 222, 210};
inline constexpr unsigned char kFairyMembraneOuter[3] = {186, 240, 255};
inline constexpr unsigned char kFairyMembraneInner[3] = {244, 202, 255};
inline constexpr unsigned char kFairyFeatherTip[3]    = {255, 255, 242};
inline constexpr unsigned char kFairyJointInner[3]    = {140, 232, 222};

// ---------------------------------------------------------------------------
// Bone tables. Column order matches WingBone:
//
//   parent, anchor(x,y), box origin(x,y), box size(x,y), z(min,max),
//   restDeg, sweepPx, taperPx, spanT, tint,
//   color(outer, inner, edge, bottom), angleIndex
//
// Bones: 0 shoulder joint, 1 upper arm, 2 wing tip / wrist, 3..6 feathers.
// z stays >= 2.5 (the torso back surface is z = +2) so nothing clips the body.
// ---------------------------------------------------------------------------

// Dragon: articulated membrane wing. The fingers fan open and curl back so
// the membrane reads as a wing surface instead of a flat comb.
inline constexpr WingBone kDragonRightBones[7] = {
    {-1,  0.0f,  0.0f, -1.5f, -1.5f, 3.0f, 3.0f, 2.5f, 4.5f,   0.0f, 0.0f, 0.0f, 0.00f, 1.00f, wings_default::kColorMembraneOuter, wings_default::kColorJointInner,    wings_default::kColorFrame, wings_default::kColorFrame,      0},
    { 0, -2.0f,  0.0f, -6.0f, -1.5f, 6.0f, 3.0f, 3.0f, 4.0f,   0.0f, 0.5f, 1.0f, 0.30f, 1.00f, wings_default::kColorMembraneOuter, wings_default::kColorMembraneInner, wings_default::kColorFrame, wings_default::kColorFrame,      1},
    { 1, -6.0f,  0.0f, -5.0f, -1.0f, 5.0f, 2.0f, 3.0f, 4.0f,  -5.0f, 1.0f, 1.0f, 0.70f, 1.04f, wings_default::kColorMembraneOuter, wings_default::kColorMembraneInner, wings_default::kColorFrame, wings_default::kColorFrame,      2},
    { 1, -2.0f, -1.5f, -1.0f, -6.0f, 2.0f, 6.0f, 3.0f, 4.0f,   5.0f, 0.5f, 1.0f, 0.50f, 1.00f, wings_default::kColorMembraneOuter, wings_default::kColorMembraneInner, wings_default::kColorFrame, wings_default::kColorFeatherTip, 3},
    { 1, -4.5f, -1.5f, -1.0f, -6.0f, 2.0f, 6.0f, 3.0f, 4.0f,  11.0f, 1.0f, 1.2f, 0.62f, 0.93f, wings_default::kColorMembraneOuter, wings_default::kColorMembraneInner, wings_default::kColorFrame, wings_default::kColorFeatherTip, 4},
    { 2, -1.5f, -1.0f, -1.0f, -6.0f, 2.0f, 6.0f, 3.0f, 4.0f,  16.0f, 1.5f, 1.2f, 0.85f, 1.00f, wings_default::kColorMembraneOuter, wings_default::kColorMembraneInner, wings_default::kColorFrame, wings_default::kColorFeatherTip, 5},
    { 2, -4.0f, -1.0f, -1.0f, -5.0f, 2.0f, 5.0f, 3.0f, 4.0f,  22.0f, 2.0f, 1.4f, 1.00f, 0.93f, wings_default::kColorMembraneOuter, wings_default::kColorMembraneInner, wings_default::kColorFrame, wings_default::kColorFeatherTip, 6},
};

// Angel: long white feathers fanned wide off a thin arm.
inline constexpr WingBone kAngelRightBones[7] = {
    {-1,  0.0f,  0.0f, -1.5f, -1.5f, 3.0f, 3.0f, 2.5f, 4.5f,   0.0f, 0.0f, 0.0f, 0.00f, 1.00f, kAngelFrame,         kAngelJointInner,    kAngelFrame, kAngelFrame,         0},
    { 0, -2.0f,  0.0f, -7.0f, -0.5f, 7.0f, 1.0f, 3.0f, 4.0f,   0.0f, 0.5f, 0.4f, 0.25f, 1.00f, kAngelFrame,         kAngelFrame,         kAngelFrame, kAngelFrame,         1},
    { 1, -7.0f,  0.0f, -1.0f, -5.0f, 2.0f, 6.0f, 3.0f, 4.0f,   3.0f, 0.8f, 1.4f, 0.75f, 1.00f, kAngelMembraneOuter, kAngelMembraneInner, kAngelFrame, kAngelFeatherTip,    2},
    { 1, -6.0f, -1.0f, -1.0f, -6.0f, 2.0f, 7.0f, 3.0f, 4.0f,   5.0f, 1.0f, 1.4f, 0.80f, 0.95f, kAngelMembraneOuter, kAngelMembraneInner, kAngelFrame, kAngelFeatherTip,    3},
    { 1, -5.0f, -2.0f, -1.0f, -7.0f, 2.0f, 8.0f, 3.0f, 4.0f,   9.0f, 1.3f, 1.5f, 0.90f, 1.00f, kAngelMembraneOuter, kAngelMembraneInner, kAngelFrame, kAngelFeatherTip,    4},
    { 1, -4.0f, -2.5f, -1.0f, -7.5f, 2.0f, 8.5f, 3.0f, 4.0f,  13.0f, 1.6f, 1.5f, 0.95f, 0.95f, kAngelMembraneOuter, kAngelMembraneInner, kAngelFrame, kAngelFeatherTip,    5},
    { 1, -2.5f, -2.5f, -1.0f, -7.0f, 2.0f, 8.0f, 3.0f, 4.0f,  17.0f, 1.9f, 1.5f, 1.00f, 1.00f, kAngelMembraneOuter, kAngelMembraneInner, kAngelFrame, kAngelFeatherTip,    6},
};

// Demon: broad membrane with spiky, sharply tapered finger claws.
inline constexpr WingBone kDemonRightBones[7] = {
    {-1,  0.0f,  0.0f, -1.5f, -1.5f, 3.0f, 3.0f, 2.5f, 4.5f,   0.0f, 0.0f, 0.0f, 0.00f, 1.00f, kDemonFrame,         kDemonJointInner,    kDemonFrame, kDemonFrame,         0},
    { 0, -2.0f,  0.0f, -7.0f, -1.5f, 7.0f, 3.0f, 3.0f, 4.0f,   0.0f, 0.5f, 1.0f, 0.28f, 1.00f, kDemonFrame,         kDemonFrame,         kDemonFrame, kDemonFrame,         1},
    { 1, -5.0f, -1.5f, -4.0f, -6.0f, 8.0f, 7.0f, 3.0f, 4.0f,   2.0f, 1.0f, 1.6f, 0.55f, 1.00f, kDemonMembraneOuter, kDemonMembraneInner, kDemonFrame, kDemonFrame,         2},
    { 1, -2.0f, -1.5f, -1.0f, -6.0f, 2.0f, 7.0f, 3.0f, 4.0f,   8.0f, 1.3f, 1.3f, 0.70f, 0.94f, kDemonMembraneOuter, kDemonMembraneInner, kDemonFrame, kDemonFeatherTip,    3},
    { 1, -5.0f, -2.0f, -1.0f, -5.0f, 2.0f, 6.0f, 3.0f, 4.0f,  15.0f, 1.6f, 1.3f, 0.88f, 1.00f, kDemonMembraneOuter, kDemonMembraneInner, kDemonFrame, kDemonFeatherTip,    4},
    { 1, -3.0f,  1.5f, -1.0f, -1.0f, 2.0f, 4.0f, 3.0f, 4.0f,  -8.0f, 0.8f, 1.0f, 0.45f, 1.05f, kDemonMembraneOuter, kDemonMembraneInner, kDemonFrame, kDemonFeatherTip,    5},
    { 1, -7.0f,  0.0f, -3.0f, -0.5f, 3.0f, 1.5f, 3.0f, 4.0f,   4.0f, 2.0f, 0.9f, 1.00f, 1.00f, kDemonMembraneOuter, kDemonMembraneInner, kDemonFrame, kDemonFeatherTip,    6},
};

// Bat: small membrane, barely tapered, fingers only slightly fanned.
inline constexpr WingBone kBatRightBones[7] = {
    {-1,  0.0f,  0.0f, -1.5f, -1.5f, 3.0f, 3.0f, 2.5f, 4.5f,   0.0f, 0.0f, 0.0f, 0.00f, 1.00f, kBatFrame,         kBatJointInner,    kBatFrame, kBatFrame,         0},
    { 0, -2.0f,  0.0f, -5.0f, -1.0f, 5.0f, 2.0f, 3.0f, 4.0f,   0.0f, 0.4f, 0.6f, 0.25f, 1.00f, kBatFrame,         kBatFrame,         kBatFrame, kBatFrame,         1},
    { 1, -4.0f, -1.0f, -3.0f, -4.0f, 7.0f, 5.0f, 3.0f, 4.0f,   2.0f, 0.8f, 1.2f, 0.55f, 1.00f, kBatMembraneOuter, kBatMembraneInner, kBatFrame, kBatFrame,         2},
    { 2, -3.0f, -4.0f, -1.0f, -2.0f, 2.0f, 3.0f, 3.0f, 4.0f,   6.0f, 1.0f, 0.8f, 0.72f, 0.94f, kBatMembraneOuter, kBatMembraneInner, kBatFrame, kBatFeatherTip,    3},
    { 2, -5.0f, -3.0f, -1.0f, -2.0f, 2.0f, 3.0f, 3.0f, 4.0f,  12.0f, 1.2f, 0.8f, 0.88f, 1.00f, kBatMembraneOuter, kBatMembraneInner, kBatFrame, kBatFeatherTip,    4},
    { 2, -4.0f,  1.0f, -1.0f, -1.0f, 2.0f, 3.0f, 3.0f, 4.0f,  -6.0f, 0.6f, 0.6f, 0.50f, 1.00f, kBatMembraneOuter, kBatMembraneInner, kBatFrame, kBatFeatherTip,    5},
    { 2, -6.0f,  0.0f, -2.0f, -0.5f, 2.0f, 1.0f, 3.0f, 4.0f,   3.0f, 1.4f, 0.6f, 1.00f, 1.00f, kBatMembraneOuter, kBatMembraneInner, kBatFrame, kBatFrame,         6},
};

// Butterfly: two broad rounded panels, barely tapered, with accent spots.
inline constexpr WingBone kButterflyRightBones[7] = {
    {-1,  0.0f,  0.0f, -1.5f, -1.5f, 3.0f, 3.0f, 2.5f, 4.5f,   0.0f, 0.0f, 0.0f, 0.00f, 1.00f, kButterflyFrame,         kButterflyJointInner,    kButterflyFrame, kButterflyFrame,         0},
    { 0, -2.0f,  0.0f, -5.0f, -0.5f, 5.0f, 1.0f, 3.0f, 4.0f,   0.0f, 0.4f, 0.4f, 0.20f, 1.00f, kButterflyFrame,         kButterflyFrame,         kButterflyFrame, kButterflyFrame,         1},
    { 1, -3.0f,  0.0f, -3.0f,  0.0f, 6.0f, 6.0f, 3.0f, 4.0f,   6.0f, 0.8f, 0.8f, 0.60f, 1.00f, kButterflyMembraneOuter, kButterflyMembraneInner, kButterflyFrame, kButterflyFrame,         2},
    { 1, -3.0f,  0.0f, -2.0f, -6.0f, 5.0f, 6.0f, 3.0f, 4.0f,  -6.0f, 0.8f, 0.8f, 0.60f, 0.96f, kButterflyMembraneOuter, kButterflyMembraneInner, kButterflyFrame, kButterflyFeatherTip,    3},
    { 2, -3.0f,  5.0f, -2.0f, -1.0f, 5.0f, 2.0f, 3.0f, 4.0f,   4.0f, 1.0f, 0.6f, 0.90f, 1.00f, kButterflyFeatherTip,    kButterflyMembraneInner, kButterflyFrame, kButterflyFeatherTip,    4},
    { 3, -3.0f, -5.0f, -2.0f, -1.0f, 5.0f, 2.0f, 3.0f, 4.0f,  -4.0f, 1.0f, 0.6f, 0.90f, 1.00f, kButterflyFeatherTip,    kButterflyMembraneInner, kButterflyFrame, kButterflyFeatherTip,    5},
    { 1, -2.0f, -0.5f, -1.0f, -3.0f, 2.0f, 6.0f, 3.0f, 4.0f,   0.0f, 1.2f, 0.6f, 0.75f, 1.04f, kButterflyFrame,         kButterflyFrame,         kButterflyFrame, kButterflyFrame,         6},
};

// Phoenix: long flowing feathers, strongly fanned and swept back.
inline constexpr WingBone kPhoenixRightBones[7] = {
    {-1,  0.0f,  0.0f, -1.5f, -1.5f, 3.0f, 3.0f, 2.5f, 4.5f,   0.0f, 0.0f, 0.0f, 0.00f, 1.00f, kPhoenixFrame,         kPhoenixJointInner,    kPhoenixFrame, kPhoenixFrame,         0},
    { 0, -2.0f,  0.0f, -7.0f, -1.5f, 7.0f, 3.0f, 3.0f, 4.0f,   0.0f, 0.6f, 1.0f, 0.28f, 1.00f, kPhoenixFrame,         kPhoenixFrame,         kPhoenixFrame, kPhoenixFrame,         1},
    { 1, -6.0f, -1.5f, -3.0f, -6.0f, 6.0f, 8.0f, 3.0f, 4.0f,   3.0f, 1.2f, 1.8f, 0.55f, 1.00f, kPhoenixMembraneOuter, kPhoenixMembraneInner, kPhoenixFrame, kPhoenixFrame,         2},
    { 1, -3.0f, -2.0f, -1.0f, -8.0f, 2.0f, 9.0f, 3.0f, 4.0f,   7.0f, 1.5f, 1.5f, 0.75f, 0.95f, kPhoenixMembraneOuter, kPhoenixMembraneInner, kPhoenixFrame, kPhoenixFeatherTip,    3},
    { 1, -5.0f, -2.5f, -1.0f, -7.0f, 2.0f, 8.0f, 3.0f, 4.0f,  13.0f, 1.8f, 1.5f, 0.90f, 1.00f, kPhoenixMembraneOuter, kPhoenixMembraneInner, kPhoenixFrame, kPhoenixFeatherTip,    4},
    { 1, -4.0f,  1.5f, -1.0f, -1.0f, 2.0f, 5.0f, 3.0f, 4.0f,  -9.0f, 1.0f, 1.2f, 0.50f, 1.04f, kPhoenixMembraneOuter, kPhoenixMembraneInner, kPhoenixFrame, kPhoenixFeatherTip,    5},
    { 1, -7.0f,  0.0f, -3.0f, -0.5f, 3.0f, 1.5f, 3.0f, 4.0f,   5.0f, 2.2f, 1.0f, 1.00f, 1.00f, kPhoenixMembraneOuter, kPhoenixMembraneInner, kPhoenixFrame, kPhoenixFrame,         6},
};

// Fairy: small wings, softly tapered, almost no sweep.
inline constexpr WingBone kFairyRightBones[7] = {
    {-1,  0.0f,  0.0f, -1.5f, -1.5f, 3.0f, 3.0f, 2.5f, 4.5f,   0.0f, 0.0f, 0.0f, 0.00f, 1.00f, kFairyFrame,         kFairyJointInner,    kFairyFrame, kFairyFrame,         0},
    { 0, -2.0f,  0.0f, -4.0f, -0.5f, 4.0f, 1.0f, 3.0f, 4.0f,   0.0f, 0.3f, 0.4f, 0.20f, 1.00f, kFairyFrame,         kFairyFrame,         kFairyFrame, kFairyFrame,         1},
    { 1, -3.0f,  0.0f, -2.0f,  0.0f, 5.0f, 4.0f, 3.0f, 4.0f,   7.0f, 0.6f, 0.8f, 0.60f, 1.00f, kFairyMembraneOuter, kFairyMembraneInner, kFairyFrame, kFairyFrame,         2},
    { 1, -2.5f,  0.0f, -2.0f, -5.0f, 4.0f, 5.0f, 3.0f, 4.0f,  -7.0f, 0.6f, 0.8f, 0.60f, 0.96f, kFairyMembraneOuter, kFairyMembraneInner, kFairyFrame, kFairyFeatherTip,    3},
    { 2, -3.0f,  3.0f, -1.0f, -1.0f, 2.0f, 2.0f, 3.0f, 4.0f,   5.0f, 0.8f, 0.5f, 0.88f, 1.00f, kFairyFeatherTip,    kFairyMembraneInner, kFairyFrame, kFairyFeatherTip,    4},
    { 3, -3.0f, -4.0f, -1.0f, -1.0f, 2.0f, 2.0f, 3.0f, 4.0f,  -5.0f, 0.8f, 0.5f, 0.88f, 1.00f, kFairyFeatherTip,    kFairyMembraneInner, kFairyFrame, kFairyFeatherTip,    5},
    { 1, -3.5f,  0.5f, -1.0f, -1.0f, 2.0f, 2.0f, 3.0f, 4.0f,   0.0f, 1.0f, 0.5f, 0.75f, 1.04f, kFairyFeatherTip,    kFairyMembraneInner, kFairyFrame, kFairyFeatherTip,    6},
};

struct WingStyle {
    const char* id;                  // config id (radio option)
    const char* label;               // human-readable name
    int boneCount;                   // bones per side
    const WingBone* rightBones;      // spans -x
};

inline constexpr WingStyle kWingStyles[] = {
    {"dragon",    "Dragon",    7, kDragonRightBones},
    {"angel",     "Angel",     7, kAngelRightBones},
    {"demon",     "Demon",     7, kDemonRightBones},
    {"bat",       "Bat",       7, kBatRightBones},
    {"butterfly", "Butterfly", 7, kButterflyRightBones},
    {"phoenix",   "Phoenix",   7, kPhoenixRightBones},
    {"fairy",     "Fairy",     7, kFairyRightBones},
};

constexpr int kWingStyleCount = static_cast<int>(sizeof(kWingStyles) / sizeof(kWingStyles[0]));
constexpr int kMaxWingBones = 16;

// Shoulder pivot of each side (model px); the boxes hang off these pivots.
constexpr float kRightRootPivotX = -3.0f;
constexpr float kLeftRootPivotX = 3.0f;
constexpr float kRootPivotY = 21.0f;

// The left wing is the mirror image of the right one. Only the geometry is
// mirrored: the rest-pose fan keeps its sign because the render code already
// applies the left side's angles with the opposite rotation sign, and a
// mirrored rotation conjugated by the x flip is exactly the mirrored pose.
inline WingBone mirrorBone(const WingBone& right) {
    WingBone out = right;
    out.anchorX = -right.anchorX;
    out.boxOX = -(right.boxOX + right.boxSX);
    return out;
}

inline void mirrorWingBones(const WingBone* right, int n, WingBone* out) {
    for (int i = 0; i < n; ++i) out[i] = mirrorBone(right[i]);
}

inline int wingStyleIndexForId(const std::string& id) {
    for (int i = 0; i < kWingStyleCount; ++i) {
        if (id == kWingStyles[i].id) return i;
    }
    return 0;  // dragon default
}

// Serializes the picker value in the launcher's radio format:
// "<selectedIndex>,<id1>,<id2>,..." (same convention as Custom Capes).
inline std::string wingStyleRadioValue(int index) {
    if (index < 0 || index >= kWingStyleCount) index = 0;
    std::string value = std::to_string(index);
    for (int i = 0; i < kWingStyleCount; ++i) {
        value += ',';
        value += kWingStyles[i].id;
    }
    return value;
}

// Parses a value coming from the config file (full radio value), from the
// launcher (just the numeric index) or a bare style id.
inline int resolveWingStyleIndex(const std::string& value) {
    if (value.empty()) return 0;

    const std::size_t comma = value.find(',');
    const std::string head = value.substr(0, comma);

    bool numeric = !head.empty();
    for (char c : head) {
        if (!std::isdigit(static_cast<unsigned char>(c))) { numeric = false; break; }
    }
    if (numeric) {
        try {
            const int idx = std::stoi(head);
            if (idx >= 0 && idx < kWingStyleCount) return idx;
        } catch (...) {}
        return 0;
    }
    return wingStyleIndexForId(head);
}

}  // namespace bedrocktools::modules::wings

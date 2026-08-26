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

// ---- Dragon (default) - original brown/dark palette, independent from
// wings_default so changing the default texture does not affect dragon ----
inline constexpr unsigned char kDragonFrame[3]         = {94, 62, 36};
inline constexpr unsigned char kDragonMembraneOuter[3] = {18, 18, 24};
inline constexpr unsigned char kDragonMembraneInner[3] = {28, 28, 36};
inline constexpr unsigned char kDragonFeatherTip[3]    = {46, 46, 60};
inline constexpr unsigned char kDragonJointInner[3]    = {76, 52, 32};

// ---- Angel: white feathers with soft gold tips ----
inline constexpr unsigned char kAngelFrame[3]         = {244, 240, 232};
inline constexpr unsigned char kAngelMembraneOuter[3] = {252, 250, 246};
inline constexpr unsigned char kAngelMembraneInner[3] = {226, 222, 210};
inline constexpr unsigned char kAngelFeatherTip[3]    = {255, 236, 190};
inline constexpr unsigned char kAngelJointInner[3]    = {214, 205, 188};

// ---- Demon: glowing red membrane on pure black frame (Demon Wings spec)
// Frame #000000 black, membrane gradient #FF0000/#E60000 -> #800000 dark bloody
inline constexpr unsigned char kDemonFrame[3]         = {0, 0, 0};
inline constexpr unsigned char kDemonMembraneOuter[3] = {230, 0, 0};   // #E60000 glowing red
inline constexpr unsigned char kDemonMembraneInner[3] = {128, 0, 0};   // #800000 dark bloody
inline constexpr unsigned char kDemonFeatherTip[3]    = {255, 0, 0};   // #FF0000 bright tip
inline constexpr unsigned char kDemonJointInner[3]    = {0, 0, 0};

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

// ---- Demon Wings / Vampire Bat Wings - requested style ----
// Pure black bones #000000, membrane gradient dramatic #FF0000/#E60000 -> #800000
// Crimson glow / aura effect saturated between black separators
inline constexpr unsigned char kDemonWingsFrame[3]         = {0, 0, 0};       // #000000
inline constexpr unsigned char kDemonWingsMembraneOuter[3] = {255, 0, 0};     // #FF0000 bright glowing red
inline constexpr unsigned char kDemonWingsMembraneInner[3] = {128, 0, 0};     // #800000 dark bloody
inline constexpr unsigned char kDemonWingsFeatherTip[3]    = {230, 0, 0};     // #E60000
inline constexpr unsigned char kDemonWingsJointInner[3]    = {0, 0, 0};

inline constexpr unsigned char kVampireFrame[3]         = {0, 0, 0};
inline constexpr unsigned char kVampireMembraneOuter[3] = {230, 0, 0};   // #E60000
inline constexpr unsigned char kVampireMembraneInner[3] = {128, 0, 0};   // #800000
inline constexpr unsigned char kVampireFeatherTip[3]    = {255, 0, 0};   // #FF0000
inline constexpr unsigned char kVampireJointInner[3]    = {0, 0, 0};

inline constexpr unsigned char kRedBatFrame[3]         = {0, 0, 0};
inline constexpr unsigned char kRedBatMembraneOuter[3] = {255, 0, 0};    // #FF0000
inline constexpr unsigned char kRedBatMembraneInner[3] = {160, 0, 0};    // slightly brighter dark
inline constexpr unsigned char kRedBatFeatherTip[3]    = {255, 51, 51};  // glowing edge
inline constexpr unsigned char kRedBatJointInner[3]    = {0, 0, 0};

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

// Dragon: articulated bat/dragon membrane wing. A two-bone forearm (upper +
// tip) forms the leading edge; four wide, overlapping membrane panels fan
// out from anchor points along that arm so the trailing edge reads as one
// continuous sail instead of thin disconnected blades - a real wing shape.
inline constexpr WingBone kDragonRightBones[7] = {
    {-1,   0.00f,   0.00f,  -1.50f,  -1.50f,  3.00f,  3.00f, 2.50f, 4.50f,    0.00f, 0.00f, 0.00f, 0.00f, 1.00f, kDragonMembraneInner, kDragonJointInner, kDragonFrame, kDragonFrame, 0},
    { 0,  -2.00f,   0.00f,  -7.50f,  -1.70f,  7.50f,  3.40f, 3.00f, 4.00f,    0.00f, 0.60f, 0.60f, 0.20f, 1.00f, kDragonFrame, kDragonFrame, kDragonFrame, kDragonFrame, 1},
    { 1,  -7.50f,   0.00f,  -6.00f,  -1.40f,  6.00f,  2.80f, 3.00f, 4.00f,   -6.00f, 1.00f, 0.70f, 0.55f, 1.02f, kDragonFrame, kDragonFrame, kDragonFrame, kDragonFrame, 2},
    { 1,  -2.40f,  -1.70f,  -2.30f,  -6.50f,  4.60f,  6.50f, 3.00f, 4.00f,    7.00f, 0.60f, 1.40f, 0.42f, 1.00f, kDragonMembraneOuter, kDragonMembraneInner, kDragonFrame, kDragonFeatherTip, 3},
    { 1,  -6.45f,  -1.70f,  -2.30f,  -7.80f,  4.60f,  7.80f, 3.00f, 4.00f,   14.00f, 1.00f, 1.40f, 0.66f, 0.97f, kDragonMembraneOuter, kDragonMembraneInner, kDragonFrame, kDragonFeatherTip, 4},
    { 2,  -1.80f,  -1.40f,  -2.30f,  -7.00f,  4.60f,  7.00f, 3.00f, 4.00f,   18.00f, 1.30f, 1.40f, 0.85f, 1.03f, kDragonMembraneOuter, kDragonMembraneInner, kDragonFrame, kDragonFeatherTip, 5},
    { 2,  -5.04f,  -1.40f,  -2.30f,  -8.20f,  4.60f,  8.20f, 3.00f, 4.00f,   26.00f, 1.70f, 1.40f, 1.00f, 0.95f, kDragonMembraneOuter, kDragonMembraneInner, kDragonFrame, kDragonFeatherTip, 6},
};

// Angel: long white primary feathers fanned wide off a slim arm, like a
// bird's covert + primary layout: a short wrist bone splits the arm in two
// and four broad, overlapping quills fan out from it, longest in the middle
// of the fan and tapering to soft points at the ends.
inline constexpr WingBone kAngelRightBones[7] = {
    {-1,   0.00f,   0.00f,  -1.50f,  -1.50f,  3.00f,  3.00f, 2.50f, 4.50f,    0.00f, 0.00f, 0.00f, 0.00f, 1.00f, kAngelMembraneInner, kAngelJointInner, kAngelFrame, kAngelFrame, 0},
    { 0,  -2.00f,   0.00f,  -7.00f,  -0.80f,  7.00f,  1.60f, 3.00f, 4.00f,    0.00f, 0.50f, 0.50f, 0.18f, 1.00f, kAngelFrame, kAngelFrame, kAngelFrame, kAngelFrame, 1},
    { 1,  -7.00f,   0.00f,  -4.34f,  -0.52f,  4.34f,  1.04f, 3.00f, 4.00f,    3.60f, 0.65f, 0.40f, 0.55f, 1.02f, kAngelFrame, kAngelFrame, kAngelFrame, kAngelFrame, 2},
    { 1,  -2.10f,  -0.80f,  -1.70f,  -7.50f,  3.40f,  7.50f, 3.00f, 4.00f,    4.00f, 0.80f, 2.40f, 0.45f, 1.00f, kAngelMembraneOuter, kAngelMembraneInner, kAngelFrame, kAngelFeatherTip, 3},
    { 1,  -5.60f,  -0.80f,  -1.70f,  -9.00f,  3.40f,  9.00f, 3.00f, 4.00f,    9.00f, 1.20f, 2.40f, 0.68f, 0.97f, kAngelMembraneOuter, kAngelMembraneInner, kAngelFrame, kAngelFeatherTip, 4},
    { 2,  -1.30f,  -0.52f,  -1.70f,  -8.50f,  3.40f,  8.50f, 3.00f, 4.00f,   14.00f, 1.50f, 2.40f, 0.88f, 1.03f, kAngelMembraneOuter, kAngelMembraneInner, kAngelFrame, kAngelFeatherTip, 5},
    { 2,  -3.56f,  -0.52f,  -1.70f,  -7.50f,  3.40f,  7.50f, 3.00f, 4.00f,   20.00f, 1.70f, 2.40f, 1.00f, 0.95f, kAngelMembraneOuter, kAngelMembraneInner, kAngelFrame, kAngelFeatherTip, 6},
};

// Demon: bat/dragon membrane wing sharing Dragon's proportions but with the
// Demon Wings palette - black frame, red glowing membrane.
inline constexpr WingBone kDemonRightBones[7] = {
    {-1,   0.00f,   0.00f,  -1.50f,  -1.50f,  3.00f,  3.00f, 2.50f, 4.50f,    0.00f, 0.00f, 0.00f, 0.00f, 1.00f, kDemonMembraneInner, kDemonJointInner, kDemonFrame, kDemonFrame, 0},
    { 0,  -2.00f,   0.00f,  -7.50f,  -1.70f,  7.50f,  3.40f, 3.00f, 4.00f,   -8.00f, 0.60f, 0.60f, 0.20f, 1.00f, kDemonFrame, kDemonFrame, kDemonFrame, kDemonFrame, 1},
    { 1,  -7.50f,   0.00f,  -6.00f,  -1.40f,  6.00f,  2.80f, 3.00f, 4.00f,   -6.00f, 1.00f, 0.70f, 0.55f, 1.02f, kDemonFrame, kDemonFrame, kDemonFrame, kDemonFrame, 2},
    { 1,  -2.40f,  -1.70f,  -2.30f,  -6.50f,  4.60f,  6.50f, 3.00f, 4.00f,    7.00f, 0.60f, 1.40f, 0.42f, 1.00f, kDemonMembraneOuter, kDemonMembraneInner, kDemonFrame, kDemonFeatherTip, 3},
    { 1,  -6.45f,  -1.70f,  -2.30f,  -7.80f,  4.60f,  7.80f, 3.00f, 4.00f,   14.00f, 1.00f, 1.40f, 0.66f, 0.97f, kDemonMembraneOuter, kDemonMembraneInner, kDemonFrame, kDemonFeatherTip, 4},
    { 2,  -1.80f,  -1.40f,  -2.30f,  -7.00f,  4.60f,  7.00f, 3.00f, 4.00f,   18.00f, 1.30f, 1.40f, 0.85f, 1.03f, kDemonMembraneOuter, kDemonMembraneInner, kDemonFrame, kDemonFeatherTip, 5},
    { 2,  -5.04f,  -1.40f,  -2.30f,  -8.20f,  4.60f,  8.20f, 3.00f, 4.00f,   26.00f, 1.70f, 1.40f, 1.00f, 0.95f, kDemonMembraneOuter, kDemonMembraneInner, kDemonFrame, kDemonFeatherTip, 6},
};

// Bat: small membrane wing, same shape language as Dragon but scaled down.
inline constexpr WingBone kBatRightBones[7] = {
    {-1,   0.00f,   0.00f,  -1.50f,  -1.50f,  3.00f,  3.00f, 2.50f, 4.50f,    0.00f, 0.00f, 0.00f, 0.00f, 1.00f, kBatMembraneInner, kBatJointInner, kBatFrame, kBatFrame, 0},
    { 0,  -2.00f,   0.00f,  -5.20f,  -1.20f,  5.20f,  2.40f, 3.00f, 4.00f,   -6.00f, 0.40f, 0.50f, 0.20f, 1.00f, kBatFrame, kBatFrame, kBatFrame, kBatFrame, 1},
    { 1,  -5.20f,   0.00f,  -4.20f,  -1.00f,  4.20f,  2.00f, 3.00f, 4.00f,   -5.00f, 0.70f, 0.60f, 0.55f, 1.02f, kBatFrame, kBatFrame, kBatFrame, kBatFrame, 2},
    { 1,  -1.66f,  -1.20f,  -1.60f,  -4.50f,  3.20f,  4.50f, 3.00f, 4.00f,    6.00f, 0.40f, 1.10f, 0.42f, 1.00f, kBatMembraneOuter, kBatMembraneInner, kBatFrame, kBatFeatherTip, 3},
    { 1,  -4.47f,  -1.20f,  -1.60f,  -5.40f,  3.20f,  5.40f, 3.00f, 4.00f,   12.00f, 0.70f, 1.10f, 0.66f, 0.97f, kBatMembraneOuter, kBatMembraneInner, kBatFrame, kBatFeatherTip, 4},
    { 2,  -1.26f,  -1.00f,  -1.60f,  -4.80f,  3.20f,  4.80f, 3.00f, 4.00f,   16.00f, 0.90f, 1.10f, 0.85f, 1.03f, kBatMembraneOuter, kBatMembraneInner, kBatFrame, kBatFeatherTip, 5},
    { 2,  -3.53f,  -1.00f,  -1.60f,  -5.60f,  3.20f,  5.60f, 3.00f, 4.00f,   22.00f, 1.20f, 1.10f, 1.00f, 0.95f, kBatMembraneOuter, kBatMembraneInner, kBatFrame, kBatFeatherTip, 6},
};

// Butterfly: two broad, overlapping rounded lobes (forewing above, hindwing
// below) sharing the shoulder pivot like real lepidoptera wings, plus two
// small accent tip lobes for the eye-spot highlight.
inline constexpr WingBone kButterflyRightBones[7] = {
    {-1,   0.00f,   0.00f,  -1.50f,  -1.50f,  3.00f,  3.00f, 2.50f, 4.50f,    0.00f, 0.00f, 0.00f, 0.00f, 1.00f, kButterflyMembraneInner, kButterflyJointInner, kButterflyFrame, kButterflyFrame, 0},
    { 0,  -2.00f,   0.00f,  -5.00f,  -0.60f,  5.00f,  1.20f, 3.00f, 4.00f,    0.00f, 0.40f, 0.40f, 0.18f, 1.00f, kButterflyFrame, kButterflyFrame, kButterflyFrame, kButterflyFrame, 1},
    { 1,  -5.00f,   0.00f,  -2.60f,  -0.40f,  2.60f,  0.80f, 3.00f, 4.00f,    8.00f, 0.60f, 0.40f, 0.55f, 1.02f, kButterflyFrame, kButterflyFrame, kButterflyFrame, kButterflyFrame, 2},
    { 1,  -1.60f,  -0.60f,  -3.40f,  -8.00f,  6.80f,  8.00f, 3.00f, 4.00f,    8.00f, 0.90f, 2.60f, 0.45f, 1.00f, kButterflyMembraneOuter, kButterflyMembraneInner, kButterflyFrame, kButterflyFrame, 3},
    { 1,  -4.20f,  -0.60f,  -3.00f,   0.00f,  6.00f,  7.00f, 3.00f, 4.00f,  -10.00f, 1.10f, 2.60f, 0.62f, 0.97f, kButterflyMembraneOuter, kButterflyMembraneInner, kButterflyFrame, kButterflyFeatherTip, 4},
    { 2,  -1.50f,  -0.40f,  -2.20f,  -3.40f,  4.40f,  3.40f, 3.00f, 4.00f,    4.00f, 1.20f, 1.60f, 0.90f, 1.00f, kButterflyFeatherTip,    kButterflyMembraneInner, kButterflyFrame, kButterflyFeatherTip, 5},
    { 4,  -3.00f,   3.50f,  -1.60f,  -2.60f,  3.20f,  2.60f, 3.00f, 4.00f,    6.00f, 1.30f, 1.20f, 0.95f, 1.04f, kButterflyFeatherTip,    kButterflyMembraneInner, kButterflyFrame, kButterflyFeatherTip, 6},
};

// Phoenix: long flowing fiery feathers off a wrist-split arm, strongly
// fanned and swept back like a bird's spread primaries.
inline constexpr WingBone kPhoenixRightBones[7] = {
    {-1,   0.00f,   0.00f,  -1.50f,  -1.50f,  3.00f,  3.00f, 2.50f, 4.50f,    0.00f, 0.00f, 0.00f, 0.00f, 1.00f, kPhoenixMembraneInner, kPhoenixJointInner, kPhoenixFrame, kPhoenixFrame, 0},
    { 0,  -2.00f,   0.00f,  -7.50f,  -1.50f,  7.50f,  3.00f, 3.00f, 4.00f,    0.00f, 1.00f, 0.50f, 0.18f, 1.00f, kPhoenixFrame, kPhoenixFrame, kPhoenixFrame, kPhoenixFrame, 1},
    { 1,  -7.50f,   0.00f,  -4.65f,  -0.98f,  4.65f,  1.95f, 3.00f, 4.00f,    5.20f, 1.30f, 0.40f, 0.55f, 1.02f, kPhoenixFrame, kPhoenixFrame, kPhoenixFrame, kPhoenixFrame, 2},
    { 1,  -2.25f,  -1.50f,  -1.90f,  -8.00f,  3.80f,  8.00f, 3.00f, 4.00f,    6.00f, 1.00f, 2.60f, 0.45f, 1.00f, kPhoenixMembraneOuter, kPhoenixMembraneInner, kPhoenixFrame, kPhoenixFeatherTip, 3},
    { 1,  -6.00f,  -1.50f,  -1.90f,  -9.50f,  3.80f,  9.50f, 3.00f, 4.00f,   13.00f, 1.40f, 2.60f, 0.68f, 0.97f, kPhoenixMembraneOuter, kPhoenixMembraneInner, kPhoenixFrame, kPhoenixFeatherTip, 4},
    { 2,  -1.40f,  -0.98f,  -1.90f,  -9.00f,  3.80f,  9.00f, 3.00f, 4.00f,   19.00f, 1.70f, 2.60f, 0.88f, 1.03f, kPhoenixMembraneOuter, kPhoenixMembraneInner, kPhoenixFrame, kPhoenixFeatherTip, 5},
    { 2,  -3.81f,  -0.98f,  -1.90f,  -8.00f,  3.80f,  8.00f, 3.00f, 4.00f,   27.00f, 2.00f, 2.60f, 1.00f, 0.95f, kPhoenixMembraneOuter, kPhoenixMembraneInner, kPhoenixFrame, kPhoenixFeatherTip, 6},
};

// Fairy: small, dainty feather wings - same fan language as Angel, scaled
// down and with a lighter taper.
inline constexpr WingBone kFairyRightBones[7] = {
    {-1,   0.00f,   0.00f,  -1.50f,  -1.50f,  3.00f,  3.00f, 2.50f, 4.50f,    0.00f, 0.00f, 0.00f, 0.00f, 1.00f, kFairyMembraneInner, kFairyJointInner, kFairyFrame, kFairyFrame, 0},
    { 0,  -2.00f,   0.00f,  -4.20f,  -0.60f,  4.20f,  1.20f, 3.00f, 4.00f,    0.00f, 0.30f, 0.50f, 0.18f, 1.00f, kFairyFrame, kFairyFrame, kFairyFrame, kFairyFrame, 1},
    { 1,  -4.20f,   0.00f,  -2.60f,  -0.39f,  2.60f,  0.78f, 3.00f, 4.00f,    4.00f, 0.39f, 0.40f, 0.55f, 1.02f, kFairyFrame, kFairyFrame, kFairyFrame, kFairyFrame, 2},
    { 1,  -1.26f,  -0.60f,  -1.20f,  -4.50f,  2.40f,  4.50f, 3.00f, 4.00f,    5.00f, 0.50f, 1.60f, 0.45f, 1.00f, kFairyMembraneOuter, kFairyMembraneInner, kFairyFrame, kFairyFeatherTip, 3},
    { 1,  -3.36f,  -0.60f,  -1.20f,  -5.40f,  2.40f,  5.40f, 3.00f, 4.00f,   10.00f, 0.70f, 1.60f, 0.68f, 0.97f, kFairyMembraneOuter, kFairyMembraneInner, kFairyFrame, kFairyFeatherTip, 4},
    { 2,  -0.78f,  -0.39f,  -1.20f,  -5.00f,  2.40f,  5.00f, 3.00f, 4.00f,   15.00f, 0.90f, 1.60f, 0.88f, 1.03f, kFairyMembraneOuter, kFairyMembraneInner, kFairyFrame, kFairyFeatherTip, 5},
    { 2,  -2.14f,  -0.39f,  -1.20f,  -4.40f,  2.40f,  4.40f, 3.00f, 4.00f,   20.00f, 1.00f, 1.60f, 1.00f, 0.95f, kFairyMembraneOuter, kFairyMembraneInner, kFairyFrame, kFairyFeatherTip, 6},
};

// ---------------------------------------------------------------------------
// NEW: Demon Wings - requested style (Demon Wings / Vampire Bat Wings)
// Membranous Bat/Dragon Wings, not feathery. Black bones prominent extended
// forming outer frame and dividers. Lower edges serrated/wavy (Webbed Bat Wing
// pattern), with slight downward angle from back attachment.
// Colors: frame #000000 black, membrane gradient #FF0000/#E60000 -> #800000
// with crimson glow aura between black separators.
// ---------------------------------------------------------------------------
inline constexpr WingBone kDemonWingsRightBones[7] = {
    {-1,   0.00f,   0.00f,  -1.50f,  -1.50f,  3.00f,  3.00f, 2.50f, 4.50f,    0.00f, 0.00f, 0.00f, 0.00f, 1.00f, kDemonWingsMembraneInner, kDemonWingsJointInner, kDemonWingsFrame, kDemonWingsFrame, 0},
    { 0,  -2.00f,   0.00f,  -8.00f,  -1.80f,  8.00f,  3.60f, 3.00f, 4.00f,  -10.00f, 0.60f, 0.60f, 0.20f, 1.00f, kDemonWingsFrame, kDemonWingsFrame, kDemonWingsFrame, kDemonWingsFrame, 1},
    { 1,  -8.00f,   0.00f,  -6.40f,  -1.50f,  6.40f,  3.00f, 3.00f, 4.00f,   -6.00f, 1.00f, 0.70f, 0.55f, 1.02f, kDemonWingsFrame, kDemonWingsFrame, kDemonWingsFrame, kDemonWingsFrame, 2},
    { 1,  -2.56f,  -1.80f,  -2.50f,  -7.00f,  5.00f,  7.00f, 3.00f, 4.00f,    7.00f, 0.60f, 1.40f, 0.42f, 1.00f, kDemonWingsMembraneOuter, kDemonWingsMembraneInner, kDemonWingsFrame, kDemonWingsFeatherTip, 3},
    { 1,  -6.88f,  -1.80f,  -2.50f,  -8.40f,  5.00f,  8.40f, 3.00f, 4.00f,   14.00f, 1.00f, 1.40f, 0.66f, 0.97f, kDemonWingsMembraneOuter, kDemonWingsMembraneInner, kDemonWingsFrame, kDemonWingsFeatherTip, 4},
    { 2,  -1.92f,  -1.50f,  -2.50f,  -7.60f,  5.00f,  7.60f, 3.00f, 4.00f,   18.00f, 1.30f, 1.40f, 0.85f, 1.03f, kDemonWingsMembraneOuter, kDemonWingsMembraneInner, kDemonWingsFrame, kDemonWingsFeatherTip, 5},
    { 2,  -5.38f,  -1.50f,  -2.50f,  -8.80f,  5.00f,  8.80f, 3.00f, 4.00f,   26.00f, 1.70f, 1.40f, 1.00f, 0.95f, kDemonWingsMembraneOuter, kDemonWingsMembraneInner, kDemonWingsFrame, kDemonWingsFeatherTip, 6},
};

// Vampire Bat Wings - smaller, darker bat variant with the same black/red
// glow membrane silhouette.
inline constexpr WingBone kVampireRightBones[7] = {
    {-1,   0.00f,   0.00f,  -1.50f,  -1.50f,  3.00f,  3.00f, 2.50f, 4.50f,    0.00f, 0.00f, 0.00f, 0.00f, 1.00f, kVampireMembraneInner, kVampireJointInner, kVampireFrame, kVampireFrame, 0},
    { 0,  -2.00f,   0.00f,  -5.60f,  -1.30f,  5.60f,  2.60f, 3.00f, 4.00f,   -9.00f, 0.50f, 0.50f, 0.20f, 1.00f, kVampireFrame, kVampireFrame, kVampireFrame, kVampireFrame, 1},
    { 1,  -5.60f,   0.00f,  -4.60f,  -1.10f,  4.60f,  2.20f, 3.00f, 4.00f,   -5.50f, 0.80f, 0.60f, 0.55f, 1.02f, kVampireFrame, kVampireFrame, kVampireFrame, kVampireFrame, 2},
    { 1,  -1.79f,  -1.30f,  -1.80f,  -5.00f,  3.60f,  5.00f, 3.00f, 4.00f,    6.00f, 0.50f, 1.20f, 0.42f, 1.00f, kVampireMembraneOuter, kVampireMembraneInner, kVampireFrame, kVampireFeatherTip, 3},
    { 1,  -4.82f,  -1.30f,  -1.80f,  -5.90f,  3.60f,  5.90f, 3.00f, 4.00f,   12.50f, 0.80f, 1.20f, 0.66f, 0.97f, kVampireMembraneOuter, kVampireMembraneInner, kVampireFrame, kVampireFeatherTip, 4},
    { 2,  -1.38f,  -1.10f,  -1.80f,  -5.30f,  3.60f,  5.30f, 3.00f, 4.00f,   16.50f, 1.00f, 1.20f, 0.85f, 1.03f, kVampireMembraneOuter, kVampireMembraneInner, kVampireFrame, kVampireFeatherTip, 5},
    { 2,  -3.86f,  -1.10f,  -1.80f,  -6.10f,  3.60f,  6.10f, 3.00f, 4.00f,   23.00f, 1.30f, 1.20f, 1.00f, 0.95f, kVampireMembraneOuter, kVampireMembraneInner, kVampireFrame, kVampireFeatherTip, 6},
};

// Red Bat - intense glowing red bat wings, smallest and most serrated.
inline constexpr WingBone kRedBatRightBones[7] = {
    {-1,   0.00f,   0.00f,  -1.50f,  -1.50f,  3.00f,  3.00f, 2.50f, 4.50f,    0.00f, 0.00f, 0.00f, 0.00f, 1.00f, kRedBatMembraneInner, kRedBatJointInner, kRedBatFrame, kRedBatFrame, 0},
    { 0,  -2.00f,   0.00f,  -4.60f,  -1.10f,  4.60f,  2.20f, 3.00f, 4.00f,  -10.00f, 0.40f, 0.50f, 0.20f, 1.00f, kRedBatFrame, kRedBatFrame, kRedBatFrame, kRedBatFrame, 1},
    { 1,  -4.60f,   0.00f,  -3.80f,  -0.90f,  3.80f,  1.80f, 3.00f, 4.00f,   -5.00f, 0.70f, 0.50f, 0.55f, 1.02f, kRedBatFrame, kRedBatFrame, kRedBatFrame, kRedBatFrame, 2},
    { 1,  -1.47f,  -1.10f,  -1.50f,  -4.20f,  3.00f,  4.20f, 3.00f, 4.00f,    6.00f, 0.40f, 1.00f, 0.42f, 1.00f, kRedBatMembraneOuter, kRedBatMembraneInner, kRedBatFrame, kRedBatFeatherTip, 3},
    { 1,  -3.96f,  -1.10f,  -1.50f,  -5.00f,  3.00f,  5.00f, 3.00f, 4.00f,   12.00f, 0.60f, 1.00f, 0.66f, 0.97f, kRedBatMembraneOuter, kRedBatMembraneInner, kRedBatFrame, kRedBatFeatherTip, 4},
    { 2,  -1.14f,  -0.90f,  -1.50f,  -4.40f,  3.00f,  4.40f, 3.00f, 4.00f,   16.00f, 0.80f, 1.00f, 0.85f, 1.03f, kRedBatMembraneOuter, kRedBatMembraneInner, kRedBatFrame, kRedBatFeatherTip, 5},
    { 2,  -3.19f,  -0.90f,  -1.50f,  -5.20f,  3.00f,  5.20f, 3.00f, 4.00f,   22.00f, 1.10f, 1.00f, 1.00f, 0.95f, kRedBatMembraneOuter, kRedBatMembraneInner, kRedBatFrame, kRedBatFeatherTip, 6},
};

struct WingStyle {
    const char* id;                  // config id (radio option)
    const char* label;               // human-readable name
    int boneCount;                   // bones per side
    const WingBone* rightBones;      // spans -x
};

inline constexpr WingStyle kWingStyles[] = {
    {"dragon",     "Dragon",     7, kDragonRightBones},
    {"angel",      "Angel",      7, kAngelRightBones},
    {"demon",      "Demon",      7, kDemonRightBones},
    {"bat",        "Bat",        7, kBatRightBones},
    {"butterfly",  "Butterfly",  7, kButterflyRightBones},
    {"phoenix",    "Phoenix",    7, kPhoenixRightBones},
    {"fairy",      "Fairy",      7, kFairyRightBones},
    // New glowing styles - Demon Wings / Vampire Bat Wings spec
    {"demon_wings","Demon Wings",7, kDemonWingsRightBones},
    {"vampire",    "Vampire Bat",7, kVampireRightBones},
    {"red_bat",    "Red Bat",    7, kRedBatRightBones},
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

#pragma once

// Selectable wing styles for the Wings module.
//
// Every style is a declarative WingDesign: the arm proportions, the membrane
// shape (chord, fan, scallops), the optional feather layer and the palette.
// wings_surface.hpp turns that description into the quad mesh the overlay
// draws, so a style is pure data - no code, no per-style branches - and the
// host tests + tools/wings_preview.cpp evaluate exactly the same table the
// game does.
//
// Right-side geometry only: the left wing is the mirror image, generated in
// buildWing() by negating x, so the two sides can never drift apart.

#include "modules/visual/wings_shape.hpp"

#include <cctype>
#include <cstddef>
#include <string>

namespace bedrocktools::modules::wings {

// ---------------------------------------------------------------------------
// Wing geometry description
// ---------------------------------------------------------------------------

// All lengths are Bedrock model pixels (16 px = 1 block) on the right wing,
// which spans towards -x from the shoulder pivot.
//
// The leading edge is an arm of three segments - upper arm, forearm and hand -
// articulated by the animation (shoulder / elbow / wrist angles) and bent into
// a smooth curve by a Catmull-Rom spline. The membrane hangs off that curve:
// its trailing edge length is chordRootPx at the shoulder, chordTipPx at the
// wing tip, with `fingers` concave scallops of scallopPx carved out between the
// finger struts, swept inboard by fanDeg. camberPx bows the sheet backwards
// (away from the body) so it catches the light, and sweepPx walks the whole
// wing backwards along its span.
//
// Feather styles (angel, phoenix, fairy) additionally grow `featherCount` long
// tapered quills off the arm, which is what makes them read as feathers and
// not as a bat sail.
struct WingDesign {
    const char* id = "dragon";
    const char* label = "Dragon";

    // --- Arm (leading edge) ---
    float upperPx = 9.5f;
    float forePx = 9.0f;
    float handPx = 8.0f;
    float raiseDeg = -8.0f;     // rest raise of the whole wing
    float elbowDeg = -12.0f;    // rest bend at the elbow
    float wristDeg = -8.0f;     // rest bend at the wrist

    // --- Membrane ---
    float chordRootPx = 13.5f;  // trailing-edge drop at the shoulder
    float chordTipPx = 6.0f;    // trailing-edge drop at the wing tip
    float chordMidPx = 0.0f;    // extra bulge at mid-span (bird/butterfly wings)
    float chordBias = 1.2f;     // >1 keeps the chord long further outboard
    float fanDeg = 34.0f;       // inboard sweep of the trailing edge at the tip
    float scallopPx = 3.2f;     // concave bite between two finger struts
    int fingers = 5;            // scallop arcs; the matching struts are drawn
    float camberPx = 1.9f;      // backwards bow of the sheet (gives it volume)
    float sweepPx = 2.2f;       // backwards walk of the arm along the span
    float rootZPx = 2.9f;       // depth of the shoulder pivot (behind the torso)
    float flutterPx = 0.85f;    // membrane ripple amplitude

    // --- Detail layers ---
    // Finger struts are what make a bat sail read as a bat sail, but they only
    // work when the chord is long near the tip as well as at the root. A sheet
    // whose chord collapses towards the tip would grow a fan of struts at the
    // shoulder that curls into a hook, so those styles turn them off.
    bool ribs = true;
    // The shoulder fairing is a 2.2 px collar drawn around the root pivot. It
    // assumes a thin root; on a sheet that already covers the shoulder it reads
    // as a pair of horns, so it is optional too.
    bool fairing = true;

    // --- Feather layer (feather styles only) ---
    bool feathers = false;
    int featherCount = 9;
    float featherLenPx = 15.0f;
    float featherWidthPx = 3.1f;
    float featherSpreadDeg = 30.0f;
    float featherStartU = 0.22f;  // 0 = shoulder, 1 = wing tip

    // --- Palette ---
    FaceColor frame{};    // spars, struts, claw
    FaceColor lead{};     // bright band along the leading edge
    FaceColor back{};     // membrane seen from behind the player (+z side)
    FaceColor front{};    // membrane seen from the body side (-z side)
    FaceColor trail{};    // trailing edge / deepest shadow
    FaceColor tip{};      // feather tips, claw point, rim glints
    FaceColor glow{};     // emissive accent colour (used when glow > 0)
    float glowStrength = 0.0f;  // 0 = plain shading, 1 = fully unlit accent
    float backLight = 0.0f;     // how much the front sheet glows through (+z)
};

// Shoulder pivot of each side (model px).
constexpr float kRightRootPivotX = -3.0f;
constexpr float kLeftRootPivotX = 3.0f;
constexpr float kRootPivotY = 21.0f;

// ---------------------------------------------------------------------------
// Palettes
// ---------------------------------------------------------------------------

// Veil: charcoal membrane under a mid-grey band. Sampled from the reference
// art, which is a near-black sheet (14,14,16) crossed by one greyer fold
// (~54,56,60): the whole wing reads as a silhouette with a single lit crease.
inline constexpr FaceColor kVeilFrame{70, 72, 78};
inline constexpr FaceColor kVeilLead{104, 108, 116};
inline constexpr FaceColor kVeilBack{22, 23, 27};
inline constexpr FaceColor kVeilFront{30, 31, 36};
inline constexpr FaceColor kVeilTrail{12, 12, 15};
inline constexpr FaceColor kVeilTip{132, 137, 146};
inline constexpr FaceColor kVeilGlow{96, 100, 110};
inline constexpr FaceColor kVeilMembraneOuter{22, 23, 27};
inline constexpr FaceColor kVeilMembraneInner{30, 31, 36};
inline constexpr FaceColor kVeilFeatherTip{132, 137, 146};
inline constexpr FaceColor kVeilJointInner{54, 56, 60};

// Dragon: warm bone spars over a slate-violet membrane.
inline constexpr FaceColor kDragonFrame{132, 92, 54};
inline constexpr FaceColor kDragonLead{196, 148, 92};
inline constexpr FaceColor kDragonBack{58, 54, 76};
inline constexpr FaceColor kDragonFront{74, 68, 94};
inline constexpr FaceColor kDragonTrail{28, 26, 40};
inline constexpr FaceColor kDragonTip{224, 176, 108};
inline constexpr FaceColor kDragonGlow{255, 214, 140};
inline constexpr FaceColor kDragonMembraneOuter{58, 54, 76};
inline constexpr FaceColor kDragonMembraneInner{74, 68, 94};
inline constexpr FaceColor kDragonFeatherTip{224, 176, 108};
inline constexpr FaceColor kDragonJointInner{96, 66, 40};

// Angel: warm-white feathers with gold primary tips.
inline constexpr FaceColor kAngelFrame{232, 228, 216};
inline constexpr FaceColor kAngelLead{244, 240, 228};
inline constexpr FaceColor kAngelBack{170, 174, 184};
inline constexpr FaceColor kAngelFront{192, 196, 204};
inline constexpr FaceColor kAngelTrail{120, 126, 138};
inline constexpr FaceColor kAngelTip{255, 206, 104};
inline constexpr FaceColor kAngelGlow{255, 250, 226};
inline constexpr FaceColor kAngelMembraneOuter{190, 192, 198};
inline constexpr FaceColor kAngelMembraneInner{232, 232, 234};
inline constexpr FaceColor kAngelFeatherTip{255, 206, 104};
inline constexpr FaceColor kAngelJointInner{214, 205, 188};

// Demon: pure black spars, glowing blood-red membrane.
inline constexpr FaceColor kDemonFrame{0, 0, 0};
inline constexpr FaceColor kDemonLead{96, 4, 8};
inline constexpr FaceColor kDemonBack{150, 10, 12};
inline constexpr FaceColor kDemonFront{196, 24, 20};
inline constexpr FaceColor kDemonTrail{66, 2, 6};
inline constexpr FaceColor kDemonTip{255, 70, 44};
inline constexpr FaceColor kDemonGlow{255, 46, 30};
inline constexpr FaceColor kDemonMembraneOuter{150, 10, 12};
inline constexpr FaceColor kDemonMembraneInner{196, 24, 20};
inline constexpr FaceColor kDemonFeatherTip{255, 70, 44};
inline constexpr FaceColor kDemonJointInner{40, 0, 4};

// Bat: small, very dark, violet-black membrane.
inline constexpr FaceColor kBatFrame{58, 54, 72};
inline constexpr FaceColor kBatLead{124, 116, 150};
inline constexpr FaceColor kBatBack{62, 58, 82};
inline constexpr FaceColor kBatFront{84, 76, 110};
inline constexpr FaceColor kBatTrail{30, 27, 42};
inline constexpr FaceColor kBatTip{120, 112, 146};
inline constexpr FaceColor kBatGlow{150, 140, 190};
inline constexpr FaceColor kBatMembraneOuter{62, 58, 82};
inline constexpr FaceColor kBatMembraneInner{84, 76, 110};
inline constexpr FaceColor kBatFeatherTip{120, 112, 146};
inline constexpr FaceColor kBatJointInner{38, 35, 48};

// Butterfly: pink forewing over an orange hindwing, cyan accents.
inline constexpr FaceColor kButterflyFrame{92, 44, 146};
inline constexpr FaceColor kButterflyLead{250, 172, 216};
inline constexpr FaceColor kButterflyBack{236, 100, 160};
inline constexpr FaceColor kButterflyFront{246, 148, 194};
inline constexpr FaceColor kButterflyTrail{124, 202, 255};
inline constexpr FaceColor kButterflyTip{255, 224, 128};
inline constexpr FaceColor kButterflyGlow{255, 216, 240};
inline constexpr FaceColor kButterflyMembraneOuter{248, 116, 174};
inline constexpr FaceColor kButterflyMembraneInner{255, 168, 208};
inline constexpr FaceColor kButterflyFeatherTip{255, 224, 128};
inline constexpr FaceColor kButterflyJointInner{126, 66, 172};

// Phoenix: fiery orange primaries with gold tips.
inline constexpr FaceColor kPhoenixFrame{176, 92, 22};
inline constexpr FaceColor kPhoenixLead{255, 186, 66};
inline constexpr FaceColor kPhoenixBack{184, 52, 18};
inline constexpr FaceColor kPhoenixFront{206, 84, 26};
inline constexpr FaceColor kPhoenixTrail{118, 20, 6};
inline constexpr FaceColor kPhoenixTip{255, 240, 148};
inline constexpr FaceColor kPhoenixGlow{255, 196, 80};
inline constexpr FaceColor kPhoenixMembraneOuter{184, 52, 18};
inline constexpr FaceColor kPhoenixMembraneInner{206, 84, 26};
inline constexpr FaceColor kPhoenixFeatherTip{255, 240, 148};
inline constexpr FaceColor kPhoenixJointInner{150, 70, 14};

// Fairy: translucent cyan/pink petals.
inline constexpr FaceColor kFairyFrame{110, 214, 204};
inline constexpr FaceColor kFairyLead{222, 250, 246};
inline constexpr FaceColor kFairyBack{126, 208, 234};
inline constexpr FaceColor kFairyFront{234, 184, 244};
inline constexpr FaceColor kFairyTrail{78, 158, 184};
inline constexpr FaceColor kFairyTip{255, 255, 250};
inline constexpr FaceColor kFairyGlow{210, 250, 255};
inline constexpr FaceColor kFairyMembraneOuter{142, 224, 246};
inline constexpr FaceColor kFairyMembraneInner{246, 202, 252};
inline constexpr FaceColor kFairyFeatherTip{255, 255, 250};
inline constexpr FaceColor kFairyJointInner{128, 220, 210};

// Demon Wings: black frame, crimson glow aura between the separators.
inline constexpr FaceColor kDemonWingsFrame{0, 0, 0};
inline constexpr FaceColor kDemonWingsLead{120, 6, 10};
inline constexpr FaceColor kDemonWingsBack{190, 8, 12};
inline constexpr FaceColor kDemonWingsFront{236, 26, 24};
inline constexpr FaceColor kDemonWingsTrail{84, 2, 6};
inline constexpr FaceColor kDemonWingsTip{255, 76, 48};
inline constexpr FaceColor kDemonWingsGlow{255, 52, 34};
inline constexpr FaceColor kDemonWingsMembraneOuter{190, 8, 12};
inline constexpr FaceColor kDemonWingsMembraneInner{236, 26, 24};
inline constexpr FaceColor kDemonWingsFeatherTip{255, 76, 48};
inline constexpr FaceColor kDemonWingsJointInner{44, 0, 4};

// Vampire Bat: darker, narrower crimson membrane.
inline constexpr FaceColor kVampireFrame{0, 0, 0};
inline constexpr FaceColor kVampireLead{84, 0, 6};
inline constexpr FaceColor kVampireBack{150, 0, 10};
inline constexpr FaceColor kVampireFront{186, 14, 18};
inline constexpr FaceColor kVampireTrail{56, 0, 4};
inline constexpr FaceColor kVampireTip{232, 36, 40};
inline constexpr FaceColor kVampireGlow{236, 30, 36};
inline constexpr FaceColor kVampireMembraneOuter{150, 0, 10};
inline constexpr FaceColor kVampireMembraneInner{186, 14, 18};
inline constexpr FaceColor kVampireFeatherTip{232, 36, 40};
inline constexpr FaceColor kVampireJointInner{32, 0, 4};

// Red Bat: the most saturated glow of the bat family.
inline constexpr FaceColor kRedBatFrame{10, 0, 2};
inline constexpr FaceColor kRedBatLead{150, 0, 8};
inline constexpr FaceColor kRedBatBack{232, 12, 16};
inline constexpr FaceColor kRedBatFront{255, 46, 40};
inline constexpr FaceColor kRedBatTrail{96, 0, 6};
inline constexpr FaceColor kRedBatTip{255, 96, 72};
inline constexpr FaceColor kRedBatGlow{255, 70, 52};
inline constexpr FaceColor kRedBatMembraneOuter{232, 12, 16};
inline constexpr FaceColor kRedBatMembraneInner{255, 46, 40};
inline constexpr FaceColor kRedBatFeatherTip{255, 96, 72};
inline constexpr FaceColor kRedBatJointInner{48, 0, 6};

// ---------------------------------------------------------------------------
// Style table
// ---------------------------------------------------------------------------
//
// Appended-only: the launcher picks a style by index (see saveConfig), so new
// styles go at the end to keep configs pointing at the same shape.

inline constexpr WingDesign kWingDesigns[] = {
    {
        .id = "dragon", .label = "Dragon",
        .upperPx = 9.5f, .forePx = 9.2f, .handPx = 8.4f,
        .raiseDeg = -8.0f, .elbowDeg = -12.0f, .wristDeg = -8.0f,
        .chordRootPx = 14.5f, .chordTipPx = 10.5f, .chordMidPx = 6.5f, .chordBias = 0.95f,
        .fanDeg = 30.0f, .scallopPx = 3.4f, .fingers = 5,
        .camberPx = 2.0f, .sweepPx = 2.4f, .rootZPx = 2.9f, .flutterPx = 0.85f,
        .feathers = false,
        .frame = kDragonFrame, .lead = kDragonLead, .back = kDragonBack,
        .front = kDragonFront, .trail = kDragonTrail, .tip = kDragonTip,
        .glow = kDragonGlow, .glowStrength = 0.0f, .backLight = 0.10f,
    },
    {
        .id = "angel", .label = "Angel",
        .upperPx = 9.6f, .forePx = 10.2f, .handPx = 10.4f,
        .raiseDeg = 6.0f, .elbowDeg = -2.0f, .wristDeg = -10.0f,
        .chordRootPx = 5.6f, .chordTipPx = 3.6f, .chordMidPx = 8.4f, .chordBias = 1.0f,
        .fanDeg = 14.0f, .scallopPx = 1.0f, .fingers = 4,
        .camberPx = 1.4f, .sweepPx = 1.6f, .rootZPx = 2.9f, .flutterPx = 0.55f,
        .feathers = true, .featherCount = 15, .featherLenPx = 17.5f,
        .featherWidthPx = 4.8f, .featherSpreadDeg = 15.0f, .featherStartU = 0.12f,
        .frame = kAngelFrame, .lead = kAngelLead, .back = kAngelBack,
        .front = kAngelFront, .trail = kAngelTrail, .tip = kAngelTip,
        .glow = kAngelGlow, .glowStrength = 0.12f, .backLight = 0.16f,
    },
    {
        .id = "demon", .label = "Demon",
        .upperPx = 9.8f, .forePx = 9.4f, .handPx = 8.6f,
        .raiseDeg = -10.0f, .elbowDeg = -10.0f, .wristDeg = -6.0f,
        .chordRootPx = 15.0f, .chordTipPx = 10.5f, .chordMidPx = 6.5f, .chordBias = 0.95f,
        .fanDeg = 32.0f, .scallopPx = 3.8f, .fingers = 5,
        .camberPx = 2.2f, .sweepPx = 2.6f, .rootZPx = 2.9f, .flutterPx = 0.9f,
        .feathers = false,
        .frame = kDemonFrame, .lead = kDemonLead, .back = kDemonBack,
        .front = kDemonFront, .trail = kDemonTrail, .tip = kDemonTip,
        .glow = kDemonGlow, .glowStrength = 0.55f, .backLight = 0.30f,
    },
    {
        .id = "bat", .label = "Bat",
        .upperPx = 8.0f, .forePx = 7.6f, .handPx = 6.6f,
        .raiseDeg = -6.0f, .elbowDeg = -14.0f, .wristDeg = -12.0f,
        .chordRootPx = 13.0f, .chordTipPx = 9.2f, .chordMidPx = 5.2f, .chordBias = 0.95f,
        .fanDeg = 28.0f, .scallopPx = 3.8f, .fingers = 4,
        .camberPx = 1.7f, .sweepPx = 2.0f, .rootZPx = 2.9f, .flutterPx = 0.8f,
        .feathers = false,
        .frame = kBatFrame, .lead = kBatLead, .back = kBatBack,
        .front = kBatFront, .trail = kBatTrail, .tip = kBatTip,
        .glow = kBatGlow, .glowStrength = 0.0f, .backLight = 0.08f,
    },
    {
        .id = "butterfly", .label = "Butterfly",
        .upperPx = 9.0f, .forePx = 9.0f, .handPx = 8.0f,
        .raiseDeg = 17.0f, .elbowDeg = -8.0f, .wristDeg = -8.0f,
        .chordRootPx = 14.0f, .chordTipPx = 9.0f, .chordMidPx = 4.0f, .chordBias = 0.9f,
        .fanDeg = 10.0f, .scallopPx = 4.4f, .fingers = 3,
        .camberPx = 2.4f, .sweepPx = 1.4f, .rootZPx = 2.9f, .flutterPx = 1.1f,
        .feathers = false,
        .frame = kButterflyFrame, .lead = kButterflyLead, .back = kButterflyBack,
        .front = kButterflyFront, .trail = kButterflyTrail, .tip = kButterflyTip,
        .glow = kButterflyGlow, .glowStrength = 0.18f, .backLight = 0.28f,
    },
    {
        .id = "phoenix", .label = "Phoenix",
        .upperPx = 10.2f, .forePx = 10.8f, .handPx = 10.6f,
        .raiseDeg = 11.0f, .elbowDeg = -2.0f, .wristDeg = -10.0f,
        .chordRootPx = 5.4f, .chordTipPx = 3.6f, .chordMidPx = 9.8f, .chordBias = 1.05f,
        .fanDeg = 16.0f, .scallopPx = 1.3f, .fingers = 4,
        .camberPx = 1.6f, .sweepPx = 2.0f, .rootZPx = 2.9f, .flutterPx = 0.9f,
        .feathers = true, .featherCount = 15, .featherLenPx = 17.5f,
        .featherWidthPx = 4.2f, .featherSpreadDeg = 18.0f, .featherStartU = 0.13f,
        .frame = kPhoenixFrame, .lead = kPhoenixLead, .back = kPhoenixBack,
        .front = kPhoenixFront, .trail = kPhoenixTrail, .tip = kPhoenixTip,
        .glow = kPhoenixGlow, .glowStrength = 0.26f, .backLight = 0.34f,
    },
    {
        .id = "fairy", .label = "Fairy",
        .upperPx = 8.0f, .forePx = 8.4f, .handPx = 7.8f,
        .raiseDeg = 15.0f, .elbowDeg = 0.0f, .wristDeg = -14.0f,
        .chordRootPx = 8.4f, .chordTipPx = 4.6f, .chordMidPx = 7.0f, .chordBias = 1.0f,
        .fanDeg = 14.0f, .scallopPx = 2.2f, .fingers = 3,
        .camberPx = 1.9f, .sweepPx = 1.4f, .rootZPx = 2.9f, .flutterPx = 1.2f,
        .feathers = true, .featherCount = 12, .featherLenPx = 13.5f,
        .featherWidthPx = 3.8f, .featherSpreadDeg = 34.0f, .featherStartU = 0.22f,
        .frame = kFairyFrame, .lead = kFairyLead, .back = kFairyBack,
        .front = kFairyFront, .trail = kFairyTrail, .tip = kFairyTip,
        .glow = kFairyGlow, .glowStrength = 0.20f, .backLight = 0.34f,
    },
    {
        .id = "demon_wings", .label = "Demon Wings",
        .upperPx = 10.2f, .forePx = 9.8f, .handPx = 9.0f,
        .raiseDeg = -10.0f, .elbowDeg = -9.0f, .wristDeg = -6.0f,
        .chordRootPx = 15.5f, .chordTipPx = 11.0f, .chordMidPx = 6.8f, .chordBias = 0.95f,
        .fanDeg = 32.0f, .scallopPx = 4.0f, .fingers = 5,
        .camberPx = 2.3f, .sweepPx = 2.8f, .rootZPx = 2.9f, .flutterPx = 1.0f,
        .feathers = false,
        .frame = kDemonWingsFrame, .lead = kDemonWingsLead, .back = kDemonWingsBack,
        .front = kDemonWingsFront, .trail = kDemonWingsTrail, .tip = kDemonWingsTip,
        .glow = kDemonWingsGlow, .glowStrength = 0.62f, .backLight = 0.42f,
    },
    {
        .id = "vampire", .label = "Vampire Bat",
        .upperPx = 8.4f, .forePx = 8.0f, .handPx = 7.0f,
        .raiseDeg = -8.0f, .elbowDeg = -14.0f, .wristDeg = -10.0f,
        .chordRootPx = 13.5f, .chordTipPx = 9.6f, .chordMidPx = 5.4f, .chordBias = 0.95f,
        .fanDeg = 30.0f, .scallopPx = 4.2f, .fingers = 4,
        .camberPx = 1.9f, .sweepPx = 2.2f, .rootZPx = 2.9f, .flutterPx = 0.9f,
        .feathers = false,
        .frame = kVampireFrame, .lead = kVampireLead, .back = kVampireBack,
        .front = kVampireFront, .trail = kVampireTrail, .tip = kVampireTip,
        .glow = kVampireGlow, .glowStrength = 0.48f, .backLight = 0.36f,
    },
    {
        .id = "red_bat", .label = "Red Bat",
        .upperPx = 7.6f, .forePx = 7.2f, .handPx = 6.4f,
        .raiseDeg = -6.0f, .elbowDeg = -15.0f, .wristDeg = -12.0f,
        .chordRootPx = 12.5f, .chordTipPx = 9.0f, .chordMidPx = 5.0f, .chordBias = 0.95f,
        .fanDeg = 28.0f, .scallopPx = 4.2f, .fingers = 4,
        .camberPx = 1.8f, .sweepPx = 2.0f, .rootZPx = 2.9f, .flutterPx = 0.9f,
        .feathers = false,
        .frame = kRedBatFrame, .lead = kRedBatLead, .back = kRedBatBack,
        .front = kRedBatFront, .trail = kRedBatTrail, .tip = kRedBatTip,
        .glow = kRedBatGlow, .glowStrength = 0.58f, .backLight = 0.40f,
    },
    {
        // Modelled on the author's in-game reference. The defining features are
        // a dead-flat top edge that runs straight out over the shoulders and a
        // membrane that is deepest at the root, tapering to a point outboard -
        // the opposite of a bat sail, whose chord is longest at the tip.
        //
        // This needs an arm that stays horizontal (raise 0, elbow 0, wrist 0 in
        // this design's own terms, which is what puts the leading edge on a
        // level line) plus a falling chord (chordTipPx < chordRootPx) so the
        // trailing edge sweeps up towards the tip instead of hanging down.
        .id = "veil", .label = "Veil",
        .upperPx = 12.0f, .forePx = 12.2f, .handPx = 11.0f,
        .raiseDeg = 0.0f, .elbowDeg = 0.0f, .wristDeg = 0.0f,
        .chordRootPx = 17.0f, .chordTipPx = 2.4f, .chordMidPx = 1.0f, .chordBias = 2.30f,
        .fanDeg = 5.0f, .scallopPx = 2.0f, .fingers = 4,
        .camberPx = 0.5f, .sweepPx = 0.8f, .rootZPx = 3.1f, .flutterPx = 0.22f,
        .ribs = false, .fairing = false,
        .feathers = false,
        .frame = kVeilFrame, .lead = kVeilLead, .back = kVeilBack,
        .front = kVeilFront, .trail = kVeilTrail, .tip = kVeilTip,
        .glow = kVeilGlow, .glowStrength = 0.0f, .backLight = 0.05f,
    },
};

struct WingStyle {
    const char* id;
    const char* label;
    int boneCount;          // kept for source compatibility; unused by the surface renderer
    const WingDesign* design;
};

inline constexpr int kWingDesignCount = static_cast<int>(sizeof(kWingDesigns) / sizeof(kWingDesigns[0]));
inline constexpr int kWingStyleCount = kWingDesignCount;

inline int wingStyleIndexForId(const std::string& id) {
    for (int i = 0; i < kWingStyleCount; ++i) {
        if (id == kWingDesigns[i].id) return i;
    }
    return 0;  // dragon default
}

// Serializes the picker value in the launcher's radio format:
// "<selectedIndex>,<id1>,<id2>,...".
inline std::string wingStyleRadioValue(int index) {
    if (index < 0 || index >= kWingStyleCount) index = 0;
    std::string value = std::to_string(index);
    for (int i = 0; i < kWingStyleCount; ++i) {
        value += ',';
        value += kWingDesigns[i].id;
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

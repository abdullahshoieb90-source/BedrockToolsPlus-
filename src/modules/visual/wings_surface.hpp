#pragma once

// Surface builder for the Wings module.
//
// The old overlay drew each wing as seven disjoint tapered boxes, which read as
// a comb of planks: gaps between the "feathers", flat unshaded faces and no
// silhouette. This builder instead produces one continuous membrane - a
// parametric sheet hanging off an articulated arm - plus the structural detail
// that makes it look like a wing:
//
//   * a smooth Catmull-Rom leading edge driven by the shoulder / elbow / wrist
//     animation angles,
//   * a curved membrane with a swept, scalloped trailing edge (one concave bite
//     per finger, exactly like a bat or dragon wing),
//   * backward camber and a travelling flutter wave, so the sheet has volume
//     and reacts to the flap instead of being a flat card,
//   * finger struts, a leading-edge spar with a bright rim, a wing-tip claw and
//     a shoulder fairing, all built as thin 3D ribbons so they never turn into
//     single-pixel slivers at grazing angles,
//   * for the feathered styles, long layered quills with a colour gradient to
//     the tip instead of the membrane scallops.
//
// The builder is pure: it takes a WingDesign + a pose and fills an array of
// WingQuad in Bedrock model pixels. No Minecraft headers, so the host tests and
// tools/wings_preview.cpp rasterize the very same quads the game submits.
//
// Every quad carries both palette sides (`back` = seen from behind the player,
// `front` = seen from the body side) plus the surface normal along +z, so the
// renderer picks a side from where the camera is, not from the sign of an
// interpolated normal - that keeps the sheet from turning into a patchwork of
// front/back colours on a curved surface.

#include "modules/visual/wings_design.hpp"
#include "modules/visual/wings_shape.hpp"

#include <cmath>
#include <cstddef>

namespace bedrocktools::modules::wings {

// ---------------------------------------------------------------------------
// Pose
// ---------------------------------------------------------------------------

// Animation input for the builder. The angles are the module's raise-positive
// degrees (positive lifts the wing tip), already blended between the idle, flap
// and glide envelopes by WingsModule.
struct WingPose {
    float shoulderDeg = 0.0f;
    float elbowDeg = 0.0f;
    float wristDeg = 0.0f;
    float phaseRad = 0.0f;   // flap phase; drives the membrane flutter
    float flutter = 0.0f;    // 0 = still air, 1 = full flutter
};

// Maps the animation controller's angles onto the builder's pose. The flap
// intensity becomes the membrane flutter and the glide pose smooths it out, so
// the sheet ripples while flapping and goes taut while gliding. Kept here (and
// not in the module) so the host tests can pin the mapping down.
inline WingPose makeWingPose(float shoulderDeg, float upperDeg, float tipDeg,
                             float phaseRad, float intensity, float glide) {
    WingPose pose;
    pose.shoulderDeg = shoulderDeg;
    pose.elbowDeg = upperDeg;
    pose.wristDeg = tipDeg;
    pose.phaseRad = phaseRad;
    pose.flutter = (0.22f + 0.78f * clamp01(intensity)) * (1.0f - 0.75f * clamp01(glide));
    return pose;
}

// Which palette side the camera sees. Model +z is the player's back, so a camera
// behind the player sees `back`. Deriving it from the player position (instead
// of from an interpolated normal) keeps a curved sheet from turning into a
// front/back patchwork.
inline bool cameraSeesBackSide(const Vec3& cam, const Vec3& bodyCenter, const Vec3& right,
                               const Vec3& forward) {
    const Vec3 backDir = modelDirToWorld({0.0f, 0.0f, 1.0f}, right, forward);
    return dot(backDir, cam - bodyCenter) > 0.0f;
}

// ---------------------------------------------------------------------------
// Output quad
// ---------------------------------------------------------------------------

// One double-sided quad in model space. `n` is the sheet normal in model space;
// its z is always >= 0, i.e. it points towards the back of the player.
struct WingQuad {
    Vec3 p[4];
    Vec3 n;
    FaceColor front;       // nominal palette for the body-facing side
    FaceColor back;        // nominal palette for the camera-behind side
    // Per-corner colours. The overlay submits these with the tessellator, so a
    // cell of the sail gets a real gradient across it instead of one flat tone
    // (which is what made the old wings look like painted cardboard).
    FaceColor frontC[4];
    FaceColor backC[4];
    float spanT = 0.5f;    // 0 at the shoulder .. 1 at the wing tip
    float emissive = 0.0f; // minimum brightness (glow accents), 0 = fully lit
};

constexpr int kMaxWingQuads = 700;
constexpr int kMembraneSpanSegments = 20;
constexpr int kMembraneChordSegments = 6;
constexpr int kSparSegments = 9;
constexpr int kFeatherSegments = 3;
constexpr int kFeatherCount = 18;
constexpr int kFairingSegments = 6;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

inline Vec3 rotateXY(const Vec3& v, float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return {v.x * c - v.y * s, v.x * s + v.y * c, v.z};
}

// Uniform Catmull-Rom segment between p1 and p2.
inline Vec3 catmullRom(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 +
            (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) * 0.5f;
}

// ---------------------------------------------------------------------------
// Arm (leading edge)
// ---------------------------------------------------------------------------

// Shoulder, elbow, wrist and wing-tip positions of the right wing.
inline void buildArmJoints(const WingDesign& d, const WingPose& pose, Vec3 joint[4]) {
    const float a0 = (d.raiseDeg + pose.shoulderDeg) * kDegToRad;
    const float a1 = a0 + (d.elbowDeg + pose.elbowDeg) * kDegToRad;
    const float a2 = a1 + (d.wristDeg + pose.wristDeg) * kDegToRad;

    joint[0] = Vec3{kRightRootPivotX, kRootPivotY, d.rootZPx};
    joint[1] = Vec3{joint[0].x - std::cos(a0) * d.upperPx, joint[0].y + std::sin(a0) * d.upperPx, joint[0].z};
    joint[2] = Vec3{joint[1].x - std::cos(a1) * d.forePx, joint[1].y + std::sin(a1) * d.forePx, joint[1].z};
    joint[3] = Vec3{joint[2].x - std::cos(a2) * d.handPx, joint[2].y + std::sin(a2) * d.handPx, joint[2].z};
}

// Point on the leading edge at u in [0, 1] (0 = shoulder, 1 = wing tip).
inline Vec3 leadingEdgePoint(const Vec3 joint[4], const WingDesign& d, float u) {
    const float t = clamp01(u) * 3.0f;
    int seg = static_cast<int>(t);
    if (seg > 2) seg = 2;
    if (seg < 0) seg = 0;
    const float lt = t - static_cast<float>(seg);
    const Vec3& p0 = joint[seg == 0 ? 0 : seg - 1];
    const Vec3& p1 = joint[seg];
    const Vec3& p2 = joint[seg + 1];
    const Vec3& p3 = joint[seg + 2 <= 3 ? seg + 2 : 3];
    Vec3 p = catmullRom(p0, p1, p2, p3, lt);
    p.z = d.rootZPx + d.sweepPx * u * u;
    return p;
}

// Unit tangent of the leading edge in the wing plane.
inline Vec3 leadingEdgeTangent(const Vec3 joint[4], const WingDesign& d, float u) {
    const float e = 0.02f;
    const Vec3 a = leadingEdgePoint(joint, d, clampf(u - e, 0.0f, 1.0f));
    const Vec3 b = leadingEdgePoint(joint, d, clampf(u + e, 0.0f, 1.0f));
    return normalized(Vec3{b.x - a.x, b.y - a.y, 0.0f});
}

// ---------------------------------------------------------------------------
// Membrane shape
// ---------------------------------------------------------------------------

// Trailing-edge length at u: the chord between the arm and the sail's edge,
// with a concave scallop carved out between two finger struts.
inline float chordLength(const WingDesign& d, float u) {
    const float t = std::pow(clamp01(u), d.chordBias > 0.05f ? d.chordBias : 1.0f);
    float c = d.chordRootPx + (d.chordTipPx - d.chordRootPx) * t;
    // Mid-span bulge: bird and butterfly wings are widest in the middle, bat
    // and dragon wings are widest at the shoulder (chordMidPx = 0).
    if (d.chordMidPx != 0.0f) c += d.chordMidPx * std::sin(kPi * clamp01(u));

    const int fingers = d.fingers < 1 ? 1 : d.fingers;
    const float scaled = u * static_cast<float>(fingers);
    const float fract = scaled - std::floor(scaled);
    const float bite = std::sin(kPi * fract);
    const float ramp = smoothstep(0.06f, 0.30f, u);
    c -= d.scallopPx * std::pow(std::fabs(bite), 1.45f) * ramp;

    return c < 0.6f ? 0.6f : c;
}

// Direction the membrane hangs in: perpendicular to the arm, swept inboard by
// the fan angle at the tip (a bat wing's trailing edge runs back towards the
// body, not straight down).
inline Vec3 chordDirection(const WingDesign& d, float u, const Vec3& tangent) {
    const Vec3 t = normalized(Vec3{tangent.x, tangent.y, 0.0f});
    // Right wing spans -x, so "down" from an outward tangent is (-t.y, t.x).
    Vec3 n{-t.y, t.x, 0.0f};
    if (n.y > 0.0f) n = n * -1.0f;  // always hang downwards
    return rotateXY(n, d.fanDeg * u * kDegToRad);
}

// Backwards bow of the sheet plus the travelling flutter wave.
inline float membraneDepth(const WingDesign& d, const WingPose& pose, float u, float v) {
    const float vv = clamp01(v);
    const float bow = d.camberPx * std::sin(kPi * vv) * (1.0f - 0.45f * clamp01(u));
    const float wave = d.flutterPx * clamp01(pose.flutter) *
        std::sin(pose.phaseRad + clamp01(u) * 5.2f + vv * 1.9f) * clamp01(u) *
        (0.35f + 0.65f * vv);
    return bow + wave;
}

// Point on the membrane at (u along the span, v along the chord).
inline Vec3 membranePoint(const Vec3 joint[4], const WingDesign& d, const WingPose& pose,
                          float u, float v) {
    const Vec3 le = leadingEdgePoint(joint, d, u);
    const Vec3 tangent = leadingEdgeTangent(joint, d, u);
    const Vec3 dir = chordDirection(d, u, tangent);
    const Vec3 p = le + dir * (chordLength(d, u) * clamp01(v));
    return Vec3{p.x, p.y, p.z + membraneDepth(d, pose, u, v)};
}

// ---------------------------------------------------------------------------
// Membrane colours
// ---------------------------------------------------------------------------

// Seen from behind the player: bright leading band, deep trailing shadow and a
// slight extra darkness at the shoulder, so the sail reads as a curved surface
// even before the lighting model touches it.
inline FaceColor membraneBackColor(const WingDesign& d, float u, float v) {
    FaceColor c = lerpColor(d.back, d.trail, std::pow(clamp01(v), 0.85f) * 0.92f);
    c = lerpColor(c, d.lead, (1.0f - smoothstep(0.0f, 0.28f, v)) * 0.50f);
    // Inboard shadow: a membrane hangs off the back, so the sheet is brightest
    // where it overhangs the light and darkest at the shoulder.
    c = lerpColor(c, d.trail, (1.0f - smoothstep(0.03f, 0.42f, u)) * 0.42f);
    return c;
}

// Seen from in front of the player. This is the side the body shadows, so it is
// deliberately a darker, flatter version of the palette: no bright leading
// highlight and no tip colour. Solving it that way keeps the wings readable
// from a front-facing camera without turning them into a pale sheet that
// competes with the sky (backLight only softens how dark it gets).
inline FaceColor membraneFrontColor(const WingDesign& d, float u, float v) {
    const float soften = 1.0f - 0.22f * clamp01(d.backLight);
    FaceColor c = lerpColor(d.front, d.trail, 0.42f + 0.34f * clamp01(v));
    // A hint of the leading edge, heavily damped: enough to show the arm line,
    // not enough to blow out.
    c = lerpColor(c, d.lead, (1.0f - smoothstep(0.0f, 0.26f, v)) * 0.16f);
    c = shadeFace(c, soften);
    c = lerpColor(c, d.trail, (1.0f - smoothstep(0.03f, 0.40f, u)) * 0.30f);
    return c;
}

inline float membraneEmissive(const WingDesign& d, float v) {
    if (d.glowStrength <= 0.0f) return 0.0f;
    return d.glowStrength * (0.62f + 0.38f * (1.0f - clamp01(v)));
}

// ---------------------------------------------------------------------------
// Mesh sink
// ---------------------------------------------------------------------------

namespace detail {

struct Sink {
    WingQuad* out;
    int cap;
    int count = 0;

    // Adds one planar quad. The winding is normalized so the derived normal
    // always points towards the back of the player (+z).
    inline void add(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
                    const FaceColor& front, const FaceColor& back,
                    float spanT, float emissive) {
        if (count >= cap) return;
        Vec3 n = normalized(cross(b - a, d - a));
        WingQuad& q = out[count++];
        if (n.z < 0.0f) {
            // Reverse the ring so every quad winds the same way.
            q.p[0] = d; q.p[1] = c; q.p[2] = b; q.p[3] = a;
            n = n * -1.0f;
        } else {
            q.p[0] = a; q.p[1] = b; q.p[2] = c; q.p[3] = d;
        }
        q.n = n;
        q.front = front;
        q.back = back;
        for (int i = 0; i < 4; ++i) {
            q.frontC[i] = front;
            q.backC[i] = back;
        }
        q.spanT = spanT;
        q.emissive = emissive;
    }

    // Same as add(), but with one colour per corner (Gouraud shading).
    inline void addShaded(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
                          const FaceColor& frontA, const FaceColor& frontB,
                          const FaceColor& frontC, const FaceColor& frontD,
                          const FaceColor& backA, const FaceColor& backB,
                          const FaceColor& backC, const FaceColor& backD,
                          float spanT, float emissive) {
        add(a, b, c, d, frontA, backA, spanT, emissive);
        WingQuad& q = out[count - 1];
        // add() may have reversed the ring to keep the normal pointing back.
        const bool reversed = !(q.p[0].x == a.x && q.p[0].y == a.y && q.p[0].z == a.z);
        if (reversed) {
            q.frontC[0] = frontD; q.frontC[1] = frontC; q.frontC[2] = frontB; q.frontC[3] = frontA;
            q.backC[0] = backD;   q.backC[1] = backC;   q.backC[2] = backB;   q.backC[3] = backA;
        } else {
            q.frontC[0] = frontA; q.frontC[1] = frontB; q.frontC[2] = frontC; q.frontC[3] = frontD;
            q.backC[0] = backA;   q.backC[1] = backB;   q.backC[2] = backC;   q.backC[3] = backD;
        }
    }

    // A flat tapered blade: one double-sided quad strip that reads as a feather
    // vane. Feathers must not be solid beams - their edge faces are what turned
    // the previous feather layer into a pile of shards - so they use this.
    inline void blade(const Vec3& a, const Vec3& b, float halfWidthA, float halfWidthB,
                      const Vec3& planeNormal, const FaceColor& frontA, const FaceColor& frontB,
                      const FaceColor& backA, const FaceColor& backB, float spanT, float emissive) {
        const Vec3 dir = b - a;
        if (length(dir) < 1e-4f) return;
        Vec3 side = normalized(cross(normalized(dir), normalized(planeNormal)));
        if (length(side) < 0.5f) side = Vec3{1.0f, 0.0f, 0.0f};
        addShaded(a + side * halfWidthA, b + side * halfWidthB, b - side * halfWidthB,
                  a - side * halfWidthA, frontA, frontB, frontB, frontA, backA, backB, backB,
                  backA, spanT, emissive);
    }

    // A tapered ribbon with real thickness: a back face, a front face and the
    // two long edges. Four quads per beam keep struts and spars readable from
    // every camera angle instead of vanishing into a sliver.
    inline void beam(const Vec3& a, const Vec3& b, float halfWidthA, float halfWidthB,
                     float halfThick, const Vec3& planeNormal, const FaceColor& face,
                     const FaceColor& edge, float spanT, float emissive) {
        const Vec3 dir = b - a;
        if (length(dir) < 1e-3f) return;
        Vec3 side = normalized(cross(normalized(dir), normalized(planeNormal)));
        if (length(side) < 0.5f) side = Vec3{0.0f, 0.0f, 1.0f};
        const Vec3 up = normalized(planeNormal);

        const Vec3 a0 = a + side * halfWidthA;
        const Vec3 b0 = b + side * halfWidthB;
        const Vec3 b1 = b - side * halfWidthB;
        const Vec3 a1 = a - side * halfWidthA;

        const Vec3 a0f = a0 + up * halfThick, b0f = b0 + up * halfThick;
        const Vec3 b1f = b1 + up * halfThick, a1f = a1 + up * halfThick;
        const Vec3 a0b = a0 - up * halfThick, b0b = b0 - up * halfThick;
        const Vec3 b1b = b1 - up * halfThick, a1b = a1 - up * halfThick;

        add(a0f, b0f, b1f, a1f, face, face, spanT, emissive);  // outward face
        add(a0b, b0b, b1b, a1b, face, face, spanT, emissive);  // inward face
        add(a0f, b0f, b0b, a0b, edge, edge, spanT, emissive);  // long edge
        add(a1f, b1f, b1b, a1b, edge, edge, spanT, emissive);  // long edge
    }
};

// ---------------------------------------------------------------------------
// Parts
// ---------------------------------------------------------------------------

// Rounded shoulder plate plus a rim, so the wing grows out of the back instead
// of floating next to it.
inline void addFairing(Sink& sink, const Vec3 joint[4], const WingDesign& d, const WingPose& pose) {
    const Vec3 c = joint[0];
    const float radius = 2.2f;
    const float start = (d.raiseDeg + pose.shoulderDeg) * kDegToRad + 45.0f * kDegToRad;
    const float sweep = 200.0f * kDegToRad;

    Vec3 rim[kFairingSegments + 1];
    for (int i = 0; i <= kFairingSegments; ++i) {
        const float a = start + sweep * (static_cast<float>(i) / kFairingSegments);
        rim[i] = Vec3{c.x - std::cos(a) * radius, c.y + std::sin(a) * radius, c.z + 0.55f};
    }

    // Plate: an annulus between a small inner arc and the outer rim, so every
    // segment is a real quad (a fan of degenerate triangles would break the
    // normal/planarity guarantees the renderer and the tests rely on).
    const float innerRadius = radius * 0.42f;
    for (int i = 0; i < kFairingSegments; ++i) {
        const float a0 = start + sweep * (static_cast<float>(i) / kFairingSegments);
        const float a1 = start + sweep * (static_cast<float>(i + 1) / kFairingSegments);
        const Vec3 inner0{c.x - std::cos(a0) * innerRadius, c.y + std::sin(a0) * innerRadius, c.z + 0.55f};
        const Vec3 inner1{c.x - std::cos(a1) * innerRadius, c.y + std::sin(a1) * innerRadius, c.z + 0.55f};
        const float t = static_cast<float>(i) / kFairingSegments;
        const FaceColor outer = lerpColor(d.frame, d.lead, 0.20f + 0.30f * t);
        const FaceColor inner = lerpColor(d.trail, d.frame, 0.55f);
        sink.add(inner0, rim[i], rim[i + 1], inner1, inner, outer, 0.0f, d.glowStrength * 0.18f);
    }

    // Rim band: gives the joint visible thickness.
    for (int i = 0; i < kFairingSegments; ++i) {
        const FaceColor col = lerpColor(d.frame, d.trail, 0.30f);
        const Vec3 a0 = rim[i] + Vec3{0.0f, 0.0f, 0.75f};
        const Vec3 b0 = rim[i + 1] + Vec3{0.0f, 0.0f, 0.75f};
        const Vec3 b1 = rim[i + 1] - Vec3{0.0f, 0.0f, 0.75f};
        const Vec3 a1 = rim[i] - Vec3{0.0f, 0.0f, 0.75f};
        sink.add(a0, b0, b1, a1, col, col, 0.0f, d.glowStrength * 0.12f);
    }
}

// The sail itself.
inline void addMembrane(Sink& sink, const Vec3 joint[4], const WingDesign& d, const WingPose& pose) {
    constexpr int cols = kMembraneChordSegments + 1;
    Vec3 grid[(kMembraneSpanSegments + 1) * cols];
    for (int i = 0; i <= kMembraneSpanSegments; ++i) {
        const float u = static_cast<float>(i) / kMembraneSpanSegments;
        for (int j = 0; j <= kMembraneChordSegments; ++j) {
            const float v = static_cast<float>(j) / kMembraneChordSegments;
            grid[i * cols + j] = membranePoint(joint, d, pose, u, v);
        }
    }

    for (int i = 0; i < kMembraneSpanSegments; ++i) {
        const float u0 = static_cast<float>(i) / kMembraneSpanSegments;
        const float u1 = static_cast<float>(i + 1) / kMembraneSpanSegments;
        const float uc = (u0 + u1) * 0.5f;
        for (int j = 0; j < kMembraneChordSegments; ++j) {
            const float v0 = static_cast<float>(j) / kMembraneChordSegments;
            const float v1 = static_cast<float>(j + 1) / kMembraneChordSegments;
            const float vc = (v0 + v1) * 0.5f;
            const Vec3& a = grid[i * cols + j];
            const Vec3& b = grid[(i + 1) * cols + j];
            const Vec3& c2 = grid[(i + 1) * cols + j + 1];
            const Vec3& d2 = grid[i * cols + j + 1];
            // Evaluate the gradient at the four corners, so the shading runs
            // smoothly from the leading edge to the trailing shadow and from
            // the shoulder out to the tip.
            sink.addShaded(a, b, c2, d2,
                           membraneFrontColor(d, u0, v0), membraneFrontColor(d, u1, v0),
                           membraneFrontColor(d, u1, v1), membraneFrontColor(d, u0, v1),
                           membraneBackColor(d, u0, v0), membraneBackColor(d, u1, v0),
                           membraneBackColor(d, u1, v1), membraneBackColor(d, u0, v1),
                           uc, membraneEmissive(d, vc));
        }
    }
}

// Finger struts: they divide the scalloped trailing edge into the classic bat /
// dragon "fingers" and stop exactly on the membrane edge, so nothing pokes out.
// Each strut is a chain of short beams that follows the cambered sheet, so the
// bulge of the membrane can never swallow it.
inline void addRibs(Sink& sink, const Vec3 joint[4], const WingDesign& d, const WingPose& pose) {
    constexpr int kRibSegments = 4;
    const int fingers = d.fingers < 1 ? 1 : d.fingers;
    for (int k = 0; k <= fingers; ++k) {
        const float u = clampf(static_cast<float>(k) / static_cast<float>(fingers), 0.03f, 0.982f);
        const float w = lerpFloat(0.34f, 0.16f, u);
        const FaceColor col = lerpColor(d.frame, d.trail, 0.42f - 0.22f * u);
        Vec3 prev = membranePoint(joint, d, pose, u, 0.03f) + Vec3{0.0f, 0.0f, 0.32f};
        for (int s = 1; s <= kRibSegments; ++s) {
            const float v = 0.03f + 0.955f * (static_cast<float>(s) / kRibSegments);
            const Vec3 cur = membranePoint(joint, d, pose, u, v) + Vec3{0.0f, 0.0f, 0.32f};
            const float w1 = w * lerpFloat(1.0f, 0.62f, static_cast<float>(s) / kRibSegments);
            sink.beam(prev, cur, w, w1, 0.08f, Vec3{0.0f, 0.0f, 1.0f}, col, d.trail, u * 0.85f,
                      d.glowStrength * 0.65f);
            prev = cur;
        }
    }
}

// Leading-edge spar with a bright rim, plus the claw at the wing tip.
inline void addSpar(Sink& sink, const Vec3 joint[4], const WingDesign& d, const WingPose& pose) {
    const FaceColor rimColor = d.glowStrength > 0.0f ? lerpColor(d.glow, d.tip, 0.30f)
                                                     : lerpColor(d.lead, d.tip, 0.35f);
    Vec3 prev = membranePoint(joint, d, pose, 0.012f, 0.0f);
    for (int i = 1; i <= kSparSegments; ++i) {
        const float u = 0.012f + 0.988f * (static_cast<float>(i) / kSparSegments);
        const Vec3 cur = membranePoint(joint, d, pose, u, 0.0f);
        const float w0 = lerpFloat(0.86f, 0.34f, clamp01((u - 0.012f) / 0.988f));
        const float w1 = lerpFloat(0.86f, 0.34f, clamp01(u));
        sink.beam(prev, cur, w0, w1, 0.26f, Vec3{0.0f, 0.0f, 1.0f}, d.frame, d.trail, u,
                  d.glowStrength * 0.85f);
        // Rim: a thinner, brighter strip sitting on the outer side of the spar,
        // so the silhouette always keeps a highlight along the leading edge.
        sink.beam(prev + Vec3{0.0f, 0.0f, 0.36f}, cur + Vec3{0.0f, 0.0f, 0.36f},
                  w0 * 0.24f, w1 * 0.24f, 0.08f, Vec3{0.0f, 0.0f, 1.0f}, rimColor, rimColor, u,
                  d.glowStrength > 0.0f ? 0.70f : 0.24f);
        prev = cur;
    }

    // Claw: a short spike continuing the arm past the wing tip.
    const Vec3 tip = membranePoint(joint, d, pose, 1.0f, 0.0f);
    const Vec3 tangent = leadingEdgeTangent(joint, d, 1.0f);
    const Vec3 claw = tip + tangent * (d.handPx * 0.22f) + Vec3{0.0f, 0.0f, 0.30f};
    sink.beam(tip, claw, 0.60f, 0.10f, 0.26f, Vec3{0.0f, 0.0f, 1.0f}, d.tip, d.frame, 1.0f,
              d.glowStrength * 0.70f);
}

// Feathered styles: three overlapping rows of quills laid over the sheet.
//
// Rows run parallel to the arm: short coverts near the leading edge, medium
// secondaries and long primaries on the trailing edge. Each row is offset down
// the chord so it starts where the row above ends, and the quills inside a row
// overlap by roughly half a width - that is what reads as one layered feather
// mass instead of a comb of separate blades.
//
// Two details matter for the silhouette: the quill lengths follow one smooth
// dome (a per-quill sawtooth reads as a torn sheet, not as feathers), and every
// vane keeps its width for most of its length before it rounds off, so the tips
// form an arc instead of a row of needles.
inline void addFeathers(Sink& sink, const Vec3 joint[4], const WingDesign& d, const WingPose& pose) {
    constexpr int kRows = 3;
    int n = d.featherCount;
    if (n < 4) n = 4;
    if (n > kFeatherCount) n = kFeatherCount;

    const float spread = d.featherSpreadDeg * kDegToRad;
    const FaceColor root = lerpColor(d.back, d.front, 0.55f);
    const FaceColor rootFront = lerpColor(d.front, d.lead, 0.18f);
    const float ceiling = d.camberPx + d.flutterPx * clamp01(pose.flutter);

    for (int row = 0; row < kRows; ++row) {
        const float rt = static_cast<float>(row) / static_cast<float>(kRows - 1);
        // Where this row is rooted along the chord, and how long its quills are.
        const float vRoot = lerpFloat(0.14f, 0.52f, rt);
        const float lenScale = lerpFloat(0.54f, 1.0f, rt);
        const float widthScale = lerpFloat(1.0f, 0.88f, rt);
        const float layer = ceiling + 0.20f + 0.34f * rt;

        for (int i = 0; i < n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n - 1);
            const float u = lerpFloat(clamp01(d.featherStartU), 0.985f, t);

            const Vec3 anchor = membranePoint(joint, d, pose, u, vRoot);
            const Vec3 tangent = leadingEdgeTangent(joint, d, u);
            Vec3 dir = chordDirection(d, u, tangent);
            dir = rotateXY(dir, spread * (t - 0.55f) * 0.75f);
            const float curl = (9.0f + 15.0f * t + 7.0f * rt) * kDegToRad;

            // One smooth dome over the whole row: longest around 60% of the
            // span, shorter at the shoulder and at the wing tip.
            const float dome = 0.58f + 0.42f * std::sin(kPi * clamp01(0.10f + 0.80f * t));
            const float len = d.featherLenPx * lenScale * dome;
            const float wRoot = d.featherWidthPx * widthScale * 0.5f;
            // Per-row tone and a very slight quill alternation: enough to keep
            // neighbouring vanes apart without turning into a checkerboard.
            const float rowTone[3] = {1.16f, 1.02f, 0.90f};
            const float tint = rowTone[row] * (0.98f + 0.04f * static_cast<float>((i + row) % 2));
            const float tilt = (t - 0.5f) * 0.85f - 0.25f;

            Vec3 pts[kFeatherSegments + 1];
            pts[0] = anchor;
            Vec3 p = anchor;
            for (int s = 1; s <= kFeatherSegments; ++s) {
                const float k = static_cast<float>(s) / kFeatherSegments;
                p = p + rotateXY(dir, curl * k * k) * (len / kFeatherSegments);
                p.z = anchor.z + layer + 0.26f * k * k + tilt * k;
                pts[s] = p;
            }

            for (int s = 0; s < kFeatherSegments; ++s) {
                const float k0 = static_cast<float>(s) / kFeatherSegments;
                const float k1 = static_cast<float>(s + 1) / kFeatherSegments;
                // Full-bodied vane, rounded off in the last third.
                const float w0 = wRoot * (1.0f - 0.94f * std::pow(k0, 2.2f));
                const float w1 = wRoot * (1.0f - 0.94f * std::pow(k1, 2.2f));
                FaceColor f0 = shadeFace(lerpColor(rootFront, d.tip, std::pow(k0, 1.7f)), tint);
                FaceColor f1 = shadeFace(lerpColor(rootFront, d.tip, std::pow(k1, 1.7f)), tint);
                FaceColor b0 = shadeFace(lerpColor(root, d.trail, 0.30f + 0.25f * k0), tint);
                FaceColor b1 = shadeFace(lerpColor(root, d.trail, 0.30f + 0.25f * k1), tint);
                // Front (body-facing) side: the same vane seen in shadow, so it
                // stays a step darker than the back instead of glowing gold.
                FaceColor f0Shadow = shadeFace(b0, 0.92f);
                FaceColor f1Shadow = shadeFace(b1, 0.92f);
                // Dark rim on the last segment: the shadow each quill casts on
                // the row below is what makes the feather layers visible.
                if (s == kFeatherSegments - 1) {
                    f0 = lerpColor(f0, d.trail, 0.22f);
                    f1 = lerpColor(f1, d.trail, 0.42f);
                    b0 = lerpColor(b0, d.trail, 0.22f);
                    b1 = lerpColor(b1, d.trail, 0.42f);
                }
                sink.blade(pts[s], pts[s + 1], w0, w1, Vec3{0.0f, 0.0f, 1.0f},
                           f0Shadow, f1Shadow, f0, f1, u, d.glowStrength * 0.30f);
            }
        }
    }
}

}  // namespace detail

// Builds one wing (side < 0 = right, side > 0 = left) into `out`.
// Returns the number of quads written (never more than the cap).
inline int buildWing(const WingDesign& d, const WingPose& pose, int side,
                     WingQuad* out, int cap) {
    if (!out || cap <= 0) return 0;

    Vec3 joint[4];
    buildArmJoints(d, pose, joint);

    detail::Sink sink{out, cap};
    detail::addFairing(sink, joint, d, pose);
    detail::addMembrane(sink, joint, d, pose);
    detail::addRibs(sink, joint, d, pose);
    if (d.feathers) detail::addFeathers(sink, joint, d, pose);
    detail::addSpar(sink, joint, d, pose);

    if (side > 0) {
        for (int i = 0; i < sink.count; ++i) {
            for (int k = 0; k < 4; ++k) out[i].p[k].x = -out[i].p[k].x;
            out[i].n.x = -out[i].n.x;
        }
    }
    return sink.count;
}

}  // namespace bedrocktools::modules::wings

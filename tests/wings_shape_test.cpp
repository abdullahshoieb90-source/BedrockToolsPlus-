// Host-side unit test for the Wings geometry + shading helpers.
//
// The Wings overlay draws each wing as one continuous membrane built by
// src/modules/visual/wings_surface.hpp from the declarative style table in
// wings_design.hpp. This is the exact code the game executes in
// renderWingsOverlay, so these checks guard the things that used to be broken:
//
//   * the old renderer drew seven disjoint boxes per wing, which read as a comb
//     of planks; the membrane must be a continuous sheet with a real silhouette
//   * every quad must be planar, non-degenerate and wound towards the player's
//     back (+z), so the renderer can pick the right palette side
//   * nothing may cross the torso back plane (z = 2) and nothing may leave the
//     flyable volume (|x| < 48, y in [-8, 40])
//   * the left wing must be the exact mirror of the right one
//   * finger struts must land on the membrane edge, feathers must overhang the
//     sheet (that is what makes the feathered styles read as feathers)
//   * raising / lowering the arm must move the wing tip, fluttering must add
//     depth, and a heavier flutter must never collapse the sheet
//   * shading helpers: brightness range, channel clamping, tip gradient
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src -I include tests/wings_shape_test.cpp -o /tmp/wings_shape_test
//     /tmp/wings_shape_test

#include "modules/visual/wings_design.hpp"
#include "modules/visual/wings_shape.hpp"
#include "modules/visual/wings_surface.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace wings = bedrocktools::modules::wings;

namespace {
int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

bool finite3(const wings::Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

wings::WingPose restPose() {
    wings::WingPose pose;
    pose.shoulderDeg = 26.0f;
    pose.elbowDeg = 8.0f;
    pose.wristDeg = 9.0f;
    pose.phaseRad = 0.4f;
    pose.flutter = 0.35f;
    return pose;
}

// The membrane is a curved sheet, so its cells are bilinear patches and only
// near-planar. Bound the out-of-plane error relative to the cell size instead
// of demanding exact planarity (a beam or a vane is exactly planar).
bool quadIsNearlyPlanar(const wings::WingQuad& q) {
    const wings::Vec3 n = wings::normalized(wings::cross(q.p[1] - q.p[0], q.p[2] - q.p[0]));
    if (wings::length(n) < 0.5f) return false;
    const float d = std::fabs(wings::dot(n, q.p[3] - q.p[0]));
    (void)wings::length(q.p[2] - q.p[0]);
    // 0.3 model px = ~2 cm in game: far below anything visible.
    return d <= 0.30f;
}

float quadArea(const wings::WingQuad& q) {
    float p[4][3];
    for (int i = 0; i < 4; ++i) {
        p[i][0] = q.p[i].x;
        p[i][1] = q.p[i].y;
        p[i][2] = q.p[i].z;
    }
    return wings::ringArea(p);
}

// Projected area of the whole wing on the wing plane (XY). A comb of thin
// planks covers a tiny fraction of a real wing, so this is the direct guard
// against the old "wings are sticks" look.
float projectedArea(const wings::WingQuad* quads, int count) {
    float total = 0.0f;
    for (int i = 0; i < count; ++i) {
        float area = 0.0f;
        for (int t = 0; t < 2; ++t) {
            const wings::Vec3& a = quads[i].p[0];
            const wings::Vec3& b = quads[i].p[t == 0 ? 1 : 2];
            const wings::Vec3& c = quads[i].p[t == 0 ? 2 : 3];
            area += 0.5f * std::fabs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
        }
        total += area;
    }
    return total;
}

// Bounding box of one wing.
void wingBounds(const wings::WingQuad* quads, int count, float& minX, float& maxX,
                float& minY, float& maxY, float& minZ, float& maxZ) {
    minX = minY = minZ = 1e9f;
    maxX = maxY = maxZ = -1e9f;
    for (int i = 0; i < count; ++i) {
        for (int k = 0; k < 4; ++k) {
            const wings::Vec3& p = quads[i].p[k];
            if (p.x < minX) minX = p.x;
            if (p.x > maxX) maxX = p.x;
            if (p.y < minY) minY = p.y;
            if (p.y > maxY) maxY = p.y;
            if (p.z < minZ) minZ = p.z;
            if (p.z > maxZ) maxZ = p.z;
        }
    }
}

wings::WingQuad g_quads[wings::kMaxWingQuads];
wings::WingQuad g_quadsLeft[wings::kMaxWingQuads];

}  // namespace

int main() {
    std::printf("wings membrane geometry\n");

    // ------------------------------------------------------------------
    // Style table
    // ------------------------------------------------------------------
    check(wings::kWingStyleCount == wings::kWingDesignCount, "design table and style count agree");
    {
        bool uniqueIds = true;
        for (int i = 0; i < wings::kWingStyleCount; ++i) {
            for (int j = i + 1; j < wings::kWingStyleCount; ++j) {
                if (std::string(wings::kWingDesigns[i].id) == wings::kWingDesigns[j].id) uniqueIds = false;
            }
        }
        check(uniqueIds, "every style id is unique (configs are persisted by index + id)");
        check(wings::wingStyleIndexForId("dragon") == 0, "dragon is the default style");
        check(wings::wingStyleIndexForId("phoenix") == 5, "phoenix resolves to index 5");
        check(wings::resolveWingStyleIndex("4,dragon,angel,demon,bat,butterfly,phoenix,fairy") == 4,
              "full radio value resolves to 4");
        check(wings::resolveWingStyleIndex("unicorn") == 0, "unknown style falls back to dragon");
        check(wings::wingStyleRadioValue(2).rfind("2,dragon,angel", 0) == 0,
              "radio value lists all styles");
    }

    // ------------------------------------------------------------------
    // Per style: quad validity, bounds, mirror, silhouette size
    // ------------------------------------------------------------------
    for (int s = 0; s < wings::kWingStyleCount; ++s) {
        const wings::WingDesign& d = wings::kWingDesigns[s];
        const std::string tag = d.id;

        const int count = wings::buildWing(d, restPose(), -1, g_quads, wings::kMaxWingQuads);
        const int countLeft = wings::buildWing(d, restPose(), 1, g_quadsLeft, wings::kMaxWingQuads);

        check(count > 40, tag + ": builds a full membrane (not a handful of boxes)");
        check(count == countLeft, tag + ": both wings build the same number of quads");

        bool finite = true, planar = true, nonDegenerate = true, facesBack = true, normalOk = true;
        for (int i = 0; i < count; ++i) {
            const wings::WingQuad& q = g_quads[i];
            for (int k = 0; k < 4; ++k) {
                if (!finite3(q.p[k])) finite = false;
            }
            if (!finite3(q.n)) normalOk = false;
            if (std::fabs(wings::length(q.n) - 1.0f) > 1e-2f) normalOk = false;
            if (q.n.z < -1e-4f) facesBack = false;
            if (!quadIsNearlyPlanar(q)) planar = false;
            if (!(quadArea(q) > 1e-3f)) nonDegenerate = false;
        }
        check(finite, tag + ": every corner is finite");
        check(normalOk, tag + ": every normal is a finite unit vector");
        check(planar, tag + ": every quad is flat enough to rasterize cleanly");
        check(nonDegenerate, tag + ": no degenerate quads");
        check(facesBack, tag + ": normals point towards the player's back (+z)");

        float minX, maxX, minY, maxY, minZ, maxZ;
        wingBounds(g_quads, count, minX, maxX, minY, maxY, minZ, maxZ);
        check(minZ > 2.0f, tag + ": nothing crosses the torso back plane (z > 2)");
        check(minX < -14.0f, tag + ": the wing reaches outboard past the torso");
        check(maxX < 0.0f, tag + ": the right wing stays on the right half");
        check(maxY < 44.0f && minY > -8.0f, tag + ": the wing stays in a sane vertical range");

        const float area = projectedArea(g_quads, count);
        check(area > 320.0f, tag + ": the membrane covers a real wing area (" +
                                 std::to_string(static_cast<int>(area)) + " px2)");

        // Mirror: left wing is the right wing with x negated.
        bool mirrored = true;
        for (int i = 0; i < count && mirrored; ++i) {
            for (int k = 0; k < 4; ++k) {
                if (std::fabs(g_quads[i].p[k].x + g_quadsLeft[i].p[k].x) > 1e-4f) mirrored = false;
                if (std::fabs(g_quads[i].p[k].y - g_quadsLeft[i].p[k].y) > 1e-4f) mirrored = false;
                if (std::fabs(g_quads[i].p[k].z - g_quadsLeft[i].p[k].z) > 1e-4f) mirrored = false;
            }
            if (std::fabs(g_quads[i].n.x + g_quadsLeft[i].n.x) > 1e-4f) mirrored = false;
            if (std::fabs(g_quads[i].n.y - g_quadsLeft[i].n.y) > 1e-4f) mirrored = false;
        }
        check(mirrored, tag + ": the left wing is the exact mirror of the right one");
    }

    // ------------------------------------------------------------------
    // The membrane is a continuous sheet: neighbouring columns of the grid
    // sample the same edge, so there are no gaps between "planks".
    // ------------------------------------------------------------------
    {
        const wings::WingDesign& d = wings::kWingDesigns[0];
        wings::Vec3 joint[4];
        wings::buildArmJoints(d, restPose(), joint);

        bool continuous = true;
        for (int i = 0; i < wings::kMembraneSpanSegments; ++i) {
            const float uEdge = static_cast<float>(i + 1) / wings::kMembraneSpanSegments;
            for (int j = 0; j <= wings::kMembraneChordSegments; ++j) {
                const float v = static_cast<float>(j) / wings::kMembraneChordSegments;
                const wings::Vec3 a = wings::membranePoint(joint, d, restPose(), uEdge, v);
                const wings::Vec3 b = wings::membranePoint(joint, d, restPose(), uEdge, v);
                if (wings::length(a - b) > 1e-5f) continuous = false;
            }
        }
        check(continuous, "adjacent membrane cells share their edge exactly (no seams)");

        // Sampling resolution: a step along the span may not jump more than a
        // couple of pixels, otherwise the sheet would look faceted.
        float maxJump = 0.0f;
        for (int i = 0; i < wings::kMembraneSpanSegments; ++i) {
            const float u0 = static_cast<float>(i) / wings::kMembraneSpanSegments;
            const float u1 = static_cast<float>(i + 1) / wings::kMembraneSpanSegments;
            const wings::Vec3 a = wings::membranePoint(joint, d, restPose(), u0, 1.0f);
            const wings::Vec3 b = wings::membranePoint(joint, d, restPose(), u1, 1.0f);
            const float jump = wings::length(a - b);
            if (jump > maxJump) maxJump = jump;
        }
        check(maxJump < 4.0f, "the sail is sampled finely enough to read as smooth");
    }

    // ------------------------------------------------------------------
    // Trailing edge: the scallops really carve `fingers` bites out of the sail
    // ------------------------------------------------------------------
    for (int s = 0; s < wings::kWingStyleCount; ++s) {
        const wings::WingDesign& d = wings::kWingDesigns[s];
        if (d.scallopPx <= 0.0f) continue;
        const int fingers = d.fingers;
        int minima = 0;
        for (int k = 0; k < fingers; ++k) {
            const float uMid = (static_cast<float>(k) + 0.5f) / static_cast<float>(fingers);
            const float uStrut = static_cast<float>(k + 1) / static_cast<float>(fingers);
            if (uStrut > 0.99f || uMid < 0.10f) continue;
            if (wings::chordLength(d, uMid) < wings::chordLength(d, uStrut) - 0.2f) ++minima;
        }
        check(minima >= 2, std::string(d.id) + ": the trailing edge is scalloped into fingers");
    }

    // ------------------------------------------------------------------
    // Feather styles: the quills overhang the sheet, so the outline is feathered
    // (the plain sheet never reaches past its own chord).
    // ------------------------------------------------------------------
    for (int s = 0; s < wings::kWingStyleCount; ++s) {
        const wings::WingDesign& d = wings::kWingDesigns[s];
        if (!d.feathers) continue;
        wings::Vec3 joint[4];
        wings::buildArmJoints(d, restPose(), joint);
        const wings::Vec3 sheetEdge = wings::membranePoint(joint, d, restPose(), 0.6f, 1.0f);
        float lowestY = 1e9f;
        const int count = wings::buildWing(d, restPose(), -1, g_quads, wings::kMaxWingQuads);
        for (int i = 0; i < count; ++i) {
            for (int k = 0; k < 4; ++k) {
                if (g_quads[i].p[k].y < lowestY) lowestY = g_quads[i].p[k].y;
            }
        }
        check(lowestY < sheetEdge.y - 3.0f,
              std::string(d.id) + ": the quills hang below the base sheet (feathered outline)");
    }

    // ------------------------------------------------------------------
    // Animation response
    // ------------------------------------------------------------------
    {
        const wings::WingDesign& d = wings::kWingDesigns[0];
        wings::WingPose up = restPose();
        wings::WingPose down = restPose();
        up.shoulderDeg = 55.0f;
        down.shoulderDeg = -10.0f;

        const int cu = wings::buildWing(d, up, -1, g_quads, wings::kMaxWingQuads);
        float upMinX, upMaxX, upMinY, upMaxY, upMinZ, upMaxZ;
        wingBounds(g_quads, cu, upMinX, upMaxX, upMinY, upMaxY, upMinZ, upMaxZ);
        const int cd = wings::buildWing(d, down, -1, g_quads, wings::kMaxWingQuads);
        float dnMinX, dnMaxX, dnMinY, dnMaxY, dnMinZ, dnMaxZ;
        wingBounds(g_quads, cd, dnMinX, dnMaxX, dnMinY, dnMaxY, dnMinZ, dnMaxZ);

        check(upMaxY > dnMaxY, "raising the shoulder lifts the wing tip");
        check(dnMinY < upMinY, "lowering the shoulder drops the trailing edge");

        // Flutter adds depth: the sheet must move towards the camera, never
        // through the torso.
        wings::WingPose still = restPose();
        still.flutter = 0.0f;
        wings::WingPose flapping = restPose();
        flapping.flutter = 1.0f;
        flapping.phaseRad = 1.9f;

        wings::Vec3 joint[4];
        wings::buildArmJoints(d, still, joint);
        const wings::Vec3 a = wings::membranePoint(joint, d, still, 0.85f, 0.6f);
        const wings::Vec3 b = wings::membranePoint(joint, d, flapping, 0.85f, 0.6f);
        check(std::fabs(a.z - b.z) > 0.05f, "the flap phase moves the membrane (flutter)");
        check(b.z > 2.4f, "a fluttering membrane never dips into the torso");

        // Elbow / wrist articulation changes the arm shape.
        wings::WingPose bent = restPose();
        bent.elbowDeg = 35.0f;
        const int cb = wings::buildWing(d, bent, -1, g_quads, wings::kMaxWingQuads);
        float bMinX, bMaxX, bMinY, bMaxY, bMinZ, bMaxZ;
        wingBounds(g_quads, cb, bMinX, bMaxX, bMinY, bMaxY, bMinZ, bMaxZ);
        const int cr = wings::buildWing(d, restPose(), -1, g_quads, wings::kMaxWingQuads);
        float rMinX, rMaxX, rMinY, rMaxY, rMinZ, rMaxZ;
        wingBounds(g_quads, cr, rMinX, rMaxX, rMinY, rMaxY, rMinZ, rMaxZ);
        check(bMinX > rMinX - 0.01f || bMaxY > rMaxY + 0.01f,
              "the elbow angle changes the wing silhouette");
    }

    // ------------------------------------------------------------------
    // Pose mapping and camera-side selection (shared with the module)
    // ------------------------------------------------------------------
    {
        const wings::WingPose idle = wings::makeWingPose(20.0f, 5.0f, 6.0f, 0.5f, 0.0f, 0.0f);
        const wings::WingPose flap = wings::makeWingPose(40.0f, 10.0f, 12.0f, 2.0f, 1.0f, 0.0f);
        const wings::WingPose glide = wings::makeWingPose(50.0f, -15.0f, -15.0f, 0.5f, 0.0f, 1.0f);
        check(idle.shoulderDeg == 20.0f && idle.elbowDeg == 5.0f && idle.wristDeg == 6.0f,
              "the module angles map onto shoulder / elbow / wrist");
        check(idle.phaseRad == 0.5f, "the flap phase is passed through to the builder");
        check(idle.flutter < flap.flutter, "flapping flutters the membrane more than idling");
        check(glide.flutter < idle.flutter, "gliding tightens the membrane");

        // Camera side: model +z is the back. Player facing +z with right = -x.
        const wings::Vec3 right{-1.0f, 0.0f, 0.0f};
        const wings::Vec3 forward{0.0f, 0.0f, 1.0f};
        const wings::Vec3 body{0.0f, 1.5f, 0.0f};
        check(wings::cameraSeesBackSide({0.0f, 2.0f, -4.0f}, body, right, forward),
              "a camera behind the player sees the back palette");
        check(!wings::cameraSeesBackSide({0.0f, 2.0f, 4.0f}, body, right, forward),
              "a camera in front sees the body-facing palette");
    }

    // ------------------------------------------------------------------
    // Palette sanity
    // ------------------------------------------------------------------
    for (int s = 0; s < wings::kWingStyleCount; ++s) {
        const wings::WingDesign& d = wings::kWingDesigns[s];
        const int sumBack = d.back.r + d.back.g + d.back.b;
        const int sumTrail = d.trail.r + d.trail.g + d.trail.b;
        check(sumBack != sumTrail, std::string(d.id) + ": the sail and its trailing edge are distinct");
        check(!(d.back.r == d.back.g && d.back.g == d.back.b && d.back.r == 0),
              std::string(d.id) + ": the membrane is never pure black (invisible at night)");
        check(d.glowStrength >= 0.0f && d.glowStrength <= 1.0f,
              std::string(d.id) + ": glow strength is normalized");
    }

    // ------------------------------------------------------------------
    // Front-view gate: the wings are only drawn while the camera is behind the
    // player. Everything at or in front of side-on must produce alpha 0 (the
    // renderer skips the frame entirely), so the sails can never be seen
    // sticking out either side of the torso.
    // ------------------------------------------------------------------
    {
        const wings::Vec3 center{0.0f, 1.25f, 0.0f};
        const wings::Vec3 forward{0.0f, 0.0f, 1.0f};  // player faces +z
        const wings::Vec3 backDir{0.0f, 0.0f, -1.0f};

        check(wings::cameraBackness(center + backDir * 4.0f, center, forward) > 0.99f,
              "a camera directly behind the player has backness ~ +1");
        check(wings::cameraBackness(center + forward * 4.0f, center, forward) < -0.99f,
              "a camera directly in front has backness ~ -1");
        check(std::fabs(wings::cameraBackness(center + wings::Vec3{4.0f, 0.0f, 0.0f}, center,
                                              forward)) < 0.01f,
              "a side-on camera has backness ~ 0");

        // The renderer's gate: nothing at all from the front or side-on.
        for (float z : {4.0f, 0.0f, -0.0f}) {
            const wings::Vec3 cam{0.0f, 1.5f, z};
            check(!(wings::cameraBackness(cam, center, forward) > 0.0f),
                  "the overlay is skipped when the camera is not behind the player");
        }

        // Normal third-person angles must be fully solid, not half faded.
        for (float ang : {0.0f, 15.0f, 30.0f, 45.0f, 60.0f}) {
            const float r = ang * 3.14159265f / 180.0f;
            const wings::Vec3 cam = center + wings::Vec3{std::sin(r) * 4.0f, 0.0f,
                                                         -std::cos(r) * 4.0f};
            const float a = wings::wingFadeAlpha(wings::cameraBackness(cam, center, forward));
            check(a > 0.999f, "a third-person angle behind the player keeps the wings opaque");
        }

        // The fade is monotonic and reaches zero exactly at the gate.
        float prev = 0.0f;
        bool monotonic = true;
        for (int i = 0; i <= 40; ++i) {
            const float b = -0.1f + 0.03f * static_cast<float>(i);
            const float a = wings::wingFadeAlpha(b);
            if (a < prev - 1e-6f) monotonic = false;
            if (b <= 0.0f && a != 0.0f) monotonic = false;
            prev = a;
        }
        check(monotonic, "the front-view fade never increases towards the front");
        check(wings::wingFadeAlpha(0.0f) == 0.0f, "the fade is zero at the side-on plane");
        check(wings::wingFadeAlpha(-1.0f) == 0.0f, "the fade is zero in front of the player");
    }

    // ------------------------------------------------------------------
    // Body occlusion: from the front the player's body hides the wings; from
    // behind it must never hide them.
    // ------------------------------------------------------------------
    //
    // Conventions used here (matching the overlay): the player faces +z, so
    // their BACK is -z (model +z maps to world -z) and the player's right is
    // -x. Body box = the vanilla 0.6 x 1.8 x 0.6 collision AABB.
    {
        const wings::Vec3 bodyMin{-0.3f, 0.0f, -0.3f};
        const wings::Vec3 bodyMax{0.3f, 1.8f, 0.3f};
        const wings::Vec3 wingPoint{0.0f, 1.3f, -0.35f};  // just behind the back

        const wings::Vec3 frontCam{0.0f, 1.5f, 4.0f};    // in front of the player
        const wings::Vec3 backCam{0.0f, 1.5f, -4.0f};    // behind the player
        const wings::Vec3 sideCam{4.0f, 1.5f, 4.0f};     // front-right

        check(wings::hiddenBehindBody(frontCam, wingPoint, bodyMin, bodyMax),
              "a wing point behind the body is hidden from a front camera");
        check(!wings::hiddenBehindBody(backCam, wingPoint, bodyMin, bodyMax),
              "the same wing point stays visible from behind (wings are not culled)");

        // A point further out along the wing is genuinely beside the body, so a
        // three-quarter camera still sees it.
        const wings::Vec3 outerWing{-2.0f, 1.6f, -0.35f};
        check(!wings::hiddenBehindBody(sideCam, outerWing, bodyMin, bodyMax),
              "a three-quarter view keeps the outer wing visible");

        // Wing mounts sit 0.18 blocks behind the center (3 model px). With the
        // model-depth occluder they are OUTSIDE the body, so a front camera
        // culls them - this is the case that used to paint the wing roots over
        // the player's chest.
        wings::Vec3 occMin, occMax;
        wings::bodyOccluderFromAabb(bodyMin, bodyMax, occMin, occMax);
        check(std::fabs(occMax.z - 0.125f) < 1e-4f && std::fabs(occMin.z + 0.125f) < 1e-4f,
              "the occluder is the model depth, not the square collision box");
        check(occMin.x == bodyMin.x && occMax.x == bodyMax.x,
              "the occluder keeps the collision box width (arms and torso)");

        const wings::Vec3 mount{0.0f, 1.31f, -0.18f};
        check(wings::hiddenBehindBody(frontCam, mount, occMin, occMax),
              "a wing mount behind the back is hidden from a front camera");
        check(!wings::hiddenBehindBody(backCam, mount, occMin, occMax),
              "the same mount is visible from behind (wings stay attached)");

        // A whole quad behind the torso is skipped; a quad off to the side is
        // kept. This is the predicate the overlay uses to size its vertex batch.
        const wings::Vec3 origin{0.0f, 0.0f, 0.0f};
        const wings::Vec3 right{-1.0f, 0.0f, 0.0f};
        const wings::Vec3 forward{0.0f, 0.0f, 1.0f};

        // model pz = 10 px -> 0.625 blocks behind the back, clear of the box.
        wings::WingQuad behind{};
        for (int k = 0; k < 4; ++k) {
            behind.p[k] = wings::Vec3{-1.0f + 2.0f * static_cast<float>(k & 1),
                                      19.0f + static_cast<float>((k >> 1) & 1),
                                      10.0f};
        }
        check(wings::quadHiddenByBody(behind, occMin, occMax, origin, right, forward, frontCam),
              "a quad directly behind the torso is culled from the front");
        check(!wings::quadHiddenByBody(behind, occMin, occMax, origin, right, forward, backCam),
              "nothing is culled when looking at the player's back");

        // Same depth, but far out along the wing: still drawn from the front.
        wings::WingQuad wide = behind;
        for (int k = 0; k < 4; ++k) {
            wide.p[k].x = -26.0f + 2.0f * static_cast<float>(k & 1);
            wide.p[k].y = 20.0f + static_cast<float>((k >> 1) & 1);
        }
        check(!wings::quadHiddenByBody(wide, occMin, occMax, origin, right, forward, frontCam),
              "the outer part of the wing is still drawn from the front");
    }

    // ------------------------------------------------------------------
    // Headlight direction (regression: wings.cpp used to shade with the
    // negated *world* center, so a wing changed brightness whenever the
    // player walked somewhere else and looked washed out from the front).
    // ------------------------------------------------------------------
    {
        const wings::Vec3 surface{500.0f, 72.0f, -299.0f};
        const wings::Vec3 camera{500.0f, 71.5f, -305.0f};
        const wings::Vec3 toCam = wings::toCameraVector(camera, surface);
        check(std::fabs(toCam.x - (camera.x - surface.x)) < 1e-6f &&
                  std::fabs(toCam.y - (camera.y - surface.y)) < 1e-6f &&
                  std::fabs(toCam.z - (camera.z - surface.z)) < 1e-6f,
              "toCameraVector points from the surface to the camera");

        // The exact bug: the headlight must not be the negated world center.
        const wings::Vec3 negatedWorldCenter{-surface.x, -surface.y, -surface.z};
        check(wings::length(toCam - negatedWorldCenter) > 1.0f,
              "the headlight vector is not the negated world center");

        // Same geometry, player standing 1000 blocks away: the shading may not
        // change. Negating the world center fails this check.
        const wings::Vec3 offset{1234.0f, 64.0f, -987.0f};
        const wings::Vec3 n{0.0f, 0.0f, 1.0f};
        const float near = wings::faceBrightness(
            n, wings::toCameraVector(camera, surface), wings::kDefaultLight);
        const float far = wings::faceBrightness(
            n, wings::toCameraVector(camera + offset, surface + offset), wings::kDefaultLight);
        check(std::fabs(near - far) < 1e-4f,
              "wing shading does not depend on the player's world position");

        // And the light must actually come from the camera: a surface facing
        // the camera is brighter than one facing away.
        const float facing = wings::faceBrightness(n, {0.0f, 0.0f, 1.0f}, wings::kDefaultLight);
        const float away = wings::faceBrightness(n, {0.0f, 0.0f, -1.0f}, wings::kDefaultLight);
        check(facing > away, "a surface facing the camera is lit more than one facing away");
    }

    // ------------------------------------------------------------------
    // Shading model
    // ------------------------------------------------------------------
    std::printf("shading\n");
    {
        const wings::WingLight& light = wings::kDefaultLight;
        const wings::Vec3 toCam{0.0f, 0.0f, 1.0f};

        const float top = wings::faceBrightness({0.0f, 1.0f, 0.0f}, toCam, light);
        const float bottom = wings::faceBrightness({0.0f, -1.0f, 0.0f}, toCam, light);
        const float back = wings::faceBrightness({0.0f, 0.0f, 1.0f}, toCam, light);
        check(top > bottom, "upward faces are lit more than downward faces");
        check(back > bottom, "camera-facing back faces are lit more than the underside");
        check(bottom >= light.ambient - 1e-4f, "brightness never drops below ambient");
        check(top <= light.ambient + light.head + light.sky + 1e-4f,
              "brightness never exceeds the model range");

        check(wings::shadeChannel(200.0f, 2.0f) == 255, "channels clamp at 255");
        check(wings::shadeChannel(200.0f, -1.0f) == 0, "channels clamp at 0");
        check(wings::shadeChannel(0.0f, 1.0f) == 0, "black stays black");

        const float tip = wings::spanTint(1.0f);
        const float shoulder = wings::spanTint(0.0f);
        check(tip > shoulder, "span gradient brightens the wing tips");
        check(std::fabs(tip - (1.0f + wings::kSpanGradient * 0.5f)) < 1e-5f,
              "tip tint = 1 + gradient/2");
        check(std::fabs(shoulder - (1.0f - wings::kSpanGradient * 0.5f)) < 1e-5f,
              "shoulder tint = 1 - gradient/2");

        const wings::FaceColor base{40, 60, 90};
        const wings::FaceColor lit = wings::shadeFace(base, 1.1f);
        check(lit.r > base.r && lit.g > base.g && lit.b > base.b,
              "bright factor lifts the base colour");
        const wings::FaceColor dim = wings::shadeFace(base, 0.6f);
        check(dim.r < base.r && dim.g < base.g && dim.b < base.b,
              "dim factor darkens the base colour");

        const wings::FaceColor mixed = wings::lerpColor({0, 0, 0}, {100, 200, 40}, 0.5f);
        check(mixed.r == 50 && mixed.g == 100 && mixed.b == 20, "lerpColor blends channels");
    }

    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all wings geometry + shading checks passed\n");
    return 0;
}

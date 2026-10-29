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
#include "modules/visual/wings_trail.hpp"

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
    // Rail styles: the leading edge is a bar of constant width, which is what
    // makes a flat-topped sheet read as a straight rail instead of a taper.
    // ------------------------------------------------------------------
    for (int s = 0; s < wings::kWingStyleCount; ++s) {
        const wings::WingDesign& d = wings::kWingDesigns[s];
        if (d.railPx <= 0.0f) continue;
        wings::Vec3 joint[4];
        // The design's own rest angles (no animation), because that is the pose
        // the shape is authored in; flapping tilts the rail on purpose.
        wings::WingPose rest;
        wings::buildArmJoints(d, rest, joint);
        // A level arm: shoulder, elbow and tip all at the same height, so the
        // rail cannot sag below the shoulders.
        float minY = 1e9f, maxY = -1e9f;
        for (int i = 0; i < 4; ++i) {
            minY = std::fmin(minY, joint[i].y);
            maxY = std::fmax(maxY, joint[i].y);
        }
        check(maxY - minY < 0.5f, std::string(d.id) + ": the rail runs dead level");
        // And the sheet keeps its chord outboard instead of tapering to a point.
        check(wings::chordLength(d, 0.9f) > 0.75f * wings::chordLength(d, 0.1f),
              std::string(d.id) + ": the sheet stays broad out to the tip");
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
    // Body shadow cone: the exact hidden volume of the body as seen from the
    // camera. It must agree with the exact segment test everywhere, otherwise
    // the wing gets eaten outside the player's silhouette (or left painted over
    // the torso).
    // ------------------------------------------------------------------
    {
        const wings::Vec3 boxMin{-0.3f, 0.0f, -0.125f};
        const wings::Vec3 boxMax{0.3f, 1.8f, 0.125f};

        struct CameraCase {
            wings::Vec3 pos;
            const char* label;
        };
        const CameraCase cameras[] = {
            {{0.0f, 1.5f, 4.0f}, "face-on"},
            {{2.6f, 2.4f, 3.2f}, "corner"},
            {{0.0f, 0.4f, 3.5f}, "low"},
            {{0.0f, 3.4f, 3.0f}, "high"},
        };

        for (const CameraCase& cc : cameras) {
            const wings::ShadowCone cone = wings::buildShadowCone(cc.pos, boxMin, boxMax);
            check(cone.count >= 4, "the body's shadow cone has supporting planes");

            int mismatches = 0;
            int hiddenCount = 0;
            // Points on the wing side of the player only (behind the back face).
            for (int ix = -6; ix <= 6; ++ix) {
                for (int iy = 0; iy <= 8; ++iy) {
                    for (int iz = 0; iz <= 5; ++iz) {
                        const wings::Vec3 p{0.5f * static_cast<float>(ix),
                                            1.0f + 0.18f * static_cast<float>(iy),
                                            -0.20f - 0.16f * static_cast<float>(iz)};
                        bool inCone = true;
                        for (int k = 0; k < cone.count; ++k) {
                            if (wings::dot(cone.plane[k], p - cc.pos) > 0.0f) {
                                inCone = false;
                                break;
                            }
                        }
                        const bool exact =
                            wings::hiddenBehindBody(cc.pos, p, boxMin, boxMax);
                        if (inCone != exact) ++mismatches;
                        if (exact) ++hiddenCount;
                    }
                }
            }
            check(mismatches == 0, "the shadow cone agrees with the exact test at every sample");
            check(hiddenCount > 0, "the sample grid actually crosses the hidden region");
        }
    }

    // ------------------------------------------------------------------
    // Clipping: the part of a wing quad behind the body is cut away, the rest
    // survives with its positions and colours intact.
    // ------------------------------------------------------------------
    {
        const wings::Vec3 boxMin{-0.3f, 0.0f, -0.125f};
        const wings::Vec3 boxMax{0.3f, 1.8f, 0.125f};
        const wings::Vec3 cam{0.0f, 1.5f, 4.0f};
        const wings::ShadowCone cone = wings::buildShadowCone(cam, boxMin, boxMax);

        // Area of a piece when projected onto the plane z = const.
        auto area = [](const wings::ClipPolygon& p) {
            float a = 0.0f;
            for (int k = 0; k < p.count; ++k) {
                const wings::Vec3 u = p.v[k].p;
                const wings::Vec3 v = p.v[(k + 1) % p.count].p;
                a += u.x * v.y - v.x * u.y;
            }
            return std::fabs(a) * 0.5f;
        };

        auto makePoly = [](wings::ClipVertex* v, float x0, float x1, float y0, float y1,
                           float z) {
            v[0].p = {x0, y0, z};
            v[1].p = {x1, y0, z};
            v[2].p = {x1, y1, z};
            v[3].p = {x0, y1, z};
            for (int k = 0; k < 4; ++k) {
                v[k].c = wings::FaceColor{static_cast<unsigned char>(40 + 20 * k),
                                          static_cast<unsigned char>(80 + 10 * k),
                                          static_cast<unsigned char>(120 + 5 * k)};
            }
        };

        wings::ClipVertex in[4];
        wings::ClipPolygon pieces[wings::kMaxShadowPlanes];

        // Entirely behind the torso -> nothing survives.
        makePoly(in, -0.05f, 0.05f, 1.20f, 1.30f, -0.30f);
        check(wings::clipPolygonOutsideCone(in, 4, cone, cam, pieces,
                                            wings::kMaxShadowPlanes) == 0,
              "a quad fully hidden behind the body is clipped away entirely");

        // Entirely outside the silhouette -> comes back whole and untouched.
        makePoly(in, -2.0f, -1.9f, 1.20f, 1.30f, -0.30f);
        int n = wings::clipPolygonOutsideCone(in, 4, cone, cam, pieces,
                                              wings::kMaxShadowPlanes);
        check(n == 1 && pieces[0].count == 4, "a quad clear of the body is left intact");
        bool sameCorners = (n == 1 && pieces[0].count == 4);
        bool sameColours = sameCorners;
        for (int k = 0; k < 4 && sameCorners; ++k) {
            if (wings::length(pieces[0].v[k].p - in[k].p) > 1e-4f) sameCorners = false;
            if (pieces[0].v[k].c.r != in[k].c.r || pieces[0].v[k].c.g != in[k].c.g ||
                pieces[0].v[k].c.b != in[k].c.b) {
                sameColours = false;
            }
        }
        check(sameCorners, "an untouched quad keeps its exact corners");
        check(sameColours, "an untouched quad keeps its exact colours");

        // Straddling the silhouette -> cut down to the visible part.
        makePoly(in, -0.90f, 0.05f, 1.20f, 1.30f, -0.30f);
        n = wings::clipPolygonOutsideCone(in, 4, cone, cam, pieces, wings::kMaxShadowPlanes);
        check(n >= 1, "a quad straddling the body silhouette is cut, not dropped");

        float keptArea = 0.0f;
        int keptVerts = 0;
        for (int i = 0; i < n; ++i) {
            keptArea += area(pieces[i]);
            keptVerts += pieces[i].count;
        }
        const float fullArea = 0.95f * 0.10f;
        check(keptArea > 0.0f && keptArea < fullArea * 0.999f,
              "the cut removes part of the quad instead of keeping all of it");

        // The surviving geometry must be genuinely visible: tested against a
        // body shrunk slightly, so vertices sitting exactly on the silhouette
        // (which is where a correct cut puts them) are not counted as hidden.
        const wings::Vec3 shrink{0.02f, 0.0f, 0.02f};
        bool allVisible = keptVerts > 0;
        for (int i = 0; i < n; ++i) {
            for (int k = 0; k < pieces[i].count; ++k) {
                if (wings::hiddenBehindBody(cam, pieces[i].v[k].p,
                                            boxMin + shrink, boxMax - shrink)) {
                    allVisible = false;
                }
            }
        }
        check(allVisible, "no surviving vertex of a cut quad is still behind the body");

        // Cut vertices carry interpolated shading, not a fresh flat tone.
        bool coloursInRange = keptVerts > 0;
        for (int i = 0; i < n; ++i) {
            for (int k = 0; k < pieces[i].count; ++k) {
                const unsigned char r = pieces[i].v[k].c.r;
                if (r < 40 || r > 100) coloursInRange = false;
            }
        }
        check(coloursInRange, "clipped vertices interpolate the shading they cut through");
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

        // The cone the overlay actually clips against is exact for the wing
        // mounts: hidden from the front, untouched from behind.
        const wings::Vec3 origin{0.0f, 0.0f, 0.0f};
        const wings::Vec3 right{-1.0f, 0.0f, 0.0f};
        const wings::Vec3 forward{0.0f, 0.0f, 1.0f};
        const wings::ShadowCone frontCone = wings::buildShadowCone(frontCam, occMin, occMax);

        // A quad directly behind the back: hidden from the front camera.
        wings::WingQuad behind{};
        for (int k = 0; k < 4; ++k) {
            behind.p[k] = wings::Vec3{-1.0f + 2.0f * static_cast<float>(k & 1),
                                      19.0f + static_cast<float>((k >> 1) & 1),
                                      10.0f};
        }
        wings::ClipVertex corners[4];
        for (int k = 0; k < 4; ++k) {
            corners[k].p = wings::modelPointToWorld(behind.p[k].x, behind.p[k].y, behind.p[k].z,
                                                    origin, right, forward);
            corners[k].c = wings::FaceColor{90, 90, 90};
        }
        wings::ClipPolygon pieces[wings::kMaxShadowPlanes];
        check(wings::clipPolygonOutsideCone(corners, 4, frontCone, frontCam, pieces,
                                            wings::kMaxShadowPlanes) == 0,
              "a wing quad behind the torso is clipped away from the front");

        // From behind, the wing sits *between* the camera and the body, so the
        // shadow cone no longer describes occlusion. The overlay therefore only
        // clips when the camera is on the front side; that gate is what keeps
        // the wings intact from behind.
        const wings::Vec3 bodyCenter{(occMin.x + occMax.x) * 0.5f,
                                     (occMin.y + occMax.y) * 0.5f,
                                     (occMin.z + occMax.z) * 0.5f};
        check(wings::cameraBackness(backCam, bodyCenter, forward) > 0.0f,
              "the overlay only clips in front, where the shadow cone is exact");
        check(wings::cameraBackness(frontCam, bodyCenter, forward) < 0.0f,
              "a front camera is on the clipping side");
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
    // Secondary motion: the wings lag behind the player and shudder on a jump
    // ------------------------------------------------------------------
    std::printf("secondary motion\n");
    {
        const wings::Vec3 right{-1.0f, 0.0f, 0.0f};   // facing +z
        const wings::Vec3 forward{0.0f, 0.0f, 1.0f};
        const float dt = 0.05f;                       // one game tick

        // Runs `ticks` ticks at a constant world velocity and returns the trail.
        auto settle = [&](float vx, float vy, int ticks) {
            wings::WingTrail t;
            for (int i = 0; i < ticks; ++i) {
                const wings::Vec3 vp = wings::velocityToModelPx({vx, vy, 0.0f}, right, forward);
                wings::stepWingTrail(t, dt, vp, false);
                wings::wingTrailRememberSample(t, vy);
            }
            return t;
        };

        // At rest nothing moves: the transform is skipped entirely in the builder.
        {
            const wings::WingTrail t = settle(0.0f, 0.0f, 5);
            check(wings::length(t.offsetPx) < 1e-4f && t.rollDeg == 0.0f,
                  "a standing player leaves the wings completely still");
        }

        // Running trails the wings BEHIND the player, i.e. against the motion.
        // Sign convention, which is the easy thing to get wrong: modelPointToWorld
        // maps model +x to world -right, and with yaw 0 right is (-1,0,0), so
        // model +x is world +x. A player moving towards +x therefore needs a
        // NEGATIVE model x offset to trail behind. If this ever flips, the wings
        // lead the player instead of dragging.
        {
            const wings::WingTrail t = settle(4.3f, 0.0f, 40);
            check(t.offsetPx.x < -1.0f, "running pushes the wings back against the motion");
            check(wings::length(t.offsetPx) <= wings::kWingTrailMaxPx + 1e-3f,
                  "the trail never exceeds its cap");
            // Up/down must stay untouched while running on flat ground.
            check(std::fabs(t.offsetPx.y) < 1e-3f, "running level does not lift or drop the wings");
        }

        // Speed scales the trail, and the cap actually caps it.
        {
            const wings::WingTrail slow = settle(2.0f, 0.0f, 40);
            const wings::WingTrail fast = settle(8.0f, 0.0f, 40);
            check(wings::length(fast.offsetPx) > wings::length(slow.offsetPx),
                  "a faster player drags the wings further back");
            check(wings::length(fast.offsetPx) < wings::kWingTrailMaxPx + 1e-3f,
                  "a very fast player is still capped by the trail limit");
        }

        // The delay is a real lag, not a snap: one tick of movement moves only a
        // fraction of the way, and stopping swings the wings back afterwards.
        {
            wings::WingTrail t;
            const wings::Vec3 run = wings::velocityToModelPx({4.3f, 0.0f, 0.0f}, right, forward);
            wings::stepWingTrail(t, dt, run, false);
            const float afterOneTick = t.offsetPx.x;
            check(afterOneTick < 0.0f && afterOneTick > -2.0f,
                  "the trail builds up over time instead of snapping to full");
            for (int i = 0; i < 40; ++i) wings::stepWingTrail(t, dt, run, false);
            const float settled = t.offsetPx.x;
            check(settled < afterOneTick, "the trail keeps growing until it settles");
            for (int i = 0; i < 60; ++i) wings::stepWingTrail(t, dt, {0.0f, 0.0f, 0.0f}, false);
            check(wings::length(t.offsetPx) < 0.02f, "stopping brings the wings back to rest");
        }

        // Rising drops the wings, falling floats them - the classic follow-through.
        {
            const wings::WingTrail up = settle(0.0f, 8.4f, 4);
            const wings::WingTrail down = settle(0.0f, -8.4f, 20);
            check(up.offsetPx.y < -1.0f, "jumping up leaves the wings hanging behind");
            check(down.offsetPx.y > 1.0f, "falling floats the wings up over the back");
        }

        // A jump is detected from the velocity step, and only a jump is.
        {
            wings::WingTrail t;
            wings::wingTrailRememberSample(t, 0.0f);
            check(!wings::wingTrailDetectsJump(t, 0.0f), "standing still is not a jump");
            check(!wings::wingTrailDetectsJump(t, 1.0f), "a step up is not a jump");
            check(wings::wingTrailDetectsJump(t, 8.4f), "a vanilla jump impulse is a jump");
            // Unprimed state never reports a jump, however large the velocity.
            wings::WingTrail fresh;
            check(!wings::wingTrailDetectsJump(fresh, 20.0f),
                  "the first sample after spawning cannot be a jump");
        }

        // The shudder rings, decays, and is clamped.
        {
            wings::WingTrail t;
            wings::stepWingTrail(t, dt, {}, true);
            check(std::fabs(t.rollDeg) > 0.0f || std::fabs(t.rollRateDegPerSec) > 0.0f,
                  "a jump kicks the shudder");
            float peak = 0.0f;
            for (int i = 0; i < 30; ++i) {
                wings::stepWingTrail(t, dt, {}, false);
                peak = std::fmax(peak, std::fabs(t.rollDeg));
            }
            check(peak > 3.0f, "the shudder is big enough to see (over 3 degrees)");
            check(peak <= wings::kWingTrailMaxRollDeg + 1e-3f, "the shudder respects its clamp");

            // A tick-rate driver has to be able to draw the ring: at least eight
            // samples per cycle, or it aliases into random jitter on screen.
            const float period = 2.0f * 3.14159265f / wings::kWingTrailRollOmega;
            check(dt <= period / 8.0f, "the shudder is slow enough for a 20 Hz tick to render");

            for (int i = 0; i < 120; ++i) wings::stepWingTrail(t, dt, {}, false);
            check(std::fabs(t.rollDeg) < 0.2f, "the shudder decays back to still");
        }

        // Stacked kicks cannot spin the wings over.
        {
            wings::WingTrail t;
            for (int i = 0; i < 40; ++i) wings::stepWingTrail(t, dt, {}, true);
            check(std::fabs(t.rollDeg) <= wings::kWingTrailMaxRollDeg + 1e-3f,
                  "repeated kicks stay inside the clamp");
        }

        // Stability: a hitch must not make the springs explode.
        {
            wings::WingTrail t;
            const wings::Vec3 vp = wings::velocityToModelPx({4.3f, 8.4f, 0.0f}, right, forward);
            for (int i = 0; i < 50; ++i) wings::stepWingTrail(t, 0.25f, vp, false);
            check(std::isfinite(t.offsetPx.x) && std::isfinite(t.offsetPx.y) &&
                  std::isfinite(t.offsetPx.z) && std::isfinite(t.rollDeg),
                  "a 250 ms hitch leaves the springs finite");
            check(wings::length(t.offsetPx) <= wings::kWingTrailMaxPx + 1.0f,
                  "a 250 ms hitch does not fling the wings off the body");
            // A zero or negative dt must be a no-op, not a divide by zero.
            const wings::WingTrail before = t;
            wings::stepWingTrail(t, 0.0f, vp, false);
            check(t.offsetPx.x == before.offsetPx.x, "dt = 0 does not disturb the springs");
        }

        // The builder applies the trail as a rigid move: same shape, same area,
        // just shifted - that is what makes it safe to bolt onto any style.
        {
            const wings::WingDesign& d = wings::kWingDesigns[0];
            static wings::WingQuad plain[wings::kMaxWingQuads];
            static wings::WingQuad moved[wings::kMaxWingQuads];
            wings::WingPose base = restPose();
            const int plainCount = wings::buildWing(d, base, -1, plain, wings::kMaxWingQuads);
            wings::WingPose shifted = restPose();
            shifted.trailPx = wings::Vec3{2.5f, -3.0f, 1.5f};
            const int movedCount = wings::buildWing(d, shifted, -1, moved, wings::kMaxWingQuads);
            check(plainCount == movedCount, "the trail does not change the quad count");
            bool offsetOk = true;
            for (int i = 0; i < plainCount; ++i)
                for (int k = 0; k < 4; ++k) {
                    if (std::fabs(moved[i].p[k].x - (plain[i].p[k].x + 2.5f)) > 1e-3f) offsetOk = false;
                    if (std::fabs(moved[i].p[k].y - (plain[i].p[k].y - 3.0f)) > 1e-3f) offsetOk = false;
                    if (std::fabs(moved[i].p[k].z - (plain[i].p[k].z + 1.5f)) > 1e-3f) offsetOk = false;
                }
            check(offsetOk, "the trail translates every quad by the same amount");
            check(std::fabs(projectedArea(moved, movedCount) - projectedArea(plain, plainCount)) < 0.5f,
                  "the trail does not change the wing's area");

            // A roll must keep the mesh rigid too (rotation preserves area).
            wings::WingPose rolled = restPose();
            rolled.trailRollDeg = 8.0f;
            const int rollCount = wings::buildWing(d, rolled, -1, moved, wings::kMaxWingQuads);
            check(rollCount == plainCount, "the shudder does not change the quad count");
            check(std::fabs(projectedArea(moved, rollCount) - projectedArea(plain, plainCount)) < 0.5f,
                  "the shudder is a rigid roll (area is preserved)");
            // The spine is the pivot, so the root barely moves.
            float rootDrift = 0.0f;
            for (int k = 0; k < 4; ++k) {
                const float dx = moved[0].p[k].x - plain[0].p[k].x;
                const float dy = moved[0].p[k].y - plain[0].p[k].y;
                rootDrift = std::fmax(rootDrift, std::sqrt(dx * dx + dy * dy));
            }
            check(rootDrift < 6.0f, "the shudder pivots about the spine, not the wing tip");
        }
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

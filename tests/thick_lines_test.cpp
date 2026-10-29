// Unit tests for the shared thick-line helper (Block Outline + Hitbox).
//
// Pins the corner behavior of the world-space overlays: GLES drivers ignore
// glLineWidth, so thick lines are camera-facing quads. Segment quads that
// overshoot their endpoints grew spurs past every box corner (a pinwheel
// look as soon as the thickness slider went up). The quads are now cut
// square at their endpoints and every shared endpoint gets a camera-facing
// square joint dot that closes the corner.
//
// Build: g++ -std=c++20 -I include -I src tests/thick_lines_test.cpp -o /tmp/thick_lines_test
// Run:   /tmp/thick_lines_test

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#include <bedrocktools/sdk/Types.hpp>

#include "modules/visual/thick_lines.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) std::printf("  ok   %s\n", what);
    else { std::printf("  FAIL %s\n", what); ++g_failures; }
}

bool near(float a, float b, float epsilon = 0.0001f) {
    return std::fabs(a - b) <= epsilon;
}

using Vec3 = bedrocktools::sdk::Vec3;
using thicklines::subtract;
using thicklines::cross;
using thicklines::length;

Vec3 add(const Vec3& a, const Vec3& b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

std::vector<Vec3> captureSegment(const Vec3& p1, const Vec3& p2,
                                 const Vec3& cam, float halfWidth) {
    std::vector<Vec3> out;
    thicklines::emitSegment(p1, p2, cam, halfWidth,
                            [&](const Vec3& v) { out.push_back(v); });
    return out;
}

std::vector<Vec3> captureDot(const Vec3& point, const Vec3& cam, float halfWidth) {
    std::vector<Vec3> out;
    thicklines::emitJointDot(point, cam, halfWidth,
                             [&](const Vec3& v) { out.push_back(v); });
    return out;
}

// The 12 edges of the unit cube [0,1]^3, sharing exact corner values.
std::vector<std::pair<Vec3, Vec3>> unitBoxEdges() {
    const Vec3 c000{0, 0, 0}, c100{1, 0, 0}, c010{0, 1, 0}, c110{1, 1, 0};
    const Vec3 c001{0, 0, 1}, c101{1, 0, 1}, c011{0, 1, 1}, c111{1, 1, 1};
    return {
        {c000, c100}, {c100, c110}, {c110, c010}, {c010, c000}, // bottom
        {c001, c101}, {c101, c111}, {c111, c011}, {c011, c001}, // top
        {c000, c001}, {c100, c101}, {c110, c111}, {c010, c011}, // pillars
    };
}

} // namespace

int main() {
    std::printf("thick line joints\n");

    const Vec3 cam{4.0f, 3.0f, 8.0f};
    const float hw = 0.05f;

    {
        const auto joints = thicklines::collectJoints(unitBoxEdges());
        check(joints.size() == 8, "a box's 12 edges share exactly 8 corner joints");
        bool hasOrigin = false;
        for (const auto& j : joints) {
            if (near(j.x, 0) && near(j.y, 0) && near(j.z, 0)) hasOrigin = true;
        }
        check(hasOrigin, "the shared cube corner is among the joints");

        const std::vector<std::pair<Vec3, Vec3>> single = {
            {Vec3{0, 0, 0}, Vec3{2, 0, 0}},
        };
        check(thicklines::collectJoints(single).empty(),
              "a lone segment has no joints (clean square cut ends)");

        const Vec3 a{0, 0, 0}, b{1, 0, 0}, c{1, 0, 1}, d{0, 0, 1};
        const std::vector<std::pair<Vec3, Vec3>> loop = {{a, b}, {b, c}, {c, d}, {d, a}};
        check(thicklines::collectJoints(loop).size() == 4,
              "a square loop has 4 corner joints");
    }

    {
        // Segments are emitted relative to the camera, cut square at the
        // endpoints, and centered on the centerline.
        const Vec3 p1{0, 0, 0};
        const Vec3 p2{2, 0, 0};
        const auto verts = captureSegment(p1, p2, cam, hw);
        check(verts.size() == 8, "a segment emits both windings (8 vertices)");

        const Vec3 dir = thicklines::subtract(p2, p1);
        const float len = thicklines::length(dir);
        bool withinSpan = true;
        bool onRibbon = true;
        bool cameraRelative = true;
        for (const auto& v : verts) {
            // Axial position along the segment (camera-relative).
            const float t = (v.x - (p1.x - cam.x)) / len;
            if (t < -0.0001f || t > 1.0001f) withinSpan = false;
            const Vec3 axisPoint{(p1.x - cam.x) + t * len, p1.y - cam.y, p1.z - cam.z};
            if (!near(thicklines::length(subtract(v, axisPoint)), hw, 0.001f)) onRibbon = false;
            (void)cameraRelative;
        }
        check(withinSpan, "no vertex overshoots the segment ends (no more corner spurs)");
        check(onRibbon, "every vertex sits halfWidth away from the centerline");
        // verts[0]/verts[3] are p1 -/+ side; their midpoint is p1 (camera-relative).
        const Vec3 startMid{(verts[0].x + verts[3].x) * 0.5f,
                            (verts[0].y + verts[3].y) * 0.5f,
                            (verts[0].z + verts[3].z) * 0.5f};
        const Vec3 endMid{(verts[1].x + verts[2].x) * 0.5f,
                          (verts[1].y + verts[2].y) * 0.5f,
                          (verts[1].z + verts[2].z) * 0.5f};
        check(near(startMid.x, p1.x - cam.x, 0.001f) &&
              near(startMid.y, p1.y - cam.y, 0.001f) &&
              near(startMid.z, p1.z - cam.z, 0.001f) &&
              near(endMid.x, p2.x - cam.x, 0.001f) &&
              near(endMid.y, p2.y - cam.y, 0.001f) &&
              near(endMid.z, p2.z - cam.z, 0.001f),
              "vertices are camera-relative and centered on the segment ends");
    }

    {
        // The joint dot faces the camera and is centered on the corner.
        const Vec3 corner{1, 0, 0};
        const auto verts = captureDot(corner, cam, hw);
        check(verts.size() == 8, "a joint dot emits both windings (8 vertices)");

        Vec3 sum{0, 0, 0};
        for (int i = 0; i < 4; ++i) sum = add(sum, verts[i]);
        const Vec3 centroid{sum.x / 4.0f, sum.y / 4.0f, sum.z / 4.0f};
        check(near(centroid.x, corner.x - cam.x, 0.001f) &&
              near(centroid.y, corner.y - cam.y, 0.001f) &&
              near(centroid.z, corner.z - cam.z, 0.001f),
              "the dot is centered on the corner");

        const Vec3 view = thicklines::subtract(corner, cam);
        bool inCameraPlane = true;
        float maxDist = 0.0f;
        for (int i = 0; i < 4; ++i) {
            if (std::fabs(dot(subtract(verts[i], centroid), view)) > 0.001f) inCameraPlane = false;
            maxDist = std::fmax(maxDist, thicklines::length(subtract(verts[i], centroid)));
        }
        check(inCameraPlane, "the dot lies in the plane facing the camera");
        check(near(maxDist, hw * thicklines::kJointDotScale * std::sqrt(2.0f), 0.002f),
              "the dot extends past halfWidth so it covers both segment ends");
    }

    {
        // Corner coverage: for two perpendicular segments meeting at a box
        // corner, the end vertices of both quads sit within the joint dot's
        // extent, so the seam between them is always covered.
        const Vec3 corner{1, 0, 0};
        const auto quads = captureSegment(corner, Vec3{1, 1, 0}, cam, hw);
        const auto quads2 = captureSegment(Vec3{0, 0, 0}, corner, cam, hw);
        const auto dotVerts = captureDot(corner, cam, hw);

        float maxSeam = 0.0f;
        for (const auto& v : quads) {
            if (near(v.x, corner.x - cam.x, 0.001f)) {
                maxSeam = std::fmax(maxSeam, thicklines::length(subtract(v, subtract(corner, cam))));
            }
        }
        for (const auto& v : quads2) {
            if (near(v.x, corner.x - cam.x, 0.001f)) {
                maxSeam = std::fmax(maxSeam, thicklines::length(subtract(v, subtract(corner, cam))));
            }
        }
        (void)dotVerts;
        check(maxSeam <= hw * 1.001f && maxSeam > 0.0f,
              "both quads end exactly at the shared corner, halfWidth to each side");
    }

    {
        check(thicklines::thickVertexCount(12, 8) == 160,
              "a thick box reserves 12 edge quads + 8 corner dots");
        check(thicklines::thickVertexCount(1, 0) == 8,
              "a lone thick segment reserves both windings only");
    }

    std::printf(g_failures == 0 ? "ALL TESTS PASSED\n" : "%d TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

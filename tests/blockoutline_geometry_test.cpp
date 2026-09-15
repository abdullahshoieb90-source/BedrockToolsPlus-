// Host-side tests for Block Outline's world geometry and animation helpers.
//
// Build: g++ -std=c++20 -I include -I src tests/blockoutline_geometry_test.cpp
//        -o /tmp/blockoutline_geometry_test
// Run:   /tmp/blockoutline_geometry_test

#include "modules/visual/blockoutline_geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) {
        std::printf("  ok   %s\n", message);
    } else {
        std::printf("  FAIL %s\n", message);
        ++failures;
    }
}

bool near(float a, float b, float epsilon = 0.0001f) {
    return std::fabs(a - b) <= epsilon;
}

using bedrocktools::sdk::Vec3;

float length(const Vec3& a, const Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                     (a.z - b.z) * (a.z - b.z));
}

} // namespace

int main() {
    std::printf("block outline geometry\n");

    const bedrocktools::sdk::BlockPos position{-4, 63, 9};
    const blockoutline::Box box = blockoutline::makeBlockBox(position, 0.002f);
    check(near(box.min.x, -4.002f) && near(box.min.y, 62.998f) && near(box.min.z, 8.998f),
          "selection box expands below the voxel bounds");
    check(near(box.max.x, -2.998f) && near(box.max.y, 64.002f) && near(box.max.z, 10.002f),
          "selection box expands above the voxel bounds");

    const auto edges = blockoutline::boxEdges(box);
    check(edges.size() == 12, "cube has exactly 12 outline edges");

    int xEdges = 0;
    int yEdges = 0;
    int zEdges = 0;
    bool axisAligned = true;
    for (const auto& edge : edges) {
        const bool xDiff = !near(edge.from.x, edge.to.x);
        const bool yDiff = !near(edge.from.y, edge.to.y);
        const bool zDiff = !near(edge.from.z, edge.to.z);
        const int changedAxes = static_cast<int>(xDiff) + static_cast<int>(yDiff) + static_cast<int>(zDiff);
        if (changedAxes != 1) axisAligned = false;
        xEdges += xDiff ? 1 : 0;
        yEdges += yDiff ? 1 : 0;
        zEdges += zDiff ? 1 : 0;
    }
    check(axisAligned, "every outline edge is axis aligned (no diagonal artifacts)");
    check(xEdges == 4 && yEdges == 4 && zEdges == 4,
          "outline contains four edges on each axis");

    std::vector<blockoutline::Edge> segments;
    blockoutline::collectBoxEdges({box, box}, segments);
    check(segments.size() == 24, "flattening two boxes yields twenty-four line segments");
    check(near(segments.front().from.x, edges.front().from.x) &&
              near(segments[12].from.x, edges.front().from.x),
          "flattened segments keep the boxEdges order of every box");
    blockoutline::collectBoxEdges({}, segments);
    check(segments.empty(), "the reused buffer is cleared instead of growing every frame");

    std::printf("block outline tapered beams\n");
    check(near(blockoutline::screenConstantHalfWidth(0.01f, {0.0f, 0.0f, 10.0f},
                                                     {0.0f, 0.0f, 0.0f}, 0.2f), 0.1f),
          "a screen-constant width grows with the distance from the camera");
    check(near(blockoutline::screenConstantHalfWidth(0.01f, {0.0f, 0.0f, 0.05f},
                                                     {0.0f, 0.0f, 0.0f}, 0.2f), 0.002f),
          "the floor keeps the end nearest the camera from collapsing to nothing");

    const blockoutline::Edge ray{{0.0f, 0.0f, 0.2f}, {0.0f, 0.0f, 10.0f}};
    std::array<Vec3, 4> strip{};
    check(blockoutline::makeTaperedBeam(ray, {0.0f, 0.0f, 0.0f}, 0.001f, 0.05f, strip),
          "a tapered strip builds along an eye ray");
    check(near(length(strip[3], strip[0]), 0.002f), "the near end keeps its own width");
    check(near(length(strip[2], strip[1]), 0.1f), "the far end keeps its own, larger width");
    check(!blockoutline::makeTaperedBeam(
              blockoutline::Edge{{1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}},
              {0.0f, 0.0f, 0.0f}, 0.01f, 0.01f, strip),
          "a zero-length segment builds no strip");

    // With one width at both ends the tapered builder has to agree with the
    // plain edge beam the box outlines use.
    const blockoutline::Edge edge{{0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}};
    std::array<Vec3, 4> tapered{};
    std::array<Vec3, 4> plain{};
    check(blockoutline::makeTaperedBeam(edge, {0.0f, 0.0f, 0.0f}, 0.01f, 0.01f, tapered) &&
              blockoutline::makeEdgeBeam(edge, {0.0f, 0.0f, 0.0f}, 0.01f, plain),
          "both beam builders accept the same segment");
    bool sameCorners = true;
    for (int i = 0; i < 4; ++i) {
        if (!near(tapered[i].x, plain[i].x) || !near(tapered[i].y, plain[i].y) ||
            !near(tapered[i].z, plain[i].z)) {
            sameCorners = false;
        }
    }
    check(sameCorners, "a uniform width reproduces the plain camera-facing beam");

    // One side direction for the whole ribbon is what keeps its four corners in
    // one plane, so its two long edges can never cross into a twisted bowtie.
    {
        const blockoutline::Edge longEdge{{0.0f, -0.4f, 1.0f}, {3.0f, 0.4f, 12.0f}};
        std::array<Vec3, 4> ribbon{};
        check(blockoutline::makeTaperedBeam(longEdge, {0.0f, 0.0f, 0.0f}, 0.004f, 0.05f, ribbon),
              "a long tapered ribbon builds");
        const Vec3 acrossNear{ribbon[3].x - ribbon[0].x, ribbon[3].y - ribbon[0].y,
                              ribbon[3].z - ribbon[0].z};
        const Vec3 acrossFar{ribbon[2].x - ribbon[1].x, ribbon[2].y - ribbon[1].y,
                             ribbon[2].z - ribbon[1].z};
        const float nearCross = std::sqrt(acrossNear.x * acrossNear.x +
                                          acrossNear.y * acrossNear.y +
                                          acrossNear.z * acrossNear.z);
        const float farCross = std::sqrt(acrossFar.x * acrossFar.x +
                                         acrossFar.y * acrossFar.y +
                                         acrossFar.z * acrossFar.z);
        const float parallelism = (acrossNear.x * acrossFar.x + acrossNear.y * acrossFar.y +
                                  acrossNear.z * acrossFar.z) / (nearCross * farCross);
        check(near(parallelism, 1.0f, 0.0001f),
              "both ends of the ribbon widen along the same direction");
        const Vec3 normal{acrossNear.y * acrossFar.z - acrossNear.z * acrossFar.y,
                          acrossNear.z * acrossFar.x - acrossNear.x * acrossFar.z,
                          acrossNear.x * acrossFar.y - acrossNear.y * acrossFar.x};
        check(near(normal.x, 0.0f) && near(normal.y, 0.0f) && near(normal.z, 0.0f),
              "the ribbon is planar, not twisted");
    }

    std::printf("block outline eye-ray ribbons\n");
    {
        // Both ends on one eye ray: the ribbon's plane then holds the eye
        // whichever perpendicular it widens along, so the segment is seen
        // edge-on and covers no pixels however wide it is built. No widening
        // trick can rescue a camera-anchored line — the anchor itself has to
        // move off the eye ray (see the Storage ESP tracer origins).
        const blockoutline::Edge atCamera{{0.0f, 0.0f, 0.2f}, {0.0f, 0.0f, 10.0f}};
        std::array<Vec3, 4> ribbon{};
        check(blockoutline::makeTaperedBeam(atCamera, {0.0f, 0.0f, 0.0f}, 0.001f, 0.05f, ribbon),
              "a tapered ribbon still builds for a segment pointing at the camera");
        const Vec3 axis{ribbon[1].x + ribbon[2].x - ribbon[0].x - ribbon[3].x,
                        ribbon[1].y + ribbon[2].y - ribbon[0].y - ribbon[3].y,
                        ribbon[1].z + ribbon[2].z - ribbon[0].z - ribbon[3].z};
        const Vec3 across{ribbon[3].x - ribbon[0].x, ribbon[3].y - ribbon[0].y,
                          ribbon[3].z - ribbon[0].z};
        const Vec3 normal{axis.y * across.z - axis.z * across.y,
                          axis.z * across.x - axis.x * across.z,
                          axis.x * across.y - axis.y * across.x};
        check(near(normal.x * ribbon[0].x + normal.y * ribbon[0].y + normal.z * ribbon[0].z, 0.0f),
              "its plane holds the eye, so it is edge-on and covers no pixels");

        // The same line anchored below the eye is no longer on a single eye
        // ray: its ribbon faces the camera, which is what a tracer needs.
        const blockoutline::Edge fromBelow{{0.0f, -0.45f, 0.0f}, {0.0f, 0.0f, 10.0f}};
        check(blockoutline::makeTaperedBeam(fromBelow, {0.0f, 0.0f, 0.0f}, 0.001f, 0.05f, ribbon),
              "a tapered ribbon builds for a line that starts below the camera");
        const Vec3 belowAxis{ribbon[1].x + ribbon[2].x - ribbon[0].x - ribbon[3].x,
                             ribbon[1].y + ribbon[2].y - ribbon[0].y - ribbon[3].y,
                             ribbon[1].z + ribbon[2].z - ribbon[0].z - ribbon[3].z};
        const Vec3 belowAcross{ribbon[3].x - ribbon[0].x, ribbon[3].y - ribbon[0].y,
                               ribbon[3].z - ribbon[0].z};
        const Vec3 belowNormal{belowAxis.y * belowAcross.z - belowAxis.z * belowAcross.y,
                               belowAxis.z * belowAcross.x - belowAxis.x * belowAcross.z,
                               belowAxis.x * belowAcross.y - belowAxis.y * belowAcross.x};
        check(!near(belowNormal.x * ribbon[0].x + belowNormal.y * ribbon[0].y +
                        belowNormal.z * ribbon[0].z,
                    0.0f),
              "anchored below the eye, the same line faces the camera instead");
        check(near(length(ribbon[3], ribbon[0]), 0.002f) &&
                  near(length(ribbon[2], ribbon[1]), 0.1f),
              "the dropped-anchor ribbon keeps a width at each of its ends");
    }

    const auto faces = blockoutline::boxFaces(box);
    check(faces.size() == 6, "full fill contains six faces");
    bool facePlanesCorrect = true;
    for (int facing = blockoutline::Down; facing <= blockoutline::East; ++facing) {
        const auto face = blockoutline::boxFace(box, facing);
        for (const auto& vertex : face) {
            switch (facing) {
                case blockoutline::Down:  facePlanesCorrect &= near(vertex.y, box.min.y); break;
                case blockoutline::Up:    facePlanesCorrect &= near(vertex.y, box.max.y); break;
                case blockoutline::North: facePlanesCorrect &= near(vertex.z, box.min.z); break;
                case blockoutline::South: facePlanesCorrect &= near(vertex.z, box.max.z); break;
                case blockoutline::West:  facePlanesCorrect &= near(vertex.x, box.min.x); break;
                case blockoutline::East:  facePlanesCorrect &= near(vertex.x, box.max.x); break;
            }
        }
    }
    check(facePlanesCorrect, "face-only fill follows all six Bedrock facing values");
    check(blockoutline::validFacing(0) && blockoutline::validFacing(5) &&
          !blockoutline::validFacing(-1) && !blockoutline::validFacing(6),
          "invalid HitResult facing values are rejected");

    std::printf("block outline effects\n");
    check(blockoutline::hsvToRgb(0.0f) == 0xFF0000u, "rainbow hue 0 is red");
    check(blockoutline::hsvToRgb(120.0f) == 0x00FF00u, "rainbow hue 120 is green");
    check(blockoutline::hsvToRgb(240.0f) == 0x0000FFu, "rainbow hue 240 is blue");
    check(blockoutline::hsvToRgb(360.0f) == 0xFF0000u, "rainbow hue wraps at 360 degrees");
    check(blockoutline::animatedRgb(0xAA123456u, false, 10.0, 0.5f) == 0x123456u,
          "static color keeps RGB and discards stored alpha");
    check(blockoutline::animatedRgb(0, true, 0.0, 1.0f) == 0xFF0000u,
          "rainbow animation starts from a deterministic hue");

    float pulseMin = 2.0f;
    float pulseMax = -1.0f;
    for (int i = 0; i <= 200; ++i) {
        const float value = blockoutline::pulseMultiplier(true, i / 100.0, 1.0f);
        pulseMin = std::min(pulseMin, value);
        pulseMax = std::max(pulseMax, value);
    }
    check(pulseMin >= 0.419f && pulseMax <= 1.001f,
          "pulse multiplier remains visible and never exceeds full opacity");
    check(pulseMax - pulseMin > 0.55f, "pulse has a clearly visible animation range");
    check(near(blockoutline::pulseMultiplier(false, 0.25, 1.0f), 1.0f),
          "disabled pulse leaves opacity unchanged");
    check(near(blockoutline::clampedOpacity(2.0f, 0.5f), 0.5f) &&
          near(blockoutline::clampedOpacity(-1.0f, 1.0f), 0.0f),
          "opacity is clamped before rendering");

    std::printf("\n");
    if (failures != 0) {
        std::printf("%d block outline check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("all block outline checks passed\n");
    return 0;
}

// Host-side tests for Block Outline's world geometry and animation helpers.
//
// Build: g++ -std=c++20 -I include -I src tests/blockoutline_geometry_test.cpp
//        -o /tmp/blockoutline_geometry_test
// Run:   /tmp/blockoutline_geometry_test

#include "modules/visual/blockoutline_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

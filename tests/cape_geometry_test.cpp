// Host test for the world-space cape geometry (cape_geometry.hpp).
//
// The renderer itself needs the game, but the mesh it draws does not: this
// covers the shape, the flap response and the invariants that keep the cape
// attached to the player.

#include "modules/player/cape_geometry.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace cape = bedrocktools::modules::cape;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++failures;
    }
}

bool near(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }

constexpr float kEps = 1e-5f;

} // namespace

int main() {
    std::printf("cape geometry: point counts and connection\n");

    cape::Vertex verts[32]{};
    cape::Vec3 right{}, forward{};
    cape::bodyAxes(0.0f, right, forward);
    const cape::Vec3 feet{0.0f, 64.0f, 0.0f};

    const int count = cape::buildCapeMesh(feet, right, forward, 10.0f, 0.0f, verts, 32);
    check(count == 12, "the cape is three quads (outer, lining, hem)");

    // Every vertex must sit at or below the shoulders and stay within one cape
    // width of the player's centre, or the cape would float off the body.
    bool insideBody = true;
    bool belowShoulder = true;
    for (int i = 0; i < count; ++i) {
        const cape::Vec3& p = verts[i].p;
        const float lateral = std::fabs(p.x) + std::fabs(p.z);
        if (lateral > cape::kShoulderHalfWidth + 0.4f) insideBody = false;
        if (p.y > feet.y + cape::kShoulderHeight + kEps) belowShoulder = false;
    }
    check(insideBody, "the cape stays within the player's silhouette");
    check(belowShoulder, "nothing is drawn above the shoulders");

    // The top edge is anchored to the shoulders, so the highest vertices are
    // exactly the shoulder height regardless of the flap.
    float maxY = -1e30f;
    for (int i = 0; i < count; ++i) maxY = std::max(maxY, verts[i].p.y);
    check(near(maxY, feet.y + cape::kShoulderHeight), "the top edge is pinned to the shoulders");

    std::printf("cape geometry: the flap moves the hem, not the mount\n");
    for (const float angle : {0.0f, 20.0f, 45.0f, 75.0f}) {
        float lowest = 1e30f;
        int lowestIndex = -1;
        for (int i = 0; i < count; ++i) {
            if (verts[i].p.y < lowest) { lowest = verts[i].p.y; lowestIndex = i; }
        }
        check(lowestIndex >= 0 && lowest < feet.y + cape::kShoulderHeight,
              "the hem hangs below the shoulders at " + std::to_string(static_cast<int>(angle)) + " deg");
    }

    // A hanging cape reaches (almost) its full length; a lifted one is shorter
    // vertically and further back, which is the whole point of the flap.
    cape::Vertex flat[32]{};
    cape::Vertex lifted[32]{};
    cape::buildCapeMesh(feet, right, forward, 0.0f, 0.0f, flat, 32);
    cape::buildCapeMesh(feet, right, forward, 75.0f, 0.0f, lifted, 32);

    auto lowestY = [](const cape::Vertex* v, int n) {
        float y = 1e30f;
        for (int i = 0; i < n; ++i) y = std::min(y, v[i].p.y);
        return y;
    };
    check(lowestY(flat, count) < lowestY(lifted, count) - 0.1f,
          "a lifted cape hangs higher than a hanging one");

    std::printf("cape geometry: flap responds to motion\n");
    const float still = cape::flapAngleDegrees(0.0f, 0.0f, true);
    const float walking = cape::flapAngleDegrees(4.3f, 0.0f, true);
    const float falling = cape::flapAngleDegrees(0.0f, -1.5f, false);
    const float jumping = cape::flapAngleDegrees(0.0f, 1.0f, false);
    check(walking > still, "walking lifts the cape");
    check(falling > walking, "falling streams the cape higher than walking");
    check(jumping < still + 1.0f, "jumping keeps the cape down");
    check(still >= 0.0f && falling <= 75.0f, "the flap stays inside its limits");

    std::printf("cape geometry: yaw rotates the cape with the body\n");
    cape::Vec3 rightNorth{}, forwardNorth{};
    cape::Vec3 rightEast{}, forwardEast{};
    cape::bodyAxes(0.0f, rightNorth, forwardNorth);
    cape::bodyAxes(90.0f, rightEast, forwardEast);
    check(near(rightNorth.x * rightEast.x + rightNorth.z * rightEast.z, 0.0f, 1e-5f),
          "a quarter turn makes the axes perpendicular");
    check(near(rightEast.x, -0.0f, 1e-5f) && near(rightEast.z, -1.0f, 1e-5f),
          "the right axis follows the body yaw");

    const cape::Vec3 offsetFeet{-32.0f, 70.0f, 128.0f};
    cape::Vertex moved[32]{};
    const int movedCount = cape::buildCapeMesh(offsetFeet, rightNorth, forwardNorth, 10.0f, 0.0f, moved, 32);
    bool rigid = true;
    for (int i = 0; i < movedCount; ++i) {
        if (!near(moved[i].p.x, verts[i].p.x - 32.0f, 1e-3f) ||
            !near(moved[i].p.y, verts[i].p.y + 6.0f, 1e-3f) ||
            !near(moved[i].p.z, verts[i].p.z + 128.0f, 1e-3f)) rigid = false;
    }
    check(rigid, "the mesh follows the player instead of the world origin");

    std::printf("cape geometry: UVs sample the classic layout\n");
    const cape::CapeUv uv = cape::capeUv();
    check(near(uv.back[0].u, 1.0f / 64.0f) && near(uv.back[0].v, 1.0f / 32.0f),
          "the outer face starts at the classic (1,1) texel");
    check(near(uv.back[2].u, 11.0f / 64.0f) && near(uv.back[2].v, 17.0f / 32.0f),
          "the outer face is 10x16 texels");
    bool insideCanvas = true;
    for (const auto* face : {uv.back, uv.front, uv.bottom}) {
        for (int i = 0; i < 4; ++i) {
            if (face[i].u < 0.0f || face[i].u > 1.0f || face[i].v < 0.0f || face[i].v > 1.0f) {
                insideCanvas = false;
            }
        }
    }
    check(insideCanvas, "every UV stays inside the cape canvas");

    // The mesh has to carry the UV itself: the tessellator is fed per vertex,
    // so a quad whose corners disagree about the face would smear the texture.
    check(near(verts[0].uv.u, uv.back[0].u) && near(verts[1].uv.u, uv.back[1].u),
          "the submitted mesh carries the outer-face UVs");

    std::printf("cape geometry: degenerate inputs are rejected\n");
    check(cape::buildCapeMesh(feet, right, forward, 0.0f, 0.0f, nullptr, 32) == 0,
          "no output buffer means no geometry");
    cape::Vertex tiny[4]{};
    check(cape::buildCapeMesh(feet, right, forward, 0.0f, 0.0f, tiny, 4) == 0,
          "a too-small buffer means no geometry (no overflow)");

    if (failures == 0) std::printf("\nall cape geometry checks passed\n");
    else std::printf("\n%d cape geometry check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}

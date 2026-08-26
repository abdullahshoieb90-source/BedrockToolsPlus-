// Host-side unit test for the Wings shape/shading helpers.
//
// The Wings overlay draws every bone as a small tapered prism and shades each
// face with the helpers in src/modules/visual/wings_shape.hpp +
// wings_styles.hpp (the same code the game executes in renderWingsOverlay).
// This test verifies, without Minecraft:
//
//   * every face of every bone of every style is a proper closed box face:
//     its four ring corners share the fixed axis coordinate (so the quad is a
//     real face and not a diagonal slice through the box) and form a simple
//     convex ring (so Bedrock's quad mode draws the whole face and not a
//     bowtie half)
//   * the legacy face table fails both checks (regression proof)
//   * the mirrored left tables are exact mirrors of the right tables
//   * taper only ever narrows the far edge and never collapses it
//   * the sweep keeps every wing box behind the back (z >= 2.5), so nothing
//     clips the torso
//   * at the rest pose the wings stay clear of the player's torso
//   * the shading model keeps faces in a sane brightness range, lights
//     upward-facing faces more than downward-facing ones, clamps channels and
//     brightens the wing tips relative to the shoulder
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src -I include tests/wings_shape_test.cpp -o /tmp/wings_shape_test
//     /tmp/wings_shape_test

#include "modules/visual/wings_shape.hpp"
#include "modules/visual/wings_styles.hpp"

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

// Corner bits: 0 = x, 1 = y, 2 = z. A face of the prism is a true box face
// when its ring walks the four corners of one side of the cube: consecutive
// ring corners differ in exactly one bit, and all four corners agree on the
// bit the face fixes. A bowtie ordering or a diagonal slice (the two legacy
// bugs) breaks the one-bit-walk.
int faceFixedBit(int face) {
    switch (face) {
        case wings::kFaceInner:
        case wings::kFaceOuter: return 2;
        case wings::kFaceSpanMin:
        case wings::kFaceSpanMax: return 0;
        default: return 1;
    }
}

bool faceIsRealBoxFace(int face) {
    const int* ring = wings::kFaceRings[face];
    const int bit = faceFixedBit(face);
    const int fixedValue = (ring[0] >> bit) & 1;
    for (int i = 0; i < 4; ++i) {
        if (((ring[i] >> bit) & 1) != fixedValue) return false;
        const int next = ring[(i + 1) & 3];
        const int diff = ring[i] ^ next;
        if (diff != 1 && diff != 2 && diff != 4) return false;  // exactly one bit
    }
    return true;
}

bool faceIsConvex(const wings::WingBox& box, int face) {
    const int* ring = wings::kFaceRings[face];
    float p[4][3];
    for (int i = 0; i < 4; ++i) {
        p[i][0] = box.px[ring[i]];
        p[i][1] = box.py[ring[i]];
        p[i][2] = box.pz[ring[i]];
    }
    return wings::isConvexRing(p);
}

wings::WingBone makeBone(float restDeg, float sweepPx, float taperPx, float spanT, float tint) {
    wings::WingBone b;
    b.restDeg = restDeg;
    b.sweepPx = sweepPx;
    b.taperPx = taperPx;
    b.spanT = spanT;
    b.tint = tint;
    return b;
}

}  // namespace

int main() {
    std::printf("wings shape + shading invariants\n");

    // ------------------------------------------------------------------
    // Every face of every bone of every style is a proper closed face,
    // at the rest pose and at strong flap poses.
    // ------------------------------------------------------------------
    for (int s = 0; s < wings::kWingStyleCount; ++s) {
        const wings::WingStyle& style = wings::kWingStyles[s];
        for (const float probeAngle : {0.0f, 35.0f, -35.0f}) {
            wings::Pose2D poses[wings::kMaxWingBones];
            bool allReal = true;
            bool allConvex = true;
            for (int i = 0; i < style.boneCount; ++i) {
                const wings::WingBone& bone = style.rightBones[i];
                const float angleRad = -1.0f * (probeAngle + bone.restDeg) * wings::kDegToRad;
                poses[i] = (bone.parent < 0 || bone.parent >= i)
                               ? wings::makePose(angleRad, wings::kRightRootPivotX, wings::kRootPivotY)
                               : wings::composePose(poses[bone.parent], bone.anchorX, bone.anchorY, angleRad);
                const wings::WingBox box = wings::buildWingBox(bone, poses[i]);
                for (int f = 0; f < wings::kFaceCount; ++f) {
                    if (!faceIsRealBoxFace(f)) allReal = false;
                    if (!faceIsConvex(box, f)) allConvex = false;
                }
            }
            check(allReal, std::string(style.id) + " probe " + std::to_string(probeAngle) +
                               ": all faces are real box faces (no diagonal slices)");
            check(allConvex, std::string(style.id) + " probe " + std::to_string(probeAngle) +
                                 ": all faces are simple convex rings (no bowties)");
        }
    }

    // ------------------------------------------------------------------
    // The legacy face table must FAIL the same checks - that is the bug the
    // old renderer shipped with.
    // ------------------------------------------------------------------
    {
        wings::WingBone bone = makeBone(0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
        bone.boxOX = -6.0f; bone.boxOY = -1.5f; bone.boxSX = 6.0f; bone.boxSY = 3.0f;
        bone.zMin = 3.0f; bone.zMax = 4.0f;
        const wings::WingBox box = wings::buildWingBox(bone, wings::Pose2D{});

        constexpr int kLegacyZMin[4] = {0, 1, 2, 3};     // bowtie ordering
        float p[4][3];
        for (int i = 0; i < 4; ++i) {
            p[i][0] = box.px[kLegacyZMin[i]];
            p[i][1] = box.py[kLegacyZMin[i]];
            p[i][2] = box.pz[kLegacyZMin[i]];
        }
        check(!wings::isConvexRing(p), "legacy zMin ring is a bowtie and fails the convexity check");

        constexpr int kLegacyXMin[4] = {0, 3, 7, 4};     // diagonal slice
        bool mixedX = false;
        for (int i = 1; i < 4; ++i) {
            if (std::fabs(box.px[kLegacyXMin[i]] - box.px[kLegacyXMin[0]]) > 1e-4f) mixedX = true;
        }
        check(mixedX, "legacy xMin ring mixes both x sides (diagonal slice through the box)");
    }

    // ------------------------------------------------------------------
    // Mirror tables
    // ------------------------------------------------------------------
    for (int s = 0; s < wings::kWingStyleCount; ++s) {
        const wings::WingStyle& style = wings::kWingStyles[s];
        wings::WingBone left[wings::kMaxWingBones];
        wings::mirrorWingBones(style.rightBones, style.boneCount, left);
        bool mirrored = true;
        for (int i = 0; i < style.boneCount; ++i) {
            const wings::WingBone& r = style.rightBones[i];
            if (left[i].parent != r.parent) mirrored = false;
            if (std::fabs(left[i].anchorX + r.anchorX) > 1e-5f) mirrored = false;
            if (std::fabs(left[i].boxOX + r.boxOX + r.boxSX) > 1e-5f) mirrored = false;
            if (std::fabs(left[i].boxSX - r.boxSX) > 1e-5f) mirrored = false;
            if (left[i].restDeg != r.restDeg || left[i].sweepPx != r.sweepPx ||
                left[i].taperPx != r.taperPx) mirrored = false;
        }
        check(mirrored, std::string(style.id) + " left table mirrors the right table");
    }

    // ------------------------------------------------------------------
    // Taper: the near edge is untouched, the far edge never collapses.
    // ------------------------------------------------------------------
    {
        wings::WingBone bone = makeBone(0.0f, 0.0f, 1.2f, 0.5f, 1.0f);
        bone.boxOX = -1.0f; bone.boxOY = -6.0f; bone.boxSX = 2.0f; bone.boxSY = 6.0f;
        bone.zMin = 3.0f; bone.zMax = 4.0f;
        const wings::WingBox box = wings::buildWingBox(bone, wings::Pose2D{});

        // The long axis is y, so the width is measured on x. Corner 0/1 sit at
        // the far edge (y = -6), corners 2/3 at the near edge (y = 0).
        const float farWidth = std::fabs(box.px[0] - box.px[1]);
        const float nearWidth = std::fabs(box.px[3] - box.px[2]);
        check(std::fabs(nearWidth - 2.0f) < 1e-4f, "taper keeps the near edge at full width");
        check(std::fabs(farWidth - 0.8f) < 1e-4f, "taper narrows the far edge by taperPx");

        // An absurd taper is clamped so the far edge keeps >= 10% of the side.
        wings::WingBone extreme = bone;
        extreme.taperPx = 100.0f;
        const wings::WingBox clamped = wings::buildWingBox(extreme, wings::Pose2D{});
        const float clampedWidth = std::fabs(clamped.px[0] - clamped.px[1]);
        check(clampedWidth >= 0.19f, "taper is clamped and the far edge never collapses");
    }

    // ------------------------------------------------------------------
    // Sweep keeps every wing box behind the back (z >= 2.5) and the rest
    // pose keeps the wings clear of the torso.
    // ------------------------------------------------------------------
    for (int s = 0; s < wings::kWingStyleCount; ++s) {
        const wings::WingStyle& style = wings::kWingStyles[s];
        bool behind = true;
        bool clearOfTorso = true;
        wings::Pose2D poses[wings::kMaxWingBones];
        for (int i = 0; i < style.boneCount; ++i) {
            const wings::WingBone& bone = style.rightBones[i];
            const float angleRad = -1.0f * bone.restDeg * wings::kDegToRad;
            poses[i] = (bone.parent < 0 || bone.parent >= i)
                           ? wings::makePose(angleRad, wings::kRightRootPivotX, wings::kRootPivotY)
                           : wings::composePose(poses[bone.parent], bone.anchorX, bone.anchorY, angleRad);
            const wings::WingBox box = wings::buildWingBox(bone, poses[i]);

            float zMin = 1e9f;
            float xMin = 1e9f, xMax = -1e9f, yMin = 1e9f, yMax = -1e9f;
            for (int c = 0; c < 8; ++c) {
                if (box.pz[c] < zMin) zMin = box.pz[c];
                if (box.px[c] < xMin) xMin = box.px[c];
                if (box.px[c] > xMax) xMax = box.px[c];
                if (box.py[c] < yMin) yMin = box.py[c];
                if (box.py[c] > yMax) yMax = box.py[c];
            }
            if (zMin < 2.5f - 1e-3f) behind = false;

            // Torso: x in [-4, 4], y in [12, 24], z in [-2, 2].
            const bool overlapX = xMax > -4.0f && xMin < 4.0f;
            const bool overlapY = yMax > 12.0f && yMin < 24.0f;
            const bool overlapZ = zMin < 2.0f;
            if (overlapX && overlapY && overlapZ) clearOfTorso = false;
        }
        check(behind, std::string(style.id) + " wings stay behind the back (z >= 2.5)");
        check(clearOfTorso, std::string(style.id) + " rest pose does not intersect the torso");
    }

    // ------------------------------------------------------------------
    // Shading model
    // ------------------------------------------------------------------
    std::printf("shading\n");
    {
        const wings::WingLight& light = wings::kDefaultLight;
        const wings::Vec3 toCam{0.0f, 0.0f, 1.0f};  // camera behind the player, facing the back

        const float top = wings::faceBrightness({0.0f, 1.0f, 0.0f}, toCam, light);
        const float bottom = wings::faceBrightness({0.0f, -1.0f, 0.0f}, toCam, light);
        const float back = wings::faceBrightness({0.0f, 0.0f, 1.0f}, toCam, light);
        check(top > bottom, "upward faces are lit more than downward faces");
        check(back > bottom, "camera-facing back faces are lit more than the underside");
        check(bottom >= light.ambient - 1e-4f, "brightness never drops below ambient");
        check(top <= light.ambient + light.head + light.sky + 1e-4f, "brightness never exceeds the model range");

        check(wings::shadeChannel(200.0f, 2.0f) == 255, "channels clamp at 255");
        check(wings::shadeChannel(200.0f, -1.0f) == 0, "channels clamp at 0");
        check(wings::shadeChannel(0.0f, 1.0f) == 0, "black stays black");

        // Tip gradient: the wing tip reads brighter than the shoulder.
        const float tip = wings::spanTint(1.0f, 1.0f);
        const float shoulder = wings::spanTint(0.0f, 1.0f);
        check(tip > shoulder, "span gradient brightens the wing tips");
        check(std::fabs(tip - (1.0f + wings::kSpanGradient * 0.5f)) < 1e-5f, "tip tint = 1 + gradient/2");
        check(std::fabs(shoulder - (1.0f - wings::kSpanGradient * 0.5f)) < 1e-5f, "shoulder tint = 1 - gradient/2");

        // A shaded face stays within the channel range and tracks the base.
        const unsigned char dark[3] = {18, 18, 24};
        const wings::FaceColor lit = wings::shadeFace(dark, 1.1f);
        check(lit.r > 18 && lit.g > 18 && lit.b > 24, "bright factor lifts the base color");
        const wings::FaceColor dim = wings::shadeFace(dark, 0.6f);
        check(dim.r < 18 && dim.g < 18 && dim.b < 24, "dim factor darkens the base color");
    }

    // ------------------------------------------------------------------
    // Style lookup helpers (shared with the module)
    // ------------------------------------------------------------------
    check(wings::wingStyleIndexForId("phoenix") == 5, "phoenix resolves to index 5");
    check(wings::resolveWingStyleIndex("4,dragon,angel,demon,bat,butterfly,phoenix,fairy") == 4,
          "full radio value resolves to 4");
    check(wings::resolveWingStyleIndex("unicorn") == 0, "unknown style falls back to dragon");
    check(wings::wingStyleRadioValue(2).rfind("2,dragon,angel", 0) == 0, "radio value lists all styles");

    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all wings shape + shading checks passed\n");
    return 0;
}

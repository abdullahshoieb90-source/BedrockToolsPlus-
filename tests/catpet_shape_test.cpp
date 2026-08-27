// Host-side unit test for the Cat Pet module's pure helpers
// (src/modules/visual/catpet_shape.hpp): part hierarchy resolution, the pose
// solver, the palette pickers and the follow ("come to heel") solver.
//
// Build and run (no game, no NDK, no extra packages):
//     g++ -std=c++20 -Wall -Wextra -O1 -I src -I include
//         tests/catpet_shape_test.cpp -o /tmp/catpet_shape_test
//     /tmp/catpet_shape_test

#include "modules/visual/catpet_shape.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace catpet = bedrocktools::modules::catpet;
namespace wings = bedrocktools::modules::wings;

static int g_failures = 0;

static void check(bool cond, const char* what) {
    if (cond) {
        std::printf("  ok: %s\n", what);
    } else {
        std::printf("  FAIL: %s\n", what);
        ++g_failures;
    }
}

static bool nearf(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) <= eps;
}

// Lowest y of every posed part corner (paw contact / ground penetration).
static void poseBounds(const catpet::CatPose& pose, float& minY, float& maxY) {
    catpet::Affine xf[catpet::kCatPartCount];
    catpet::partTransforms(pose, xf);
    minY = 1e30f;
    maxY = -1e30f;
    for (int i = 0; i < catpet::kCatPartCount; ++i) {
        catpet::Vec3 corners[wings::kCornerCount];
        catpet::buildPartCorners(i, xf[i], pose.blink, corners);
        for (int c = 0; c < wings::kCornerCount; ++c) {
            if (corners[c].y < minY) minY = corners[c].y;
            if (corners[c].y > maxY) maxY = corners[c].y;
        }
    }
}

static void testHierarchy() {
    std::printf("cat pet - part hierarchy\n");

    // Parents must come before children so one resolution pass suffices.
    bool ordered = true;
    for (int i = 0; i < catpet::kCatPartCount; ++i) {
        if (catpet::kCatParts[i].parent >= i) ordered = false;
    }
    check(ordered, "part table is ordered parents-first");

    // Neutral pose: identity transforms, feet on the ground, head above body.
    catpet::CatPose neutral{};
    float minY = 0.0f, maxY = 0.0f;
    poseBounds(neutral, minY, maxY);
    check(nearf(minY, 0.0f, 0.05f), "neutral pose: paws rest on y=0");
    check(maxY > 14.0f && maxY < 22.0f, "neutral pose: ear tips are cat-sized");

    // All palette slots used by parts are valid for every style.
    bool slotsValid = true;
    for (int i = 0; i < catpet::kCatPartCount; ++i) {
        int slots[wings::kFaceCount];
        catpet::partFaceSlots(i, slots);
        for (int f = 0; f < wings::kFaceCount; ++f) {
            if (slots[f] < 0 || slots[f] >= catpet::kPaletteSlotCount) slotsValid = false;
        }
    }
    check(slotsValid, "every face resolves to a valid palette slot");

    // Blink shrinks the eyes but leaves everything else untouched.
    catpet::CatPose blink{};
    blink.blink = 1.0f;
    catpet::Affine xf[catpet::kCatPartCount];
    catpet::partTransforms(blink, xf);
    bool eyesShrink = true;
    bool othersStable = true;
    for (int i = 0; i < catpet::kCatPartCount; ++i) {
        catpet::Vec3 open[wings::kCornerCount], closed[wings::kCornerCount];
        catpet::buildPartCorners(i, xf[i], 0.0f, open);
        catpet::buildPartCorners(i, xf[i], 1.0f, closed);
        float openH = open[2].y - open[0].y;      // corner 2 = y1, corner 0 = y0
        float closedH = closed[2].y - closed[0].y;
        if (catpet::kCatParts[i].isEye) {
            if (!(closedH < openH * 0.4f)) eyesShrink = false;
        } else if (!nearf(openH, closedH)) {
            othersStable = false;
        }
    }
    check(eyesShrink, "blink squashes the eye boxes");
    check(othersStable, "blink leaves every other part alone");
}

static void testPoseSolver() {
    std::printf("cat pet - pose solver\n");

    // Idle: no gallop bounce, legs neutral, tail raised.
    const catpet::CatPose idle = catpet::computeCatPose(1.0f, 0.0f, 0.0f, 0.0f);
    check(nearf(idle.hopPx, 0.0f), "idle has no gallop bounce");
    check(std::fabs(idle.legPitchDeg[0]) < 0.01f, "idle legs are neutral");
    check(idle.tailPitchDeg[0] > 30.0f, "idle tail is raised");

    // Running mid-stride: bounce, diagonal leg pairs.
    const float phase = 1.2f;
    const catpet::CatPose run = catpet::computeCatPose(2.0f, phase, 1.0f, 0.0f);
    check(run.hopPx > 0.5f, "running bounces the cat");
    check(nearf(run.legPitchDeg[0], run.legPitchDeg[3]) &&
          nearf(run.legPitchDeg[1], run.legPitchDeg[2]),
          "legs move in diagonal pairs (trot)");
    check(nearf(run.legPitchDeg[0], -run.legPitchDeg[1]),
          "diagonal pairs are in antiphase");
    check(std::fabs(run.legPitchDeg[0]) > 20.0f, "running leg swing is strong");

    // Sitting: chest up, hind legs tucked far more than front legs.
    const catpet::CatPose sit = catpet::computeCatPose(3.0f, 0.0f, 0.0f, 1.0f);
    check(sit.bodyPitchDeg < -15.0f, "sitting pitches the body (chest up)");
    check(sit.legPitchDeg[2] > sit.legPitchDeg[0] + 20.0f,
          "sitting tucks the hind legs");
    float minY = 0.0f, maxY = 0.0f;
    poseBounds(sit, minY, maxY);
    check(minY > -2.5f, "sitting keeps ground penetration tiny");

    // Movement cancels sitting inside the solver.
    const catpet::CatPose moveSit = catpet::computeCatPose(3.0f, 1.0f, 1.0f, 1.0f);
    check(nearf(moveSit.sit, 0.0f), "full movement overrides the sit blend");

    // Blink spikes: mostly open, closed at least sometimes.
    float maxBlink = 0.0f;
    float openShare = 0.0f;
    int samples = 0;
    for (float t = 0.0f; t < 40.0f; t += 0.01f) {
        const catpet::CatPose p = catpet::computeCatPose(t, 0.0f, 0.0f, 0.0f);
        if (p.blink > maxBlink) maxBlink = p.blink;
        if (p.blink < 0.1f) openShare += 1.0f;
        ++samples;
    }
    check(maxBlink > 0.9f, "the cat does blink");
    check(openShare / static_cast<float>(samples) > 0.85f, "eyes are open most of the time");

    // Ear twitches: sharp and occasional.
    float maxEar = 0.0f;
    float quietShare = 0.0f;
    samples = 0;
    for (float t = 0.0f; t < 60.0f; t += 0.01f) {
        const catpet::CatPose p = catpet::computeCatPose(t, 0.0f, 0.0f, 0.0f);
        maxEar = std::max(maxEar, -p.earPitchDeg[0]);
        if (-p.earPitchDeg[0] < 3.0f) quietShare += 1.0f;
        ++samples;
    }
    check(maxEar > 25.0f, "ears twitch hard");
    check(quietShare / static_cast<float>(samples) > 0.8f, "ears are still most of the time");

    // Stride rate grows with speed and stays clamped.
    check(catpet::strideRateForSpeed(0.0f) < catpet::strideRateForSpeed(4.0f),
          "stride rate grows with speed");
    check(catpet::strideRateForSpeed(100.0f) <= 26.0f, "stride rate is clamped");
}

static void testStyles() {
    std::printf("cat pet - styles\n");

    check(catpet::kCatStyleCount >= 4, "at least four coats");
    check(catpet::catStyleIndexForId("orange") == 0, "orange tabby is the default");
    check(catpet::catStyleIndexForId("nope") == 0, "unknown id falls back to default");

    const int siamese = catpet::catStyleIndexForId("siamese");
    check(siamese > 0, "siamese exists");

    // Radio round trip: full radio value, bare index and bare id all resolve.
    const std::string radio = catpet::catStyleRadioValue(siamese);
    check(catpet::resolveCatStyleIndex(radio) == siamese, "radio value round-trips");
    check(catpet::resolveCatStyleIndex(std::to_string(siamese)) == siamese, "numeric index resolves");
    check(catpet::resolveCatStyleIndex("siamese") == siamese, "bare id resolves");
    check(catpet::resolveCatStyleIndex("") == 0, "empty value falls back to default");
    check(catpet::resolveCatStyleIndex("99") == 0, "out-of-range index falls back to default");

    // Every style id appears in the radio list exactly once.
    bool allListed = true;
    for (int i = 0; i < catpet::kCatStyleCount; ++i) {
        if (radio.find(catpet::kCatStyles[i].id) == std::string::npos) allListed = false;
    }
    check(allListed, "radio value lists every style id");
}

static void testFollow() {
    std::printf("cat pet - follow solver\n");

    // First step snaps to the heel spot.
    catpet::FollowState st{};
    catpet::Vec3 owner{10.0f, 64.0f, 10.0f};
    catpet::Vec3 target = catpet::heelTarget(owner, 0.0f, 1.75f);
    catpet::FollowResult res = catpet::stepCatFollow(st, target, owner, 0.0f, 0.05f);
    check(res.teleported, "first step teleports to the heel spot");
    check(nearf(st.x, target.x) && nearf(st.z, target.z), "snap lands on the target");

    // The heel spot is offset from the owner and scales with the pet size.
    const catpet::Vec3 small = catpet::heelTarget(owner, 0.0f, 0.5f);
    const catpet::Vec3 big = catpet::heelTarget(owner, 0.0f, 5.0f);
    const float dSmall = std::hypot(small.x - owner.x, small.z - owner.z);
    const float dBig = std::hypot(big.x - owner.x, big.z - owner.z);
    check(dSmall > 0.5f, "heel spot is not inside the owner");
    check(dBig > dSmall + 1.0f, "bigger cats keep more distance");

    // Owner walks away: the cat chases and eventually settles in the deadzone.
    float ownerZ = 10.0f;
    for (int i = 0; i < 200; ++i) {
        ownerZ += 4.3f * 0.05f;  // owner walks at 4.3 blocks/s
        owner = {10.0f, 64.0f, ownerZ};
        target = catpet::heelTarget(owner, 0.0f, 1.75f);
        res = catpet::stepCatFollow(st, target, owner, 0.0f, 0.05f);
    }
    check(!res.teleported, "steady walking never needs a teleport");
    check(res.speed > 1.0f, "the cat keeps walking while the owner walks");
    float gap = std::hypot(st.x - target.x, st.z - target.z);
    check(gap < 2.0f, "the cat stays near the heel spot while walking");

    // Owner stops: the cat parks inside the deadzone and faces the owner.
    for (int i = 0; i < 400; ++i) {
        res = catpet::stepCatFollow(st, target, owner, 0.0f, 0.05f);
    }
    gap = std::hypot(st.x - target.x, st.z - target.z);
    check(res.speed <= 0.01f, "the cat stops inside the deadzone");
    check(st.smoothedSpeed < 0.05f, "smoothed speed settles to zero");
    const float lookYaw = catpet::catYawFromDir(owner.x - st.x, owner.z - st.z);
    float yawDiff = std::fabs(st.yawDeg - lookYaw);
    while (yawDiff > 180.0f) yawDiff = std::fabs(yawDiff - 360.0f);
    check(yawDiff < 10.0f, "the resting cat looks at its owner");

    // Teleport when left far behind.
    owner = {500.0f, 64.0f, 500.0f};
    target = catpet::heelTarget(owner, 0.0f, 1.75f);
    res = catpet::stepCatFollow(st, target, owner, 0.0f, 0.05f);
    check(res.teleported, "a faraway owner makes the cat teleport");

    // The yaw convention: catBasis(yaw of +z direction) faces +z.
    catpet::Vec3 right{}, forward{};
    catpet::catBasis(catpet::catYawFromDir(0.0f, 1.0f), right, forward);
    check(nearf(forward.x, 0.0f) && nearf(forward.z, 1.0f), "yaw/basis round-trips towards +z");
    catpet::catBasis(catpet::catYawFromDir(1.0f, 0.0f), right, forward);
    check(nearf(forward.x, 1.0f) && nearf(forward.z, 0.0f), "yaw/basis round-trips towards +x");
}

int main() {
    std::printf("cat pet shape/animation/follow tests\n");
    testHierarchy();
    testPoseSolver();
    testStyles();
    testFollow();

    if (g_failures == 0) {
        std::printf("all cat pet checks passed\n");
        return EXIT_SUCCESS;
    }
    std::printf("%d cat pet check(s) FAILED\n", g_failures);
    return EXIT_FAILURE;
}

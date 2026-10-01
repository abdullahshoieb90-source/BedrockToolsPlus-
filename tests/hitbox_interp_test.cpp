// Regression test for Hitbox per-actor sample interpolation.
//
// The collision AABB is a tick sample while the game draws the entity mesh at
// a position interpolated between the last two samples, so every box is drawn
// at lerp(prevSample, curSample, alpha). The phase used to come from the local
// player's tick callback; while gliding with an elytra that callback fires
// off-cadence, the phase collapsed back to zero and the box stayed frozen on
// the previous sample - several blocks behind a firework-boosted glide. The
// phase is now measured from the box samples themselves.
//
//     g++ -std=c++20 -I include -I src tests/hitbox_interp_test.cpp -o /tmp/hitbox_interp_test
//     /tmp/hitbox_interp_test

#include "modules/visual/hitbox_interp.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using bedrocktools::sdk::Vec3;

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

bool near(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}

bool vecNear(const Vec3& v, float x, float y, float z) {
    return near(v.x, x) && near(v.y, y) && near(v.z, z);
}

Vec3 lerp(const Vec3& a, const Vec3& b, float t) {
    return Vec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
} // namespace

int main() {
    std::printf("hitbox sample phase\n");

    // 20 Hz samples: 0 right after one, 1 right before the next.
    check(hitbox::sampleFraction(0.0f, 0.05f) == 0.0f, "just after the sample -> 0");
    check(near(hitbox::sampleFraction(0.025f, 0.05f), 0.5f), "halfway -> 0.5");
    check(hitbox::sampleFraction(0.05f, 0.05f) == 1.0f, "at the next sample -> 1");
    check(hitbox::sampleFraction(0.4f, 0.05f) == 1.0f,
          "a stalled callback must not hold the box behind the newest sample");
    check(hitbox::sampleFraction(-0.01f, 0.05f) == 0.0f, "negative elapsed clamped to 0");
    check(hitbox::sampleFraction(std::nanf(""), 0.05f) == 0.0f, "NaN elapsed does not leak through");
    check(hitbox::sampleFraction(0.01f, 0.0f) == 1.0f, "unknown interval -> raw box");
    check(hitbox::sampleFraction(0.01f, -0.05f) == 1.0f, "negative interval -> raw box");

    std::printf("hitbox sample offset\n");

    const Vec3 prev{9.5f, 64.0f, 10.0f};
    const Vec3 cur{10.0f, 64.0f, 10.0f};

    check(vecNear(hitbox::sampleOffset(prev, cur, 0.0f), -0.5f, 0.0f, 0.0f),
          "alpha 0 -> box on the previous sample (the mesh position)");
    check(vecNear(hitbox::sampleOffset(prev, cur, 0.5f), -0.25f, 0.0f, 0.0f),
          "alpha 0.5 -> box halfway between the samples");
    check(vecNear(hitbox::sampleOffset(prev, cur, 1.0f), 0.0f, 0.0f, 0.0f),
          "alpha 1 -> newest sample (raw tick box)");

    // The shifted box is exactly lerp(prev, cur, alpha), i.e. the position the
    // game draws the entity mesh at.
    for (float alpha = 0.0f; alpha <= 1.0f; alpha += 0.25f) {
        const Vec3 offset = hitbox::sampleOffset(prev, cur, alpha);
        const Vec3 shifted{cur.x + offset.x, cur.y + offset.y, cur.z + offset.z};
        const Vec3 expected = lerp(prev, cur, alpha);
        check(vecNear(shifted, expected.x, expected.y, expected.z),
              "shifted box equals lerp(prev, cur, alpha)");
    }

    check(vecNear(hitbox::sampleOffset(cur, cur, 0.3f), 0.0f, 0.0f, 0.0f),
          "no movement -> no offset");

    std::printf("hitbox sample history\n");

    {
        hitbox::Track track;
        check(!track.hasSample && !track.hasPrev, "fresh track has no samples");
        check(near(track.intervalSeconds, 0.05f), "fresh track assumes a 50 ms interval");

        check(hitbox::pushSample(track, prev, 0.0f), "first sample accepted");
        check(track.hasSample && !track.hasPrev, "first sample cannot interpolate yet");

        // Same position again: not a new sample, so the phase keeps running.
        check(!hitbox::pushSample(track, prev, 0.02f), "unchanged box is not a new sample");

        check(hitbox::pushSample(track, cur, 0.02f), "moved box is a new sample");
        check(track.hasPrev, "second sample enables interpolation");
        check(near(track.intervalSeconds, 0.02f), "short interval measured from the samples");
        check(vecNear(track.prevCenter, prev.x, prev.y, prev.z) &&
                  vecNear(track.curCenter, cur.x, cur.y, cur.z),
              "history holds the last two samples");

        // A hitch is not adopted as the sample interval.
        check(hitbox::pushSample(track, Vec3{10.5f, 64.0f, 10.0f}, 4.0f), "sample after a hitch accepted");
        check(near(track.intervalSeconds, 0.02f), "hitch interval rejected");

        // A teleport keeps prev == cur, so that step cannot be interpolated
        // across and the box stays on the newest sample.
        check(hitbox::pushSample(track, Vec3{80.0f, 64.0f, 10.0f}, 0.02f), "teleport sample accepted");
        check(!track.hasPrev, "teleport leaves nothing to interpolate");
        check(vecNear(track.curCenter, 80.0f, 64.0f, 10.0f), "newest sample is the teleport landing");

        check(!hitbox::pushSample(track, Vec3{std::nanf(""), 0.0f, 0.0f}, 0.02f),
              "non-finite sample rejected");
        check(vecNear(track.curCenter, 80.0f, 64.0f, 10.0f), "rejected sample leaves history intact");
    }

    // The reported bug: a player glides away (firework boost) while no new
    // sample is accepted, and the box must never stay on an old sample.
    {
        hitbox::Track track;
        hitbox::pushSample(track, Vec3{0.0f, 64.0f, 0.0f}, 0.0f);
        hitbox::pushSample(track, Vec3{1.0f, 64.0f, 0.0f}, 0.05f);

        const float stalledPhase = hitbox::sampleFraction(0.4f, track.intervalSeconds);
        const Vec3 offset = hitbox::sampleOffset(track.prevCenter, track.curCenter, stalledPhase);
        check(vecNear(offset, 0.0f, 0.0f, 0.0f),
              "stalled sampling leaves the box on the newest sample, never on the old one");
    }

    std::printf("hitbox sample sanity guard\n");

    check(hitbox::isPlausibleSampleJump(prev, cur), "walking sample accepted");
    check(hitbox::isPlausibleSampleJump(cur, Vec3{10.0f, 60.0f, 10.0f}),
          "terminal-velocity fall accepted");
    check(hitbox::isPlausibleSampleJump(cur, Vec3{10.0f + hitbox::kMaxSampleJump, 64.0f, 10.0f}),
          "boundary jump accepted");
    check(!hitbox::isPlausibleSampleJump(cur, Vec3{40.0f, 64.0f, 10.0f}), "teleport rejected");
    check(!hitbox::isPlausibleSampleJump(cur, Vec3{10.0f, -500.0f, 10.0f}), "bad read rejected");
    check(!hitbox::isPlausibleSampleJump(cur, Vec3{std::nanf(""), 64.0f, 10.0f}),
          "NaN sample rejected");
    check(!hitbox::isPlausibleSampleJump(Vec3{std::numeric_limits<float>::infinity(), 0.0f, 0.0f}, cur),
          "infinite sample rejected");

    check(hitbox::isFinite(Vec3{1.0f, 2.0f, 3.0f}), "finite value accepted");
    check(!hitbox::isFinite(Vec3{std::nanf(""), 2.0f, 3.0f}), "NaN value rejected");

    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all hitbox interpolation math checks passed\n");
    return 0;
}

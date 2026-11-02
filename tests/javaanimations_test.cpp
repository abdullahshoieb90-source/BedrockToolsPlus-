// Host tests for the Java Animations module's animation model.
//
// The module (src/modules/player/javaanimations.cpp) only ships wall-clock time
// into javaanimations_curve.hpp and pushes the resulting pose into the game's
// matrix stack; every curve, timer and easing lives in that dependency-free
// header, so the animation itself is testable without a device. These tests
// drive it frame by frame the way the renderer would and pin the properties
// that made the wings animation jitter before it was fixed: continuity between
// frames, a clean return to the rest pose, and no dependence on frame rate.
//
// Build: g++ -std=c++20 -I src tests/javaanimations_test.cpp -o /tmp/javaanimations_test
// Run:   /tmp/javaanimations_test

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "modules/player/javaanimations_curve.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++g_failures;
    }
}

bool near(float a, float b, float epsilon = 0.0001f) {
    return std::fabs(a - b) <= epsilon;
}

bool finitePose(const javaanim::HandPose& pose) {
    return std::isfinite(pose.posX) && std::isfinite(pose.posY) && std::isfinite(pose.posZ) &&
           std::isfinite(pose.rotX) && std::isfinite(pose.rotY) && std::isfinite(pose.rotZ) &&
           std::isfinite(pose.scale);
}

float maxRotationDelta(const javaanim::HandPose& a, const javaanim::HandPose& b) {
    float worst = 0.0f;
    worst = std::fmax(worst, std::fabs(a.rotX - b.rotX));
    worst = std::fmax(worst, std::fabs(a.rotY - b.rotY));
    worst = std::fmax(worst, std::fabs(a.rotZ - b.rotZ));
    return worst;
}

// One simulated frame at the given frame rate.
struct Frame {
    javaanim::HandPose pose;
};

std::vector<Frame> simulateSwing(float fps, int ticks, float swingScale = 1.0f,
                                 javaanim::SwingStyle style = javaanim::SwingStyle::Java17) {
    javaanim::Settings settings;
    settings.style = style;
    settings.swingScale = swingScale;
    javaanim::SwingState swing;
    javaanim::UseState use;

    swing.start(ticks, style);

    const float dt = 1.0f / fps;
    std::vector<Frame> frames;
    // Run well past the end of the swing so the return to rest is covered.
    const int count = static_cast<int>((ticks * javaanim::kTickSeconds + 0.5f) * fps) + 1;
    for (int i = 0; i < count; ++i) {
        frames.push_back({javaanim::currentPose(settings, swing, use, true)});
        swing.advance(dt);
        use.advance(dt, settings.useRaise);
    }
    return frames;
}

}  // namespace

int main() {
    std::printf("Java Animations - animation model\n");

    // --- the arc itself ---------------------------------------------------
    {
        check(javaanim::swingArc(0.0f, javaanim::SwingStyle::Java17) == 0.0f,
              "the 1.7 arc starts at zero (no pop on the first frame)");
        check(javaanim::swingArc(1.0f, javaanim::SwingStyle::Java17) == 0.0f,
              "the 1.7 arc ends at zero (no pop when the swing retires)");
        check(javaanim::swingArc(0.0f, javaanim::SwingStyle::Java18) == 0.0f &&
                  javaanim::swingArc(1.0f, javaanim::SwingStyle::Java18) == 0.0f,
              "the 1.8 arc is closed at both ends too");

        float peak17 = 0.0f;
        float peak17At = 0.0f;
        float peak18 = 0.0f;
        float peak18At = 0.0f;
        for (int i = 0; i <= 1000; ++i) {
            const float p = static_cast<float>(i) / 1000.0f;
            const float a17 = javaanim::swingArc(p, javaanim::SwingStyle::Java17);
            const float a18 = javaanim::swingArc(p, javaanim::SwingStyle::Java18);
            if (a17 > peak17) { peak17 = a17; peak17At = p; }
            if (a18 > peak18) { peak18 = a18; peak18At = p; }
        }
        check(peak17 > 0.99f && peak17 < 1.0001f, "the 1.7 arc peaks at full amplitude");
        check(peak18 > 0.99f && peak18 < 1.0001f, "the 1.8 arc peaks at full amplitude");
        check(peak17At < peak18At,
              "1.7 lifts faster than 1.8 (its peak lands earlier in the swing)");
        check(javaanim::swingArc(0.4f, javaanim::SwingStyle::Vanilla) == 0.0f,
              "the vanilla profile contributes no arc at all");
    }

    // --- one swing, frame by frame ----------------------------------------
    {
        const auto frames = simulateSwing(60.0f, 6);
        check(!frames.empty(), "a 6-tick swing produces frames at 60 fps");

        bool allFinite = true;
        float worstStep = 0.0f;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            if (!finitePose(frames[i].pose)) allFinite = false;
            if (i > 0) worstStep = std::fmax(worstStep, maxRotationDelta(frames[i - 1].pose, frames[i].pose));
        }
        check(allFinite, "every pose in a swing is finite");
        // A 1.7 swing is a fast chop: 72 degrees there and back inside six
        // ticks is ~15 degrees per frame at 60 fps by design. What matters is
        // that it never teleports, and that it leaves and re-enters the rest
        // pose at zero velocity.
        check(worstStep < 20.0f, "no frame rotates the hand by more than 20 degrees");
        // Zero velocity at both ends means the first and the last step are a
        // small fraction of the fastest step of the swing, not the same size.
        check(maxRotationDelta(frames[0].pose, frames[1].pose) < 0.6f * worstStep,
              "the swing eases in (the first step is far slower than the fastest one)");
        const std::size_t tail = frames.size() - 1;
        check(maxRotationDelta(frames[tail - 1].pose, frames[tail].pose) < 0.6f * worstStep,
              "the swing eases out (the last step is far slower than the fastest one)");
        check(worstStep > 4.0f, "the swing actually travels (it is not frozen at the rest pose)");

        const javaanim::HandPose& last = frames.back().pose;
        const javaanim::HandPose rest = javaanim::restPose(javaanim::Settings{});
        check(maxRotationDelta(last, rest) < 0.05f && std::fabs(last.posY - rest.posY) < 0.0005f,
              "the hand returns exactly to the rest pose when the swing is over");
    }

    // --- amplitude and scaling -------------------------------------------
    {
        javaanim::Settings settings;
        const float peak17 = javaanim::swingPose(settings, 0.35f, javaanim::SwingStyle::Java17).rotX;
        settings.style = javaanim::SwingStyle::Java18;
        const float peak18 = javaanim::swingPose(settings, 0.5f, javaanim::SwingStyle::Java18).rotX;
        check(peak17 > peak18, "the 1.7 swing is the bigger arc");
        check(peak17 > 60.0f && peak17 < 80.0f, "the 1.7 peak lift is in the intended range");

        javaanim::Settings zeroScale;
        zeroScale.swingScale = 0.0f;
        check(javaanim::isNeutral(javaanim::swingPose(zeroScale, 0.35f, javaanim::SwingStyle::Java17)),
              "a swing scale of zero mutes the swing but keeps the rest pose");

        javaanim::Settings huge;
        huge.swingScale = 1000.0f;
        const javaanim::HandPose clamped = javaanim::currentPose(huge, javaanim::SwingState{},
                                                                 javaanim::UseState{}, true);
        check(finitePose(clamped) && std::fabs(clamped.rotX) <= 180.0f,
              "an absurd swing scale is clamped instead of producing garbage");
    }

    // --- frame-rate independence -----------------------------------------
    {
        const auto at60 = simulateSwing(60.0f, 6);
        const auto at144 = simulateSwing(144.0f, 6);
        const float end60 = at60.back().pose.rotX;
        const float end144 = at144.back().pose.rotX;
        check(near(end60, end144, 0.05f), "the swing ends in the same pose at 60 and 144 fps");

        float peak60 = 0.0f;
        float peak144 = 0.0f;
        for (const auto& frame : at60) peak60 = std::fmax(peak60, frame.pose.rotX);
        for (const auto& frame : at144) peak144 = std::fmax(peak144, frame.pose.rotX);
        check(std::fabs(peak60 - peak144) < 3.0f, "the swing reaches the same height at any frame rate");
    }

    // --- swing timer ------------------------------------------------------
    {
        javaanim::SwingState swing;
        check(!swing.active && swing.progress() == 0.0f, "an idle swing reads zero progress");

        swing.start(6, javaanim::SwingStyle::Java17);
        check(swing.active, "starting a swing arms it");
        swing.advance(0.05f * 3.0f);
        check(swing.progress() > 0.4f && swing.progress() < 0.6f, "progress tracks the elapsed time");
        swing.advance(0.05f * 3.0f);
        check(!swing.active, "the swing retires once its duration has elapsed");
        check(swing.progress() == 0.0f, "a retired swing reads zero again");

        javaanim::SwingState negative;
        negative.start(6, javaanim::SwingStyle::Java17);
        negative.advance(-1.0f);
        check(negative.active && negative.progress() == 0.0f, "a backwards clock cannot move the swing");

        javaanim::SwingState clamped;
        clamped.start(1, javaanim::SwingStyle::Java17);
        check(clamped.ticks == javaanim::kMinSwingTicks, "a too-short duration is clamped up");
        clamped.start(9999, javaanim::SwingStyle::Java17);
        check(clamped.ticks == javaanim::kMaxSwingTicks, "a too-long duration is clamped down");
    }

    // --- use (eat / drink) ------------------------------------------------
    {
        javaanim::Settings settings;
        javaanim::UseState use;

        // The game reports a use every tick while the item is in use.
        float previous = use.raise;
        bool monotone = true;
        for (int i = 0; i < 30; ++i) {
            use.noteUse();
            use.advance(0.05f, true);
            if (use.raise < previous - 0.0001f) monotone = false;
            previous = use.raise;
        }
        check(monotone, "the raise only ever goes up while the item is in use");
        check(use.raise > 0.99f, "holding use lifts the arm all the way");

        const javaanim::HandPose up = javaanim::currentPose(settings, javaanim::SwingState{}, use, true);
        check(up.rotX > 20.0f, "the raise is a real lift towards the mouth, not a tilt");

        // Heartbeat stops: the arm comes back down.
        for (int i = 0; i < 40; ++i) use.advance(0.05f, true);
        check(use.raise == 0.0f, "the raise returns to zero once the use stops");
        const javaanim::HandPose down = javaanim::currentPose(settings, javaanim::SwingState{}, use, true);
        check(javaanim::isNeutral(javaanim::usePose(settings, use)) && near(down.rotX, javaanim::restPose(settings).rotX),
              "a released item leaves only the rest pose");

        // The raise is frame-rate independent.
        javaanim::UseState slow;
        javaanim::UseState fast;
        for (int i = 0; i < 10; ++i) { slow.noteUse(); slow.advance(1.0f / 30.0f, true); }
        for (int i = 0; i < 40; ++i) { fast.noteUse(); fast.advance(1.0f / 120.0f, true); }
        check(std::fabs(slow.raise - fast.raise) < 0.05f,
              "the raise reaches the same level at 30 and 120 fps");

        // A single use tick must not flicker: the raise is held for a few
        // missed heartbeats before it starts to fall.
        javaanim::UseState single;
        single.noteUse();
        single.advance(0.05f, true);
        single.advance(0.05f, true);
        check(single.raise > 0.5f, "one use tick still holds the arm up on the following frame");

        javaanim::Settings off = settings;
        off.useRaise = false;
        javaanim::UseState held;
        held.noteUse();
        held.advance(0.05f, false);
        check(javaanim::isNeutral(javaanim::usePose(off, held)), "the raise can be switched off");
    }

    // --- combined session continuity --------------------------------------
    {
        javaanim::Settings settings;
        javaanim::SwingState swing;
        javaanim::UseState use;
        swing.start(6, javaanim::SwingStyle::Java17);

        javaanim::HandPose previous = javaanim::currentPose(settings, swing, use, true);
        float worstStep = 0.0f;
        bool allFinite = finitePose(previous);
        for (int i = 0; i < 120; ++i) {
            if (i == 20) use.noteUse();
            if (i == 60) swing.start(6, javaanim::SwingStyle::Java17);  // a second attack mid-swing
            if (i < 40) use.noteUse();
            const javaanim::HandPose pose = javaanim::currentPose(settings, swing, use, true);
            worstStep = std::fmax(worstStep, maxRotationDelta(previous, pose));
            if (!finitePose(pose)) allFinite = false;
            previous = pose;
            swing.advance(1.0f / 60.0f);
            use.advance(1.0f / 60.0f, true);
        }
        check(allFinite, "a mixed swing + eat session stays finite");
        check(worstStep < 25.0f, "re-swinging while eating never teleports the hand");

        // The frame where a second attack arrives is the one that used to snap.
        javaanim::SwingState again;
        javaanim::UseState eating;
        again.start(6, javaanim::SwingStyle::Java17);
        for (int i = 0; i < 6; ++i) again.advance(1.0f / 60.0f);
        const javaanim::HandPose before = javaanim::currentPose(settings, again, eating, true);
        again.start(6, javaanim::SwingStyle::Java17);
        const javaanim::HandPose after = javaanim::currentPose(settings, again, eating, true);
        check(maxRotationDelta(before, after) < 1.0f,
              "re-arming a swing keeps the height it is already at (no snap on attack spam)");
    }

    // --- offhand mirroring -------------------------------------------------
    {
        javaanim::Settings settings;
        javaanim::SwingState swing;
        swing.start(6, javaanim::SwingStyle::Java17);
        swing.advance(0.1f);
        javaanim::UseState use;
        use.noteUse();
        use.advance(0.05f, true);

        const javaanim::HandPose main = javaanim::currentPose(settings, swing, use, true);
        const javaanim::HandPose off = javaanim::currentPose(settings, swing, use, false);
        check(near(off.posX, -main.posX) && near(off.rotY, -main.rotY) && near(off.rotZ, -main.rotZ),
              "the offhand pose is the main-hand pose mirrored");
        check(near(off.posY, main.posY) && near(off.rotX, main.rotX),
              "mirroring leaves the vertical pose untouched");
    }

    // --- vanilla / disabled -------------------------------------------------
    {
        javaanim::Settings vanilla;
        vanilla.style = javaanim::SwingStyle::Vanilla;
        javaanim::SwingState swing;
        swing.start(6, javaanim::SwingStyle::Java17);
        swing.advance(0.1f);
        javaanim::UseState use;
        use.noteUse();
        use.advance(0.05f, true);
        check(javaanim::isNeutral(javaanim::currentPose(vanilla, swing, use, true)),
              "the vanilla profile leaves the game's own animation alone");

        javaanim::Settings bare;
        bare.itemPose = false;
        check(javaanim::isNeutral(javaanim::restPose(bare)), "the rest stance can be switched off");
    }

    // --- sanitizing ---------------------------------------------------------
    {
        javaanim::HandPose poison;
        poison.posX = std::nanf("");
        poison.posY = 1.0e9f;
        poison.posZ = -1.0e9f;
        poison.rotX = std::nanf("");
        poison.rotY = 1.0e9f;
        poison.rotZ = -1.0e9f;
        poison.scale = 0.0f;
        const javaanim::HandPose clean = javaanim::sanitizePose(poison);
        check(finitePose(clean), "a poisoned pose is sanitized to finite values");
        check(clean.rotY <= 180.0f && clean.rotZ >= -180.0f, "sanitized rotations stay in range");
        check(clean.scale >= 0.05f && clean.scale <= 4.0f, "sanitized scale stays in range");
    }

    // --- config round-trip ---------------------------------------------------
    {
        for (int i = 0; i < javaanim::kSwingStyleCount; ++i) {
            const std::string value = javaanim::styleRadioValue(i);
            check(javaanim::resolveStyleIndex(value) == i, "a serialized style round-trips");
        }
        check(javaanim::resolveStyleIndex("2") == 2, "a bare index from the launcher is accepted");
        check(javaanim::resolveStyleIndex("Java 1.8") == 1, "a bare style id is accepted");
        check(javaanim::resolveStyleIndex("") == 0, "an empty value falls back to Java 1.7");
        check(javaanim::resolveStyleIndex("nonsense") == 0, "an unknown value falls back to Java 1.7");
        check(javaanim::resolveStyleIndex("99") == 0, "an out-of-range index falls back to Java 1.7");
        check(javaanim::styleForIndex(0) == javaanim::SwingStyle::Java17 &&
                  javaanim::styleForIndex(1) == javaanim::SwingStyle::Java18 &&
                  javaanim::styleForIndex(2) == javaanim::SwingStyle::Vanilla,
              "style indices map to the documented profiles");
    }

    std::printf(g_failures == 0 ? "ALL TESTS PASSED\n" : "%d TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

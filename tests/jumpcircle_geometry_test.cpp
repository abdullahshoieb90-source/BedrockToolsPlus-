#include "modules/visual/jumpcircle_geometry.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

int main() {
    using namespace jumpcircle;
    MotionTracker m;
    assert(!m.update(0).takeoff);
    auto e = m.update(.42f); // also works without a jump-button input
    assert(e.takeoff && !e.landing);
    assert(!m.update(.34f).takeoff);
    assert(!m.update(.01f).landing); // apex is not a landing
    assert(!m.update(-.08f).landing);
    e = m.update(0);
    assert(e.landing && !e.takeoff);
    assert(!m.update(0).landing); // exactly once
    e = m.update(.42f); assert(e.takeoff);
    m.update(-.08f);
    e = m.update(.42f); // jump immediately on landing
    assert(e.landing && e.takeoff);
    m.update(-.1f);
    e = m.update(10); assert(!e.landing && !e.takeoff && !m.airborne);
    assert(!m.update(.5f).takeoff); // ordinary half-block step
    assert(!m.update(1.0f).takeoff);
    // Walking off a block does not need a preceding jump.
    MotionTracker falling;
    assert(!falling.update(0).landing);
    assert(!falling.update(-.4f).landing);
    assert(falling.update(0).landing);
    assert(!falling.update(0).landing);
    assert(!falling.update(-.15f).landing);
    auto bounce = falling.update(.42f);
    assert(bounce.landing && bounce.takeoff);
    assert(styleIndex("2,Circle,Square,Star") == 2);
    assert(styleIndex("Square") == 1 && styleIndex("nonsense") == 0);
    assert(styleRadioValue(2) == "2,Circle,Square,Star");
    auto start = animate(0, defaultDuration, false);
    auto middle = animate(defaultDuration * .5f, defaultDuration, false);
    auto end = animate(defaultDuration, defaultDuration, false);
    assert(start.radius < middle.radius && middle.radius < end.radius);
    assert(start.alpha > middle.alpha && middle.alpha > end.alpha && end.alpha == 0);
    assert(animate(1, 2, false).alpha > 0 && animate(1, .5f, false).alpha == 0);
    auto persistent = animate(20, 2, true);
    assert(persistent.alpha == 1 && persistent.radius == end.radius);
    assert(safeDuration(-10) == minDuration && safeDuration(100) == maxDuration);
    for (int style = 0; style < styleCount; ++style) {
        int count = 0;
        bool bright = false, transparent = false;
        emitRing({2, 3, 4}, .1f, defaultDuration, false, static_cast<Style>(style), [&](Point p, float alpha) {
            assert(std::isfinite(p.x) && p.y == 3 && std::isfinite(p.z));
            assert(alpha >= 0 && alpha <= 1);
            bright |= alpha > .1f; transparent |= alpha == 0;
            ++count;
        });
        assert(count == segments * bands * 4 && bright && transparent);
        auto a = contour(static_cast<Style>(style), 0), b = contour(static_cast<Style>(style), segments);
        assert(std::abs(a.x - b.x) < .00001f && std::abs(a.z - b.z) < .00001f);
    }
    auto starTip = contour(Style::Star, 0), starValley = contour(Style::Star, segments / 10);
    assert(std::hypot(starTip.x, starTip.z) > std::hypot(starValley.x, starValley.z) * 2);
    std::puts("jumpcircle motion and geometry: OK");
}

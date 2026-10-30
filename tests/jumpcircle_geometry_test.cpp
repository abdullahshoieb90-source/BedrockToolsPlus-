#include "modules/visual/jumpcircle_geometry.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

int main() {
    using namespace jumpcircle;
    assert(takeoff(0, .42f, true));
    assert(takeoff(-.1f, .42f, true)); // continuous jumping after landing
    assert(!takeoff(0, .42f, false));
    assert(!takeoff(.42f, .34f, true)); // one effect per ascent
    assert(!takeoff(0, 0, true));
    assert(!takeoff(0, 5, true)); // teleport
    auto start = animate(0), middle = animate(duration * .5f), end = animate(duration);
    assert(start.radius < middle.radius && middle.radius < end.radius);
    assert(start.alpha > middle.alpha && middle.alpha > end.alpha && end.alpha == 0);
    int count = 0;
    bool bright = false, transparent = false;
    emitRing({2, 3, 4}, .1f, [&](Point p, float alpha) {
        assert(std::isfinite(p.x) && p.y == 3 && std::isfinite(p.z));
        assert(alpha >= 0 && alpha <= 1);
        bright |= alpha > .1f; transparent |= alpha == 0;
        ++count;
    });
    assert(count == segments * bands * 4 && bright && transparent);
    std::puts("jumpcircle geometry: OK");
}

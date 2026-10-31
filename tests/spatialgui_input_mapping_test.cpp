// Host test for screen-space touch to world-space panel mapping.
#include "modules/spatialgui/input_mapping.hpp"
#include <cmath>
#include <cstdio>
#include <limits>

using namespace bedrocktools::spatialgui;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* description) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", description);
        failures += !ok;
    };
    auto near = [](float a, float b) { return std::fabs(a-b) < 0.0001f; };
    // 90-degree OpenGL perspective, aspect 1, near=1, far=10.
    Camera camera{{1,0,0,0, 0,1,0,0, 0,0,0,-.45f, 0,0,-1,.55f}, 600, 600};
    const auto panel = makePanel({0,0,-3}, {0,0,0}, 2, 2);
    if (!panel) return 1;
    auto ray = screenRay(camera, {300,300});
    check(ray && near(ray->origin.z,-1) && near(ray->direction.z,-1), "center near-plane ray");
    auto hit = mapTouch(camera, *panel, {300,300}, {200,100});
    check(hit && near(hit->x,100) && near(hit->y,50), "center maps to UI center");
    hit = mapTouch(camera, *panel, {200,200}, {200,100});
    check(hit && near(hit->x,0) && near(hit->y,0), "top-left maps with y down");
    hit = mapTouch(camera, *panel, {400,400}, {200,100});
    check(hit && near(hit->x,200) && near(hit->y,100), "bottom-right edge maps to UI bounds");
    check(!mapTouch(camera, *panel, {450,300}, {200,100}), "touch outside panel rejected");
    check(!screenRay(camera, {-1,300}), "touch outside viewport rejected");
    check(!mapTouch(camera, *panel, {300,300}, {0,100}), "invalid UI size rejected");
    auto invalid = camera;
    invalid.viewportHeight = 0;
    check(!screenRay(invalid, {0,0}), "invalid viewport rejected");
    invalid = camera;
    invalid.inverseViewProjection[15] = std::numeric_limits<float>::quiet_NaN();
    check(!screenRay(invalid, {300,300}), "non-finite matrix rejected");
    invalid = camera;
    invalid.inverseViewProjection.fill(0);
    check(!screenRay(invalid, {300,300}), "zero homogeneous w rejected");
    return failures ? 1 : 0;
}

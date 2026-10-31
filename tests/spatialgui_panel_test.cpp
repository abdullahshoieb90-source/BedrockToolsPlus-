// Host test: g++ -std=c++20 -I include -I src tests/spatialgui_panel_test.cpp -o /tmp/spatialgui_panel_test
#include "modules/spatialgui/panel_geometry.hpp"
#include <cmath>
#include <cstdio>
#include <limits>

using namespace bedrocktools::spatialgui;
int main() {
    int failures = 0;
    auto check = [&](bool condition, const char* message) {
        std::printf("%s %s\n", condition ? "ok" : "FAIL", message);
        failures += !condition;
    };
    auto near = [](float a, float b) { return std::fabs(a - b) < 0.0001f; };
    const auto p = makePanel({0, 1, 0}, {0, 1, 5}, 4, 2);
    check(p.has_value(), "valid panel");
    if (!p) return 1;
    const auto c = corners(*p);
    check(near(c[0].x, -2) && near(c[0].y, 2) && near(c[2].x, 2) && near(c[2].y, 0), "corners and top-left orientation");
    auto hit = hitTest(*p, {0, 1, 5}, {0, 0, -10});
    check(hit && near(hit->x, .5f) && near(hit->y, .5f), "center hit with non-unit ray");
    hit = hitTest(*p, {-2, 2, 5}, {0, 0, -1});
    check(hit && near(hit->x, 0) && near(hit->y, 0), "top-left edge included");
    hit = hitTest(*p, {2, 0, 5}, {0, 0, -1});
    check(hit && near(hit->x, 1) && near(hit->y, 1), "bottom-right edge included");
    check(!hitTest(*p, {2.01f, 1, 5}, {0, 0, -1}), "outside rejected");
    check(!hitTest(*p, {0, 1, 5}, {1, 0, 0}), "parallel rejected");
    check(!hitTest(*p, {0, 1, 5}, {0, 0, 1}), "ray pointing away rejected");
    check(!hitTest(*p, {0, 1, -5}, {0, 0, 1}), "back face rejected");
    check(!hitTest(*p, {0, 1, 5}, {0, 0, 0}), "zero ray rejected");
    const auto rotated = makePanel({2, 3, 4}, {7, 3, 4}, 2, 2);
    check(rotated && hitTest(*rotated, {7, 3, 4}, {-1, 0, 0}) &&
          near(hitTest(*rotated, {7, 3, 4}, {-1, 0, 0})->x, .5f), "rotated panel hit");
    check(!makePanel({0,0,0}, {0,1,0}, 2, 2), "vertical facing rejected");
    check(!makePanel({0,0,0}, {0,0,1}, 0, 2), "zero width rejected");
    check(!makePanel({0,0,0}, {0,0,1}, std::numeric_limits<float>::infinity(), 2), "non-finite width rejected");
    check(!hitTest(*p, {0, 1, 5}, {NAN, 0, -1}), "non-finite ray rejected");
    return failures ? 1 : 0;
}

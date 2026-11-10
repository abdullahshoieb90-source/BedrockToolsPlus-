#include "modules/visual/cape_shape.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
namespace cape = bedrocktools::modules::cape;

int main() {
    std::vector<unsigned char> image(2 * 3 * 4, 255);
    image[0] = 230; image[1] = 10; image[2] = 20;
    image[4 + 3] = 0;
    auto quads = cape::build(image.data(), 2, 3);
    assert(quads.size() == 20); // five cells, two faces plus ten boundary edges
    assert(quads[0].backC[0].r == 230 && quads[0].backC[0].g == 10);
    assert(quads[0].p[0].y == 24 && quads[0].p[0].z == cape::kFront + cape::kDepth);
    assert(quads[1].p[0].z == cape::kFront);
    assert(cape::build(nullptr, 2, 3).empty());
    assert(cape::build(image.data(), 2049, 1).empty());

    std::vector<unsigned char> atlas(64 * 32 * 4, 0);
    for (int y = 1; y < 17; ++y) for (int x = 1; x < 11; ++x) {
        auto i = (y * 64 + x) * 4;
        atlas[i] = 27; atlas[i+3] = 255;
    }
    quads = cape::build(atlas.data(), 64, 32);
    assert(quads.size() == 320 + 2 * (10 + 16) && quads[0].backC[0].r == 27);
    std::vector<unsigned char> large(128 * 256 * 4, 255);
    assert(cape::build(large.data(), 128, 256).size() == 32 * 48 * 2 + 2 * (32 + 48));

    // A rigid panel pivots at the shoulders: no stretching, and its lower
    // edge swings outward instead of sticking to the torso while moving.
    auto top = cape::transformPoint({0, 24, cape::kFront}, {35, 0});
    auto bottom = cape::transformPoint({0, 8, cape::kFront}, {35, 0});
    assert(top.y == 24 && top.z == cape::kFront);
    assert(bottom.z > cape::kFront + 8.0f);
    assert(std::abs(std::hypot(24 - bottom.y, bottom.z - cape::kFront) - 16.0f) < 0.001f);

    cape::Motion m;
    m.update(0, 0, 0, 0, 0.05f);
    for (int i=1; i<=20; ++i) m.update(0, 0, i * 0.2f, 0, 0.05f);
    assert(m.current.pitch > 20 && m.current.pitch < 48);
    auto moving = m.current.pitch;
    for (int i=0; i<40; ++i) m.update(0, 0, 4, 0, 0.05f);
    assert(m.current.pitch < moving && std::abs(m.current.pitch - 6) < 0.1f);
    m.update(100, 0, 0, 0, 0.05f); // teleport resets rather than flinging cape
    assert(m.current.pitch == 6 && m.current.roll == 0);
    m.update(100, 0, 0, 359, 0.05f);
    m.update(100, 0, 0, 1, 0.05f);
    assert(std::abs(m.current.roll) < 1.0f); // shortest arc over 360 degrees
    auto mid = cape::interpolate({6, 0}, {26, 8}, 0.5f);
    assert(mid.pitch == 16 && mid.roll == 4);
    std::puts("cape geometry and motion tests passed");
}

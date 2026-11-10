#include "modules/visual/cape_shape.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
using bedrocktools::modules::cape::build;

int main() {
    std::vector<unsigned char> image(2 * 3 * 4, 255);
    image[0] = 230; image[1] = 10; image[2] = 20;
    image[4 + 3] = 0;
    auto quads = build(image.data(), 2, 3);
    assert(quads.size() == 5);
    assert(quads[0].backC[0].r == 230 && quads[0].backC[0].g == 10);
    assert(quads[0].p[0].y == 24 && quads[0].p[0].z > 2);
    assert(quads.back().p[2].y == 8);
    assert(build(nullptr, 2, 3).empty());
    assert(build(image.data(), 2049, 1).empty());

    // In a standard cape atlas only the 10x16 back face is sampled.
    std::vector<unsigned char> atlas(64 * 32 * 4, 0);
    for (int y = 1; y < 17; ++y) for (int x = 1; x < 11; ++x) {
        auto i = (y * 64 + x) * 4;
        atlas[i] = 27; atlas[i+3] = 255;
    }
    quads = build(atlas.data(), 64, 32);
    assert(quads.size() == 160 && quads[0].backC[0].r == 27);
    std::vector<unsigned char> large(128 * 256 * 4, 255);
    assert(build(large.data(), 128, 256).size() == 32 * 48);
    std::puts("cape shape tests passed");
}

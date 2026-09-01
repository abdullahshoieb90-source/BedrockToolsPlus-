// Unit tests for the Cape Physics pure simulation and palette sampler
// (src/modules/visual/capephysics_sim.hpp).
//
// Covered:
//   * cloth configuration, reset and anchoring (top row pinned exactly)
//   * gravity settles the cape below the shoulder line, above the ground
//   * the body collision keeps every free particle outside the elliptic
//     cross-section around the player axis
//   * structural constraints hold: the sheet stays cape-shaped and no edge
//     ever exceeds the hard stretch clamp (no explosion)
//   * determinism: identical inputs produce identical cloth states
//   * stability: max dt + max wind for hundreds of steps stays finite and
//     bounded
//   * teleport: a > 3 block anchor jump re-hangs the cape instead of
//     slinging it across the world
//   * running: moving anchors drag the cloth along (it trails behind)
//   * palette sampling on a 64x32 canvas with the native 10x16 grid is
//     pixel-identical to the cape texture (outer, lining and edge strips)
//   * every source size works: 22x23 and 704x736 sources resample through
//     the Custom Capes pipeline into a full palette (the module's "any size"
//     support end-to-end), and normalizeCapeCanvas maps proportional layouts
//   * detail preset radio round trip
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src -I include -I third_party
//         tests/capephysics_sim_test.cpp -o /tmp/capephysics_sim_test
//     /tmp/capephysics_sim_test

#include "modules/visual/capephysics_sim.hpp"
#include "modules/player/customcapes_files.hpp"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace cp = bedrocktools::modules::capephysics;
namespace ccc = customcapes;

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

bool approxEqual(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) <= eps;
}

// A deterministic test cape canvas: every pixel gets a unique color derived
// from its coordinates so any sampling mistake is immediately visible.
std::vector<std::uint8_t> uniqueColorCanvas() {
    std::vector<std::uint8_t> canvas(cp::kCanvasBytes, 0);
    for (std::uint32_t y = 0; y < cp::kCanvasHeight; ++y) {
        for (std::uint32_t x = 0; x < cp::kCanvasWidth; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * cp::kCanvasWidth + x) * 4u;
            canvas[i + 0] = static_cast<std::uint8_t>(x * 4); // 0..252
            canvas[i + 1] = static_cast<std::uint8_t>(y * 8); // 0..248
            canvas[i + 2] = static_cast<std::uint8_t>(255 - x * 2);
            canvas[i + 3] = 255;
        }
    }
    return canvas;
}

const std::uint8_t* canvasPixel(const std::vector<std::uint8_t>& canvas, std::uint32_t x, std::uint32_t y) {
    return canvas.data() + (static_cast<std::size_t>(y) * cp::kCanvasWidth + static_cast<std::size_t>(x)) * 4u;
}

cp::Vec3 anchorsForBody(const cp::BodyFrame& body, int cols) {
    // helper: returns anchors[0] only (left edge); tests build full arrays
    // with cp::buildAnchors.
    cp::Vec3 anchors[64];
    cp::buildAnchors(body, cols, anchors);
    return anchors[0];
}

} // namespace

int main() {
    std::printf("cape physics - cloth simulation\n");

    // ------------------------------------------------------------------
    // Configuration, reset, anchoring
    // ------------------------------------------------------------------
    std::printf("configure and reset\n");
    {
        cp::Cloth cloth;
        cloth.configure(10, 16);
        check(cloth.cols() == 10 && cloth.rows() == 16, "native grid is 10x16");

        cp::BodyFrame body{{0.0f, 64.0f, 0.0f}, 0.0f, 1.8f};
        cp::Vec3 anchors[64];
        cp::buildAnchors(body, 10, anchors);

        // Shoulder line geometry: 11 anchors across the cape width at the
        // shoulder height, behind the torso.
        const cp::Vec3& first = anchors[0];
        const cp::Vec3& last = anchors[10];
        check(approxEqual(first.y, 64.0f + cp::kCapeTopModelY / 16.0f), "anchor row at shoulder height");
        const float span = cp::length(last - first);
        check(approxEqual(span, cp::kCapeWidthBlocks, 1e-2f), "anchor row spans the 10px cape width");

        cloth.reset(anchors, body);
        check(cloth.settled(), "cloth settled after reset");
        for (int i = 0; i <= 10; ++i) {
            check(approxEqual(cloth.at(i, 0).x, anchors[i].x) &&
                      approxEqual(cloth.at(i, 0).y, anchors[i].y) &&
                      approxEqual(cloth.at(i, 0).z, anchors[i].z),
                  "top row pinned to the shoulder line");
        }
        const float drop = cloth.at(5, 16).y - cloth.at(5, 0).y;
        check(approxEqual(drop, -cp::kCapeHeightBlocks, 0.05f), "hem hangs a full cape height below the shoulders");

        cp::ClothParams params; // defaults: gravity 1, wind 0.35, stiffness 0.85
        cloth.step(1.0f / 60.0f, anchors, body, params, 0.0f);
        for (int i = 0; i <= 10; ++i) {
            check(approxEqual(cloth.at(i, 0).x, anchors[i].x, 1e-4f) &&
                      approxEqual(cloth.at(i, 0).y, anchors[i].y, 1e-4f) &&
                      approxEqual(cloth.at(i, 0).z, anchors[i].z, 1e-4f),
                  "top row still pinned after a step");
        }
        (void)anchorsForBody;
    }

    // ------------------------------------------------------------------
    // Gravity settling + collision + constraint health
    // ------------------------------------------------------------------
    std::printf("gravity, collision and constraints\n");
    {
        cp::Cloth cloth;
        cloth.configure(10, 16);
        cp::BodyFrame body{{100.0f, 64.0f, 100.0f}, 30.0f, 1.8f};
        cp::Vec3 anchors[64];
        cp::buildAnchors(body, 10, anchors);
        cloth.reset(anchors, body);

        cp::ClothParams params;
        params.windMul = 0.0f; // gravity only
        for (int s = 0; s < 240; ++s) { // 4 seconds
            cloth.step(1.0f / 60.0f, anchors, body, params, static_cast<float>(s) / 60.0f);
        }

        bool allFinite = true;
        for (int j = 0; j <= 16; ++j) {
            for (int i = 0; i <= 10; ++i) {
                if (!cp::isFinite(cloth.at(i, j))) allFinite = false;
            }
        }
        check(allFinite, "all particles finite after 4 s of gravity");

        // The hem must sit below the shoulders and above the feet.
        const float hemY = cloth.at(5, 16).y;
        check(hemY < cloth.at(5, 0).y - 0.5f, "hem below the shoulder line");
        check(hemY > body.feet.y, "hem above the feet");

        // Every free particle inside the body's height range must be
        // outside the elliptic cross-section.
        const float yawRad = body.yawDeg * 3.14159265358979f / 180.0f;
        const float rightX = -std::cos(yawRad), rightZ = -std::sin(yawRad);
        const float fwdX = -std::sin(yawRad), fwdZ = std::cos(yawRad);
        bool allOutside = true;
        for (int j = 1; j <= 16; ++j) {
            for (int i = 0; i <= 10; ++i) {
                const cp::Vec3& p = cloth.at(i, j);
                const float dy = p.y - body.feet.y;
                if (dy < -0.10f || dy > body.height + 0.25f) continue;
                const float dx = p.x - body.feet.x;
                const float dz = p.z - body.feet.z;
                const float lx = dx * rightX + dz * rightZ;
                const float lz = dx * fwdX + dz * fwdZ;
                const float e = (lx / cp::kBodyRadiusX) * (lx / cp::kBodyRadiusX) +
                                (lz / cp::kBodyRadiusZ) * (lz / cp::kBodyRadiusZ);
                if (e < 0.81f) allOutside = false; // 0.9^2 tolerance
            }
        }
        check(allOutside, "no particle inside the body's elliptic cross-section");

        check(cloth.constraintError() < 3.0f, "structural constraints stay cape-shaped");

        // The hard stretch clamp must hold on every structural edge.
        const float restW = cp::kCapeWidthBlocks / 10.0f;
        const float restH = cp::kCapeHeightBlocks / 16.0f;
        bool bounded = true;
        for (int j = 0; j <= 16; ++j) {
            for (int i = 0; i < 10; ++i) {
                if (cp::length(cloth.at(i, j) - cloth.at(i + 1, j)) > restW * cp::kMaxStretch * 1.05f) bounded = false;
            }
        }
        for (int j = 0; j < 16; ++j) {
            for (int i = 0; i <= 10; ++i) {
                if (cp::length(cloth.at(i, j) - cloth.at(i, j + 1)) > restH * cp::kMaxStretch * 1.05f) bounded = false;
            }
        }
        check(bounded, "no structural edge exceeds the stretch clamp");
    }

    // ------------------------------------------------------------------
    // Determinism
    // ------------------------------------------------------------------
    std::printf("determinism\n");
    {
        cp::BodyFrame body{{0.0f, 32.0f, 0.0f}, -140.0f, 1.8f};
        cp::Vec3 anchors[64];
        cp::buildAnchors(body, 10, anchors);
        cp::ClothParams params;
        params.windMul = 1.7f; // strong wind so any divergence grows fast

        cp::Cloth a, b;
        a.configure(10, 16);
        b.configure(10, 16);
        a.reset(anchors, body);
        b.reset(anchors, body);
        for (int s = 0; s < 120; ++s) {
            const float t = static_cast<float>(s) / 60.0f;
            a.step(1.0f / 60.0f, anchors, body, params, t);
            b.step(1.0f / 60.0f, anchors, body, params, t);
        }
        bool identical = true;
        for (int j = 0; j <= 16 && identical; ++j) {
            for (int i = 0; i <= 10 && identical; ++i) {
                identical = approxEqual(a.at(i, j).x, b.at(i, j).x, 1e-6f) &&
                            approxEqual(a.at(i, j).y, b.at(i, j).y, 1e-6f) &&
                            approxEqual(a.at(i, j).z, b.at(i, j).z, 1e-6f);
            }
        }
        check(identical, "identical inputs give identical cloth states");
    }

    // ------------------------------------------------------------------
    // Stability at the dt / wind limits
    // ------------------------------------------------------------------
    std::printf("stability at the limits\n");
    {
        cp::Cloth cloth;
        cloth.configure(14, 22); // fine grid is the largest configuration
        cp::BodyFrame body{{-50.0f, 70.0f, 12.0f}, 95.0f, 1.8f};
        cp::Vec3 anchors[64];
        cp::buildAnchors(body, 14, anchors);
        cloth.reset(anchors, body);

        cp::ClothParams params;
        params.windMul = 2.0f;  // maximum wind
        params.gravityMul = 2.0f;
        params.stiffness = 0.05f; // floppiest
        for (int s = 0; s < 600; ++s) { // 10 s at max dt
            cloth.step(1.0f / 20.0f, anchors, body, params, static_cast<float>(s) / 20.0f);
        }

        bool allFinite = true;
        bool bounded = true;
        for (int j = 0; j <= 22; ++j) {
            for (int i = 0; i <= 14; ++i) {
                const cp::Vec3& p = cloth.at(i, j);
                if (!cp::isFinite(p)) allFinite = false;
                if (cp::length(p - anchors[0]) > 12.0f) bounded = false;
            }
        }
        check(allFinite, "max dt + max wind keeps every particle finite");
        check(bounded, "max dt + max wind keeps the cape near the player");
    }

    // ------------------------------------------------------------------
    // Teleport re-hangs the cape
    // ------------------------------------------------------------------
    std::printf("teleport\n");
    {
        cp::Cloth cloth;
        cloth.configure(10, 16);
        cp::BodyFrame body{{0.0f, 64.0f, 0.0f}, 0.0f, 1.8f};
        cp::Vec3 anchors[64];
        cp::buildAnchors(body, 10, anchors);
        cloth.reset(anchors, body);

        // Swing it around first so it has momentum.
        cp::ClothParams params;
        for (int s = 0; s < 60; ++s) {
            cp::BodyFrame moving{{0.02f * s, 64.0f, 0.0f}, 2.0f * s, 1.8f};
            cp::buildAnchors(moving, 10, anchors);
            cloth.step(1.0f / 60.0f, anchors, moving, params, static_cast<float>(s) / 60.0f);
        }

        // Teleport 200 blocks away.
        cp::BodyFrame farAway{{200.0f, 64.0f, -300.0f}, 0.0f, 1.8f};
        cp::buildAnchors(farAway, 10, anchors);
        cloth.step(1.0f / 60.0f, anchors, farAway, params, 1.0f);

        const cp::Vec3& hem = cloth.at(5, 16);
        check(std::fabs(hem.x - farAway.feet.x) < 1.0f && std::fabs(hem.z - farAway.feet.z) < 1.0f,
              "cape re-hangs at the destination instead of slinging across");
        check(hem.y < farAway.feet.y + cp::kCapeTopModelY / 16.0f,
              "re-hung cape hangs below the shoulders");
    }

    // ------------------------------------------------------------------
    // Running drags the cape along (it trails behind the movement)
    // ------------------------------------------------------------------
    std::printf("running\n");
    {
        cp::Cloth cloth;
        cloth.configure(10, 16);
        cp::ClothParams params;
        params.windMul = 0.0f;

        // Player runs along +x at 4.3 blocks/s (sprint) for 2 s.
        cp::BodyFrame body{{0.0f, 64.0f, 0.0f}, 90.0f, 1.8f};
        cp::Vec3 anchors[64];
        cp::buildAnchors(body, 10, anchors);
        cloth.reset(anchors, body);

        const float speed = 4.3f;
        for (int s = 0; s < 120; ++s) {
            body.feet.x = speed * static_cast<float>(s) / 60.0f;
            cp::buildAnchors(body, 10, anchors);
            cloth.step(1.0f / 60.0f, anchors, body, params, static_cast<float>(s) / 60.0f);
        }

        // The cloth moved with the player but lags behind the anchor row.
        cp::Vec3 clothCenter{0, 0, 0};
        for (int j = 0; j <= 16; ++j)
            for (int i = 0; i <= 10; ++i) clothCenter += cloth.at(i, j);
        clothCenter = clothCenter * (1.0f / (17.0f * 11.0f));
        check(clothCenter.x > body.feet.x - 2.0f, "cape follows the runner");
        check(clothCenter.x < anchors[5].x, "cape trails behind the shoulders");

        // And it lifts: the hem is further back than the shoulders relative
        // to the movement direction.
        const float hemLag = anchors[5].x - cloth.at(5, 16).x;
        const float midLag = anchors[5].x - cloth.at(5, 8).x;
        check(hemLag > midLag, "hem streams further back than the middle of the cape");
    }

    // ------------------------------------------------------------------
    std::printf("cape physics - palette sampling\n");
    // ------------------------------------------------------------------

    // Native grid on a 64x32 canvas: pixel-identical mapping.
    std::printf("native palette is pixel-identical\n");
    {
        const std::vector<std::uint8_t> canvas = uniqueColorCanvas();
        cp::CapePalette palette;
        cp::buildCapePalette(canvas.data(), cp::kCanvasWidth, cp::kCanvasHeight, 10, 16, palette);

        check(palette.outer.size() == 10 * 16 * 3, "outer palette has one entry per cape pixel");
        bool outerOk = true, innerOk = true, leftOk = true, rightOk = true, topOk = true, bottomOk = true;
        for (int j = 0; j < 16; ++j) {
            for (int i = 0; i < 10; ++i) {
                const std::size_t cell = (static_cast<std::size_t>(j) * 10 + i) * 3u;
                const std::uint8_t* back = canvasPixel(canvas, 1 + i, 1 + j);
                const std::uint8_t* front = canvasPixel(canvas, 12 + i, 1 + j);
                for (int c = 0; c < 3; ++c) {
                    if (palette.outer[cell + c] != back[c]) outerOk = false;
                    if (palette.inner[cell + c] != front[c]) innerOk = false;
                }
            }
            const std::uint8_t* left = canvasPixel(canvas, 0, 1 + j);
            const std::uint8_t* right = canvasPixel(canvas, 11, 1 + j);
            const std::size_t er = static_cast<std::size_t>(j) * 3u;
            for (int c = 0; c < 3; ++c) {
                if (palette.edgeLeft[er + c] != left[c]) leftOk = false;
                if (palette.edgeRight[er + c] != right[c]) rightOk = false;
            }
        }
        for (int i = 0; i < 10; ++i) {
            const std::uint8_t* top = canvasPixel(canvas, 1 + i, 0);
            const std::uint8_t* bottom = canvasPixel(canvas, 11 + i, 0);
            const std::size_t ec = static_cast<std::size_t>(i) * 3u;
            for (int c = 0; c < 3; ++c) {
                if (palette.edgeTop[ec + c] != top[c]) topOk = false;
                if (palette.edgeBottom[ec + c] != bottom[c]) bottomOk = false;
            }
        }
        check(outerOk, "outer face maps the back region pixel-for-pixel");
        check(innerOk, "lining face maps the front region pixel-for-pixel");
        check(leftOk && rightOk, "side strips map the 1px edge columns");
        check(topOk && bottomOk, "top/bottom strips map the edge rows");
    }

    // Fine grid colors stay within the range of the texels they cover.
    std::printf("fine grid averages covered texels\n");
    {
        const std::vector<std::uint8_t> canvas = uniqueColorCanvas();
        cp::CapePalette palette;
        cp::buildCapePalette(canvas.data(), cp::kCanvasWidth, cp::kCanvasHeight, 14, 22, palette);
        check(palette.outer.size() == 14 * 22 * 3, "fine palette has 14x22 entries");

        bool inRange = true;
        for (int j = 0; j < 22; ++j) {
            for (int i = 0; i < 14; ++i) {
                const std::size_t cell = (static_cast<std::size_t>(j) * 14 + i) * 3u;
                // Covered rectangle in canvas pixels.
                const float x0 = 1.0f + 10.0f * i / 14.0f;
                const float x1 = 1.0f + 10.0f * (i + 1) / 14.0f;
                const float y0 = 1.0f + 16.0f * j / 22.0f;
                const float y1 = 1.0f + 16.0f * (j + 1) / 22.0f;
                for (int c = 0; c < 3; ++c) {
                    std::uint8_t lo = 255, hi = 0;
                    for (int y = static_cast<int>(y0); y <= static_cast<int>(std::ceil(y1)) && y < 17; ++y) {
                        for (int x = static_cast<int>(x0); x <= static_cast<int>(std::ceil(x1)) && x < 11; ++x) {
                            const std::uint8_t v = canvasPixel(canvas, x, y)[c];
                            if (v < lo) lo = v;
                            if (v > hi) hi = v;
                        }
                    }
                    const std::uint8_t v = palette.outer[cell + c];
                    if (v < lo || v > hi) inRange = false;
                }
            }
        }
        check(inRange, "every fine-grid cell color lies within its covered texels");
    }

    // normalizeCapeCanvas: proportional layout for any size.
    std::printf("normalize any canvas size\n");
    {
        // 22x23 solid magenta -> fully magenta 64x32.
        std::vector<std::uint8_t> small(22 * 23 * 4);
        for (std::size_t i = 0; i < small.size(); i += 4) {
            small[i + 0] = 200; small[i + 1] = 40; small[i + 2] = 160; small[i + 3] = 255;
        }
        std::vector<std::uint8_t> norm;
        cp::normalizeCapeCanvas(small.data(), 22, 23, norm);
        bool solid = true;
        for (std::size_t i = 0; i < norm.size(); i += 4) {
            if (norm[i] != 200 || norm[i + 1] != 40 || norm[i + 2] != 160) solid = false;
        }
        check(norm.size() == cp::kCanvasBytes && solid, "22x23 solid canvas normalizes to a solid 64x32");

        // A 128x64 canvas split into 4 solid quadrants must land each
        // quadrant's color in the matching quadrant of the 64x32 output.
        std::vector<std::uint8_t> hd(128 * 64 * 4, 0);
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 128; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * 128 + x) * 4u;
                hd[i + 0] = x < 64 ? 255 : 0;   // left/right
                hd[i + 1] = y < 32 ? 255 : 0;   // top/bottom
                hd[i + 2] = 0;
                hd[i + 3] = 255;
            }
        }
        cp::normalizeCapeCanvas(hd.data(), 128, 64, norm);
        bool quadrants = true;
        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 64; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * 64 + x) * 4u;
                const bool left = x < 32, top = y < 16;
                if (norm[i] != (left ? 255 : 0) || norm[i + 1] != (top ? 255 : 0)) quadrants = false;
            }
        }
        check(quadrants, "128x64 HD canvas maps proportionally onto 64x32");

        // A 704x736 vertical gradient survives normalization as a gradient.
        std::vector<std::uint8_t> huge(704 * 736 * 4, 0);
        for (int y = 0; y < 736; ++y) {
            const std::uint8_t v = static_cast<std::uint8_t>(y * 255 / 735);
            for (int x = 0; x < 704; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * 704 + x) * 4u;
                huge[i + 0] = v; huge[i + 1] = v; huge[i + 2] = v; huge[i + 3] = 255;
            }
        }
        cp::normalizeCapeCanvas(huge.data(), 704, 736, norm);
        bool gradient = norm[0] < 40 && norm[(31 * 64 + 63) * 4] > 215;
        for (int y = 1; y < 32; ++y) {
            const std::uint8_t a = norm[((y - 1) * 64 + 8) * 4];
            const std::uint8_t b = norm[(y * 64 + 8) * 4];
            if (b < a) gradient = false; // non-decreasing down the canvas
        }
        check(gradient, "704x736 gradient normalizes top-dark / bottom-bright");
    }

    // End-to-end "any size" support: the exact sizes the module promises,
    // through the Custom Capes resampler and into a full palette.
    std::printf("any-size end-to-end (22x23 .. 704x736)\n");
    {
        struct SizeCase { std::uint32_t w, h; };
        const SizeCase sizes[] = {{22, 23}, {64, 32}, {128, 64}, {176, 184}, {704, 736}};

        for (const SizeCase& sc : sizes) {
            std::vector<std::uint8_t> src(static_cast<std::size_t>(sc.w) * sc.h * 4);
            for (std::uint32_t y = 0; y < sc.h; ++y) {
                for (std::uint32_t x = 0; x < sc.w; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(y) * sc.w + x) * 4u;
                    src[i + 0] = static_cast<std::uint8_t>(x * 255 / (sc.w - 1));
                    src[i + 1] = static_cast<std::uint8_t>(y * 255 / (sc.h - 1));
                    src[i + 2] = 120;
                    src[i + 3] = 255;
                }
            }

            const std::vector<std::uint8_t> cape = ccc::resampleToCape(src.data(), sc.w, sc.h);
            cp::CapePalette palette;
            cp::buildCapePalette(cape.data(), cp::kCanvasWidth, cp::kCanvasHeight, 10, 16, palette);

            bool nonEmpty = !palette.outer.empty() && !palette.inner.empty() &&
                            !palette.edgeLeft.empty() && !palette.edgeRight.empty() &&
                            !palette.edgeTop.empty() && !palette.edgeBottom.empty();
            // The 64x32 output spans x=0..255 across its width, so the cape
            // face (columns 1..11 of 64) must hold a rising red ramp.
            bool ramp = true;
            for (int j = 0; j < 16 && ramp; ++j) {
                const std::uint8_t lo = palette.outer[(static_cast<std::size_t>(j) * 10 + 0) * 3u];
                const std::uint8_t hi = palette.outer[(static_cast<std::size_t>(j) * 10 + 9) * 3u];
                if (lo >= hi) ramp = false;
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%ux%u source yields a full, left-to-right cape palette", sc.w, sc.h);
            check(nonEmpty && ramp, buf);
        }
    }

    // ------------------------------------------------------------------
    // Detail presets
    // ------------------------------------------------------------------
    std::printf("detail presets\n");
    {
        check(cp::kDetailPresets[0].cols == 10 && cp::kDetailPresets[0].rows == 16, "native preset is 10x16");
        check(cp::clampDetailIndex(-3) == 0 && cp::clampDetailIndex(99) == 0, "detail index clamps to native");
        check(cp::detailIndexFromLabel("fine") == 1 && cp::detailIndexFromLabel("FINE") == 1, "label lookup is case-insensitive");
        const std::string radio = cp::detailRadioValue(1);
        check(radio == "1,Native,Fine", "radio value is \"1,Native,Fine\"");
        int idx = -1;
        std::string name;
        ccc::parseRadioValue(radio, idx, name);
        check(idx == 1 && name == "Fine", "radio value parses back to Fine");
    }

    std::printf("\n%s\n", g_failures == 0 ? "all cape physics sim tests passed" : "some cape physics sim tests failed");
    return g_failures == 0 ? 0 : 1;
}

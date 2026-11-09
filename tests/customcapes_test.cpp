// Unit tests for the Custom Capes module's pure helpers (folder scanning,
// the launcher "radio" value format, and the cape layout/resampling —
// including the back-face-only design placement, the flat lining color on
// the inner front face, the edge strips that give the cape its thickness,
// and the tapered Elytra UV fallback generated from the same artwork).
//
// Also covered: the picker labels (file names without ".png", kept unique),
// the PNG-header validation that runs before the decoder, the three fit
// modes (Cover / Contain / Stretch), and the area-average sampler that keeps
// a large photo from turning into aliasing noise on the 10x16 face.
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src tests/customcapes_test.cpp -o /tmp/customcapes_test
//     /tmp/customcapes_test

#include "modules/player/customcapes_files.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace cc = customcapes;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

void checkEqual(const std::string& got, const std::string& want, const std::string& what) {
    check(got == want, what + " -> \"" + got + "\" (want \"" + want + "\")");
}

void checkEqual(int got, int want, const std::string& what) {
    check(got == want, what + " -> " + std::to_string(got) + " (want " + std::to_string(want) + ")");
}

std::string join(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) out += '|';
        out += items[i];
    }
    return out;
}

// True when the 64x32-canvas pixels at (ax,ay) and (bx,by) are identical.
bool samePixel(const std::vector<std::uint8_t>& img, std::uint32_t ax, std::uint32_t ay,
               std::uint32_t bx, std::uint32_t by) {
    const std::size_t a = (static_cast<std::size_t>(ay) * cc::kCapeWidth + ax) * 4u;
    const std::size_t b = (static_cast<std::size_t>(by) * cc::kCapeWidth + bx) * 4u;
    return img[a] == img[b] && img[a + 1] == img[b + 1] &&
           img[a + 2] == img[b + 2] && img[a + 3] == img[b + 3];
}

} // namespace

int main() {
    // --- folder scanning -------------------------------------------------
    std::printf("capes folder scan\n");
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "customcapes_test_dir";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "subdir"); // must be ignored

    auto touch = [&](const std::string& name) {
        std::ofstream(dir / name, std::ios::binary) << "x";
    };
    touch("zeta.png");
    touch("alpha.png");
    touch("Mojang.PNG");      // case-insensitive extension
    touch("notes.txt");       // not a png
    touch("weird,comma.png"); // commas break the radio format -> skipped
    touch("alfa.png");

    check(cc::scanCapeFiles("").empty(), "empty directory path yields no files");
    check(cc::scanCapeFiles((dir / "does_not_exist").string()).empty(),
          "missing directory yields no files");
    checkEqual(join(cc::scanCapeFiles(dir.string())),
               "Mojang.PNG|alfa.png|alpha.png|zeta.png",
               "png files only, sorted, no comma names, no subdirs");

    // --- radio serialization ---------------------------------------------
    std::printf("radio value format\n");
    checkEqual(cc::makeRadioValue(0, {}), "0,None", "no files, None selected");
    checkEqual(cc::makeRadioValue(2, {"a.png", "b.png", "c.png"}),
               "2,None,a,b,c", "selection + option list (labels without .png)");
    checkEqual(cc::makeRadioValue(-1, {"a.png"}), "0,None,a", "negative index clamps to None");
    checkEqual(cc::makeRadioValue(5, {"a.png"}), "1,None,a",
               "index past the list clamps to last option");
    checkEqual(cc::makeRadioValue(1, {"a.png", "b.png"}, {"a.png", "b.png"}),
               "1,None,a.png,b.png", "explicit labels override the derived ones");
    checkEqual(cc::makeRadioValue(1, {"a.png", "b.png"}, {"only-one"}),
               "1,None,a.png,b.png", "label list of the wrong size is ignored");
    checkEqual(cc::makeRadioValue(1, {"Cape.png", "Cape.PNG", "Other.png"}),
               "1,None,Cape,Cape.PNG,Other",
               "colliding labels fall back to the file name so options stay unique");

    // labels
    std::printf("picker labels\n");
    checkEqual(join(cc::makeCapeLabels({"dir/x.png", "logo.PNG", "noext"})),
               "dir/x|logo|noext", "extension stripped, case-insensitive");
    checkEqual(join(cc::makeCapeLabels({".png", "a.png"})),
               ".png|a", "a bare '.png' name keeps its file name instead of an empty label");

    // --- radio parsing -----------------------------------------------------
    std::printf("radio value parsing\n");
    int idx = -1;
    std::string name;

    check(cc::parseRadioValue("2,None,a.png,b.png,c.png", idx, name), "full value parses");
    checkEqual(idx, 2, "embedded index");
    checkEqual(name, "b.png", "embedded option name");

    check(cc::parseRadioValue("0,None,a.png", idx, name), "None value parses");
    checkEqual(idx, 0, "None index");
    checkEqual(name, "None", "None name");

    check(cc::parseRadioValue("2", idx, name), "launcher change value parses");
    checkEqual(idx, 2, "plain index from menu");
    checkEqual(name, "", "plain index has no name");

    check(cc::parseRadioValue("a.png", idx, name), "bare file name parses");
    checkEqual(idx, 0, "bare name -> index 0");
    checkEqual(name, "a.png", "bare name recovered");

    check(cc::parseRadioValue("9,None,a.png", idx, name), "oversized index parses");
    checkEqual(idx, 9, "oversized index kept");
    checkEqual(name, "", "oversized index has no embedded name");

    check(!cc::parseRadioValue("", idx, name), "empty value rejected");

    // --- selection resolution ---------------------------------------------
    std::printf("selection resolution\n");
    const std::vector<std::string> files{"a.png", "b.png", "c.png"};
    checkEqual(cc::resolveSelectionIndex(3, "c.png", files), 3, "name and index agree");
    checkEqual(cc::resolveSelectionIndex(1, "c.png", files), 3,
               "stale index fixed via recovered name (list reordered)");
    checkEqual(cc::resolveSelectionIndex(2, "deleted.png", files), 0,
               "deleted file falls back to None");
    checkEqual(cc::resolveSelectionIndex(2, "", files), 2,
               "menu change (no name) stays positional");
    checkEqual(cc::resolveSelectionIndex(0, "b", files), 2,
               "a short display label resolves back to its file");
    checkEqual(cc::resolveSelectionIndex(2, "b.png", files), 2,
               "a raw file name still resolves after the switch to labels");
    checkEqual(cc::resolveSelectionIndex(0, "None", files), 0, "None stays None");
    checkEqual(cc::resolveSelectionIndex(4, "", files), 0, "positional overflow clamps to None");
    checkEqual(cc::resolveSelectionIndex(-1, "", files), 0, "negative clamps to None");

    // full save -> load round-trip through the picker value
    {
        const std::string saved = cc::makeRadioValue(2, files);
        int rIdx = -1;
        std::string rName;
        cc::parseRadioValue(saved, rIdx, rName);
        checkEqual(cc::resolveSelectionIndex(rIdx, rName, files), 2,
                   "save/load round-trip keeps selection");
    }

    // --- resampling ---------------------------------------------------------
    std::printf("cape resampling\n");
    {
        // Identity for a complete manually-authored canvas that already has
        // Elytra pixels: every byte and every custom wing shape is preserved.
        std::vector<std::uint8_t> src(cc::kCapeWidth * cc::kCapeHeight * 4u);
        for (std::size_t i = 0; i < src.size(); ++i) src[i] = static_cast<std::uint8_t>(i % 251);
        const std::vector<std::uint8_t> out = cc::resampleToCape(src.data(), cc::kCapeWidth, cc::kCapeHeight);
        check(out == src, "64x32 input with authored Elytra pixels stays byte-identical");
    }
    {
        // A traditional 64x32 cape with an empty right-hand/Elytra area gets
        // a generated wing fallback, while its authored cape face stays put.
        std::vector<std::uint8_t> src(cc::kCapeWidth * cc::kCapeHeight * 4u, 0);
        for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y) {
            for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
                const std::size_t i =
                    (static_cast<std::size_t>(cc::kCapeBackY + y) * cc::kCapeWidth +
                     cc::kCapeBackX + x) * 4u;
                src[i + 0] = 20;
                src[i + 1] = 180;
                src[i + 2] = 60;
                src[i + 3] = 255;
            }
        }
        check(!cc::hasElytraArtwork(src), "transparent Elytra UV is detected as empty");
        const std::vector<std::uint8_t> out =
            cc::resampleToCape(src.data(), cc::kCapeWidth, cc::kCapeHeight);
        check(cc::hasElytraArtwork(out), "empty 64x32 Elytra UV receives generated artwork");
        const std::size_t wing =
            (static_cast<std::size_t>(11) * cc::kCapeWidth + cc::kElytraBackX) * 4u;
        check(out[wing] == 20 && out[wing + 1] == 180 &&
                  out[wing + 2] == 60 && out[wing + 3] == 255,
              "generated Elytra uses the existing cape-face colors");
        bool capeUnchanged = true;
        for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y) {
            for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
                const std::size_t i =
                    (static_cast<std::size_t>(cc::kCapeBackY + y) * cc::kCapeWidth +
                     cc::kCapeBackX + x) * 4u;
                for (int c = 0; c < 4; ++c) capeUnchanged &= out[i + c] == src[i + c];
            }
        }
        check(capeUnchanged, "authored cape face remains intact while adding Elytra artwork");
    }
    {
        // 2x1 red|blue source — now with Bilinear + Smart Crop (aspect preserve)
        // Source aspect 2.0 > target 0.625, so we crop width centrally.
        // CropW = 1*0.625=0.625 centered at boundary (1.0) => blend of red+blue.
        // Expected: back face is uniform purple-ish (bilinear blend), not half red half blue.
        const std::uint8_t src[8] = {255, 0, 0, 255, 0, 0, 255, 255};
        const std::vector<std::uint8_t> out = cc::resampleToCape(src, 2, 1);
        check(out.size() == cc::kCapeWidth * cc::kCapeHeight * 4u, "output is 64x32 RGBA (2x1)");

        // For tiny 2x1, cropW <1 guard keeps full width, so we get red left, blue right, blended middle via bilinear
        bool leftRed = true, rightBlue = true, hasBlend = false;
        for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y) {
            for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
                const std::size_t i =
                    (static_cast<std::size_t>(cc::kCapeBackY + y) * cc::kCapeWidth +
                     cc::kCapeBackX + x) * 4u;
                if (x < 3) leftRed &= out[i]==255 && out[i+1]==0 && out[i+2]==0;
                if (x >= 7) rightBlue &= out[i]==0 && out[i+1]==0 && out[i+2]==255;
                if (x>=3 && x<=6) {
                    if (out[i+0]>0 && out[i+0]<255 && out[i+2]>0 && out[i+2]<255) hasBlend = true;
                }
            }
        }
        check(leftRed, "bilinear: left edge stays red for 2x1");
        check(rightBlue, "bilinear: right edge stays blue for 2x1");
        check(hasBlend, "bilinear: middle is blended (smooth) for 2x1");

        // inner front face: one flat lining color = half the average of the
        // sampled back face (computed from the output so the expectation
        // follows the sampler instead of hard-coding a rounding).
        bool frontUniform = true, frontIsLiner = true;
        double sumR = 0.0, sumB = 0.0;
        for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y) {
            for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
                const std::size_t i =
                    (static_cast<std::size_t>(cc::kCapeBackY + y) * cc::kCapeWidth +
                     cc::kCapeBackX + x) * 4u;
                sumR += out[i];
                sumB += out[i + 2];
            }
        }
        const double area = static_cast<double>(cc::kCapeBackWidth * cc::kCapeBackHeight);
        const int wantR = static_cast<int>(sumR / area * 0.5 + 0.5);
        const int wantB = static_cast<int>(sumB / area * 0.5 + 0.5);
        const std::size_t first =
            (static_cast<std::size_t>(cc::kCapeFrontY) * cc::kCapeWidth + cc::kCapeFrontX) * 4u;
        for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y) {
            for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
                const std::size_t i =
                    (static_cast<std::size_t>(cc::kCapeFrontY + y) * cc::kCapeWidth +
                     cc::kCapeFrontX + x) * 4u;
                frontIsLiner &= std::abs(out[i] - wantR) <= 1 && out[i + 1] == 0 &&
                                std::abs(out[i + 2] - wantB) <= 1 && out[i + 3] == 255;
                frontUniform &= out[i] == out[first] && out[i + 1] == out[first + 1] &&
                                out[i + 2] == out[first + 2] && out[i + 3] == out[first + 3];
            }
        }
        check(frontIsLiner, "front face is half the average back-face color (flat lining)");
        check(frontUniform, "front face is a single uniform color after smart crop");

        // edges copy adjacent back-face pixels
        bool edgesCopyAdjacent = true;
        for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
            edgesCopyAdjacent &=
                samePixel(out, cc::kCapeTopX + x, cc::kCapeTopY, cc::kCapeBackX + x, cc::kCapeBackY);
            edgesCopyAdjacent &= samePixel(out, cc::kCapeBottomX + x, cc::kCapeBottomY,
                                           cc::kCapeBackX + x,
                                           cc::kCapeBackY + cc::kCapeBackHeight - 1);
        }
        for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y) {
            edgesCopyAdjacent &= samePixel(out, cc::kCapeSideRightX, cc::kCapeSideY + y,
                                           cc::kCapeBackX, cc::kCapeBackY + y);
            edgesCopyAdjacent &= samePixel(out, cc::kCapeSideLeftX, cc::kCapeSideY + y,
                                           cc::kCapeBackX + cc::kCapeBackWidth - 1,
                                           cc::kCapeBackY + y);
        }
        check(edgesCopyAdjacent, "edge strips exactly copy adjacent back-face pixels (smart-crop)");

        bool exactWingMask = true;
        for (std::uint32_t y = 0; y < cc::kElytraUvHeight; ++y) {
            for (std::uint32_t x = cc::kElytraUvX;
                 x < cc::kElytraUvX + cc::kElytraUvWidth; ++x) {
                if (!cc::isElytraUvPixel(x, y)) continue;
                const std::size_t i =
                    (static_cast<std::size_t>(y) * cc::kCapeWidth + x) * 4u;
                exactWingMask &= (out[i + 3] != 0) == cc::isElytraMaskPixel(x, y);
            }
        }
        check(exactWingMask, "Elytra artwork keeps the vanilla tapered alpha silhouette (2x1)");
        check(cc::hasElytraArtwork(out), "generated cape reports visible Elytra artwork (2x1)");
    }
    {
        // exact edge continuation: a 10x16 source maps 1:1 onto the back
        // face, so every edge-strip pixel must equal its adjacent back-face
        // edge pixel exactly. Bilinear with exact size should still be exact.
        std::vector<std::uint8_t> src(cc::kCapeBackWidth * cc::kCapeBackHeight * 4u);
        for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y) {
            for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * cc::kCapeBackWidth + x) * 4u;
                src[i + 0] = static_cast<std::uint8_t>(x * 25);
                src[i + 1] = static_cast<std::uint8_t>(y * 16 + 1);
                src[i + 2] = 200;
                src[i + 3] = 255;
            }
        }
        const std::vector<std::uint8_t> out =
            cc::resampleToCape(src.data(), cc::kCapeBackWidth, cc::kCapeBackHeight);
        bool exact = true;
        for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
            exact &= samePixel(out, cc::kCapeTopX + x, cc::kCapeTopY,
                               cc::kCapeBackX + x, cc::kCapeBackY);
            exact &= samePixel(out, cc::kCapeBottomX + x, cc::kCapeBottomY,
                               cc::kCapeBackX + x, cc::kCapeBackY + cc::kCapeBackHeight - 1);
        }
        for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y) {
            exact &= samePixel(out, cc::kCapeSideRightX, cc::kCapeSideY + y,
                               cc::kCapeBackX, cc::kCapeBackY + y);
            exact &= samePixel(out, cc::kCapeSideLeftX, cc::kCapeSideY + y,
                               cc::kCapeBackX + cc::kCapeBackWidth - 1, cc::kCapeBackY + y);
        }
        check(exact, "edge strips exactly copy the adjacent back-face edge pixels (10x16)");
    }
    {
        // degenerate input never crashes and stays transparent-black
        const std::vector<std::uint8_t> out = cc::resampleToCape(nullptr, 0, 0);
        check(out.size() == cc::kCapeWidth * cc::kCapeHeight * 4u, "null input still yields canvas");
        bool allZero = true;
        for (std::uint8_t b : out) allZero &= b == 0;
        check(allZero, "null input is transparent");
    }

    // --- new custom sizes required by task ---------------------------------
    std::printf("custom sizes 22x23, 88x92, 176x184, 704x736, 736x797\n");
    {
        struct TestSize { std::uint32_t w, h; };
        std::vector<TestSize> sizes = {{22,23},{88,92},{176,184},{704,736},{736,797}};
        for (auto sz : sizes) {
            // Create a gradient test image for this size
            std::vector<std::uint8_t> src(static_cast<std::size_t>(sz.w) * sz.h * 4u);
            for (std::uint32_t y = 0; y < sz.h; ++y) {
                for (std::uint32_t x = 0; x < sz.w; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(y) * sz.w + x) * 4u;
                    src[i+0] = static_cast<std::uint8_t>((x * 255) / (sz.w>1?sz.w-1:1)); // R gradient X
                    src[i+1] = static_cast<std::uint8_t>((y * 255) / (sz.h>1?sz.h-1:1)); // G gradient Y
                    src[i+2] = static_cast<std::uint8_t>(128);
                    src[i+3] = 255;
                }
            }
            const std::vector<std::uint8_t> out = cc::resampleToCape(src.data(), sz.w, sz.h);
            check(out.size() == cc::kCapeWidth * cc::kCapeHeight * 4u,
                  "custom size " + std::to_string(sz.w) + "x" + std::to_string(sz.h) + " yields 64x32");

            // Check that back face is not empty and has gradient preserved (bilinear should keep gradient)
            bool backHasContent = false;
            bool backAlphaOk = true;
            for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y) {
                for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(cc::kCapeBackY + y) * cc::kCapeWidth + cc::kCapeBackX + x) * 4u;
                    if (out[i+3] != 0) backHasContent = true;
                    backAlphaOk &= out[i+3] == 255;
                }
            }
            check(backHasContent, "back face has content for " + std::to_string(sz.w) + "x" + std::to_string(sz.h));
            check(backAlphaOk, "back face alpha preserved for " + std::to_string(sz.w) + "x" + std::to_string(sz.h));

            // Smart crop: verify that aspect ratio is preserved — for these sizes, source is wider than target,
            // so crop should happen on width. The resulting back face should NOT be stretched vertically:
            // top row should be different from bottom row due to Y gradient, but left/right should be similar after crop?
            // We test that bilinear gives smooth gradient, not blocky nearest neighbor:
            // For bilinear, adjacent pixels in back face should differ by small amount, not jump.
            bool smooth = true;
            for (std::uint32_t y = 0; y < cc::kCapeBackHeight-1; ++y) {
                for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
                    const std::size_t i1 = (static_cast<std::size_t>(cc::kCapeBackY + y) * cc::kCapeWidth + cc::kCapeBackX + x) * 4u;
                    const std::size_t i2 = (static_cast<std::size_t>(cc::kCapeBackY + y+1) * cc::kCapeWidth + cc::kCapeBackX + x) * 4u;
                    int diff = std::abs(static_cast<int>(out[i1+1]) - static_cast<int>(out[i2+1])); // G channel vertical diff
                    // For gradient 0-255 over 16 pixels, diff should be ~16, not 0 or >100 (blocky would be 0 then jump)
                    if (diff > 50) smooth = false;
                }
            }
            check(smooth, "bilinear smooth gradient for " + std::to_string(sz.w) + "x" + std::to_string(sz.h));

            // Edge strips should copy adjacent back face (thickness auto-fill)
            bool edgesOk = true;
            for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
                edgesOk &= samePixel(out, cc::kCapeTopX + x, cc::kCapeTopY, cc::kCapeBackX + x, cc::kCapeBackY);
                edgesOk &= samePixel(out, cc::kCapeBottomX + x, cc::kCapeBottomY,
                                     cc::kCapeBackX + x, cc::kCapeBackY + cc::kCapeBackHeight - 1);
            }
            check(edgesOk, "edge strips auto-fill for " + std::to_string(sz.w) + "x" + std::to_string(sz.h));

            // Elytra should be generated
            check(cc::hasElytraArtwork(out), "elytra generated for " + std::to_string(sz.w) + "x" + std::to_string(sz.h));

            // Front face should be flat lining (half brightness)
            bool frontUniform = true;
            const std::size_t firstFront = (static_cast<std::size_t>(cc::kCapeFrontY) * cc::kCapeWidth + cc::kCapeFrontX) * 4u;
            for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y) {
                for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(cc::kCapeFrontY + y) * cc::kCapeWidth + cc::kCapeFrontX + x) * 4u;
                    frontUniform &= out[i]==out[firstFront] && out[i+1]==out[firstFront+1] && out[i+2]==out[firstFront+2];
                }
            }
            check(frontUniform, "front face flat lining for " + std::to_string(sz.w) + "x" + std::to_string(sz.h));
        }
    }

    // --- bilinear vs nearest neighbor specific test ------------------------
    std::printf("bilinear specific\n");
    {
        // 4x4 checker: bilinear should produce intermediate values when downscaled to 10x16 (upscale)
        // Create 2x2 with distinct corners, upscale to back face 10x16 should be smooth gradient, not blocky
        std::vector<std::uint8_t> src(2*2*4);
        src[0]=0; src[1]=0; src[2]=0; src[3]=255;       // black
        src[4]=255; src[5]=0; src[6]=0; src[7]=255;     // red
        src[8]=0; src[9]=255; src[10]=0; src[11]=255;   // green
        src[12]=0; src[13]=0; src[14]=255; src[15]=255; // blue
        auto out = cc::resampleToCape(src.data(), 2, 2);
        // Center of back face should be blend, not pure black/red/green/blue
        const std::size_t center = (static_cast<std::size_t>(cc::kCapeBackY + cc::kCapeBackHeight/2) * cc::kCapeWidth + cc::kCapeBackX + cc::kCapeBackWidth/2) * 4u;
        bool isBlend = !(out[center]==0 && out[center+1]==0 && out[center+2]==0) &&
                       !(out[center]==255 && out[center+1]==0 && out[center+2]==0) &&
                       out[center+3]==255;
        check(isBlend, "bilinear produces blended intermediate values (not nearest neighbor)");
    }

    // --- PNG header validation -------------------------------------------
    std::printf("png header validation\n");
    {
        std::vector<std::uint8_t> png(33, 0);
        const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        std::memcpy(png.data(), sig, 8);
        const char ihdr[4] = {'I', 'H', 'D', 'R'};
        std::memcpy(png.data() + 12, ihdr, 4);
        auto setSize = [&](std::uint32_t w, std::uint32_t h) {
            png[16] = static_cast<std::uint8_t>(w >> 24); png[17] = static_cast<std::uint8_t>(w >> 16);
            png[18] = static_cast<std::uint8_t>(w >> 8);  png[19] = static_cast<std::uint8_t>(w);
            png[20] = static_cast<std::uint8_t>(h >> 24); png[21] = static_cast<std::uint8_t>(h >> 16);
            png[22] = static_cast<std::uint8_t>(h >> 8);  png[23] = static_cast<std::uint8_t>(h);
        };
        setSize(64, 32);
        png[24] = 8;  // bit depth
        png[25] = 6;  // truecolour + alpha
        std::uint32_t w = 0, h = 0;
        check(cc::readPngHeader(png.data(), png.size(), w, h), "valid PNG header parses");
        check(w == 64 && h == 32, "IHDR dimensions are read big-endian");

        std::vector<std::uint8_t> notPng = png;
        notPng[1] = 'J'; // PNG -> JNG signature
        check(!cc::readPngHeader(notPng.data(), notPng.size(), w, h),
              "a renamed JPEG is rejected before decoding");

        std::vector<std::uint8_t> truncated = png;
        truncated.resize(20);
        check(!cc::readPngHeader(truncated.data(), truncated.size(), w, h),
              "a truncated header is rejected");

        std::vector<std::uint8_t> badDepth = png;
        badDepth[24] = 3; // 3-bit is not a legal PNG bit depth
        check(!cc::readPngHeader(badDepth.data(), badDepth.size(), w, h),
              "an illegal bit depth is rejected");

        std::vector<std::uint8_t> zero = png;
        zero[16] = zero[17] = zero[18] = zero[19] = 0; // width 0
        check(!cc::readPngHeader(zero.data(), zero.size(), w, h), "a zero width is rejected");
        check(!cc::readPngHeader(nullptr, 0, w, h), "null data is rejected");

        check(cc::isSupportedSourceSize(64, 32), "64x32 is a supported source size");
        check(cc::isSupportedSourceSize(2048, 2048), "2048x2048 is the largest supported source");
        check(!cc::isSupportedSourceSize(4096, 4096), "4096x4096 (64 MB decoded) is rejected");
        check(!cc::isSupportedSourceSize(0, 0), "zero-sized source is rejected");
    }

    // --- large-image prefilter -------------------------------------------
    std::printf("large-image prefilter\n");
    {
        check(cc::sourceNeedsPrefilter(2048, 2048), "2048x2048 is prefiltered");
        check(!cc::sourceNeedsPrefilter(1024, 1024), "1024x1024 is sampled directly");
        check(!cc::sourceNeedsPrefilter(22, 23), "a tiny cape needs no prefilter");

        std::uint32_t w = 0, h = 0;
        cc::prefilterSize(2048, 1024, w, h);
        check(w == 1024 && h == 512, "the longer side is capped, aspect kept");
        cc::prefilterSize(1024, 4096, w, h);
        check(w == 256 && h == 1024, "a tall image is capped on its height");
        cc::prefilterSize(3000, 3001, w, h);
        check(w == 1024 && h == 1024, "near-square aspect stays within one pixel");
        cc::prefilterSize(0, 0, w, h);
        check(w == 1 && h == 1, "a degenerate size still yields a 1x1 target");

        // Downscaling a solid colour must keep that colour exactly, and the
        // result must be usable as a source for the cape face.
        std::vector<std::uint8_t> solid(2048 * 1024 * 4);
        for (std::size_t i = 0; i < solid.size(); i += 4) {
            solid[i + 0] = 12;
            solid[i + 1] = 200;
            solid[i + 2] = 90;
            solid[i + 3] = 255;
        }
        const auto reduced = cc::downscaleArea(solid.data(), 2048, 1024, 1024, 512);
        check(reduced.size() == static_cast<std::size_t>(1024) * 512 * 4u,
              "the prefilter returns the requested size");
        bool solidKept = true;
        for (std::size_t i = 0; i < reduced.size(); i += 4) {
            solidKept &= reduced[i] == 12 && reduced[i + 1] == 200 && reduced[i + 2] == 90 &&
                         reduced[i + 3] == 255;
        }
        check(solidKept, "an area downscale of a solid colour is exact");
        check(cc::downscaleArea(nullptr, 4, 4, 2, 2).empty(), "null input yields no image");
        check(cc::downscaleArea(solid.data(), 2048, 1024, 0, 0).empty(),
              "a zero target yields no image");

        // The checkerboard test again, but through the prefilter: it must still
        // end up mid grey (i.e. the prefilter itself does not alias).
        std::vector<std::uint8_t> checker(2048 * 2048 * 4);
        for (std::uint32_t y = 0; y < 2048; ++y) {
            for (std::uint32_t x = 0; x < 2048; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * 2048 + x) * 4u;
                const std::uint8_t v = ((x ^ y) & 1u) ? 255 : 0;
                checker[i + 0] = v;
                checker[i + 1] = v;
                checker[i + 2] = v;
                checker[i + 3] = 255;
            }
        }
        const auto down = cc::downscaleArea(checker.data(), 2048, 2048, 1024, 1024);
        check(down[0] > 60 && down[0] < 195,
              "a 2048px checkerboard becomes mid grey after the prefilter");
        const auto cape = cc::resampleToCapeEx(down.data(), 1024, 1024, cc::FitMode::Cover);
        const std::size_t middle =
            (static_cast<std::size_t>(cc::kCapeBackY + 8) * cc::kCapeWidth + cc::kCapeBackX + 5) * 4u;
        check(cape[middle] > 60 && cape[middle] < 195,
              "the prefiltered source still lands as smooth mid grey on the cape");
    }

    // --- fit modes --------------------------------------------------------
    std::printf("fit modes\n");
    {
        // 20x10 (aspect 2.0) with a red border and a green center: Cover must
        // crop the borders away, Contain must keep some of them.
        const std::uint32_t W = 20, H = 10;
        std::vector<std::uint8_t> src(static_cast<std::size_t>(W) * H * 4u, 255);
        for (std::uint32_t y = 0; y < H; ++y) {
            for (std::uint32_t x = 0; x < W; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4u;
                const bool border = x < 2 || x >= W - 2;
                src[i + 0] = border ? 255 : 0;
                src[i + 1] = border ? 0 : 255;
                src[i + 2] = 0;
                src[i + 3] = 255;
            }
        }

        auto redPixels = [&](const std::vector<std::uint8_t>& out) {
            int red = 0;
            for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y) {
                for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(cc::kCapeBackY + y) *
                                               cc::kCapeWidth + cc::kCapeBackX + x) * 4u;
                    if (out[i] > 200 && out[i + 1] < 80) ++red;
                }
            }
            return red;
        };

        const auto cover = cc::resampleToCapeEx(src.data(), W, H, cc::FitMode::Cover);
        const auto contain = cc::resampleToCapeEx(src.data(), W, H, cc::FitMode::Contain);
        const auto stretch = cc::resampleToCapeEx(src.data(), W, H, cc::FitMode::Stretch);
        check(redPixels(cover) == 0, "Cover crops the border away completely");
        check(redPixels(contain) > redPixels(cover),
              "Contain keeps the border (letterboxed, not cropped)");
        check(cover.size() == contain.size() && contain.size() == stretch.size(),
              "every fit mode yields the same canvas size");
        check(cc::resampleToCape(src.data(), W, H) == cover,
              "the default entry point is Cover");
        check(cc::fitModeLabel(cc::fitModeFromInt(1)) == std::string("Contain"),
              "fit mode labels round-trip");
        check(cc::fitModeFromInt(99) == cc::FitMode::Cover, "an out-of-range fit maps to Cover");
        check(cc::fitModeFromInt(-3) == cc::FitMode::Cover, "a negative fit maps to Cover");

        // Cover must preserve the source aspect ratio: a circle stays a circle,
        // i.e. the horizontal and vertical scale factors are equal.
        const auto crop = cc::computeCrop(400, 100, cc::kCapeBackWidth, cc::kCapeBackHeight,
                                          cc::FitMode::Cover);
        const double scaleX = crop.width / cc::kCapeBackWidth;
        const double scaleY = crop.height / cc::kCapeBackHeight;
        check(std::abs(scaleX - scaleY) < 1e-9, "Cover uses one scale factor for both axes");
        check(crop.x > 0.0 && crop.y == 0.0, "a wide image is cropped horizontally, centered");

        const auto stretchCrop = cc::computeCrop(400, 100, cc::kCapeBackWidth, cc::kCapeBackHeight,
                                                 cc::FitMode::Stretch);
        check(stretchCrop.width == 400.0 && stretchCrop.height == 100.0,
              "Stretch keeps the whole source (aspect not preserved)");
    }

    // --- area-average downscaling ----------------------------------------
    std::printf("area-average downscaling\n");
    {
        // A 1-pixel checkerboard is pure high-frequency noise: any sampler that
        // reads single texels returns black or white, while an area average
        // returns mid grey. This is the aliasing the old bilinear sampler had.
        std::vector<std::uint8_t> checker(256 * 256 * 4);
        for (std::uint32_t y = 0; y < 256; ++y) {
            for (std::uint32_t x = 0; x < 256; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * 256 + x) * 4u;
                const std::uint8_t v = ((x ^ y) & 1u) ? 255 : 0;
                checker[i + 0] = v;
                checker[i + 1] = v;
                checker[i + 2] = v;
                checker[i + 3] = 255;
            }
        }
        const auto out = cc::resampleToCapeEx(checker.data(), 256, 256, cc::FitMode::Cover);
        int mid = 0, extreme = 0;
        for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y) {
            for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x) {
                const std::size_t i = (static_cast<std::size_t>(cc::kCapeBackY + y) *
                                           cc::kCapeWidth + cc::kCapeBackX + x) * 4u;
                const int v = out[i];
                if (v > 60 && v < 195) ++mid;
                if (v < 10 || v > 245) ++extreme;
            }
        }
        check(mid >= 150, "a 1px checkerboard averages to mid grey (no aliasing)");
        check(extreme == 0, "no texel keeps a pure black/white noise value");
    }

    // --- premultiplied alpha ---------------------------------------------
    std::printf("transparent source handling\n");
    {
        // A solid red square on a fully transparent (but black-RGB) background:
        // the transparent pixels must not darken the result.
        const std::uint32_t W = 32, H = 32;
        std::vector<std::uint8_t> src(static_cast<std::size_t>(W) * H * 4u, 0);
        for (std::uint32_t y = 8; y < 24; ++y) {
            for (std::uint32_t x = 8; x < 24; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * W + x) * 4u;
                src[i + 0] = 255;
                src[i + 3] = 255;
            }
        }
        const auto out = cc::resampleToCapeEx(src.data(), W, H, cc::FitMode::Cover);
        // Middle of the design: opaque red, not a darkened blend.
        const std::size_t middle =
            (static_cast<std::size_t>(cc::kCapeBackY + cc::kCapeBackHeight / 2) * cc::kCapeWidth +
             cc::kCapeBackX + cc::kCapeBackWidth / 2) * 4u;
        check(out[middle + 0] > 240 && out[middle + 3] == 255,
              "an opaque logo keeps its colour next to transparent texels");
        // The inner lining must not be black just because the source was.
        const std::size_t front =
            (static_cast<std::size_t>(cc::kCapeFrontY) * cc::kCapeWidth + cc::kCapeFrontX) * 4u;
        check(out[front + 0] > 0 && out[front + 3] == 255,
              "the lining colour ignores fully transparent texels");
    }

    std::filesystem::remove_all(dir, ec);

    std::printf("\n%s\n", g_failures == 0 ? "all custom capes tests passed" : "SOME TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}

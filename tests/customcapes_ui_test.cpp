// Unit tests for the Custom Capes in-game preview grid (CustomCapesUi) and
// its UV-crop texture helper (customcapes::makeCapeFacePreview).
//
// Covered:
//   * the preview bitmap is exactly the visible outer cape face (10x16
//     region at UV x=1..11, y=1..17 of the 64x32 canvas), bilinearly
//     upscaled 8x — and NEVER the whole 64x32 texture atlas (front lining,
//     edge strips and Elytra UV area must not bleed into the thumbnail).
//   * addCapeEntry() feeds that cropped RGBA bitmap to the overlay texture
//     loader (pl::modmenu::registerImage) and appends a grid card.
//   * the grid layout is deterministic: fixed 4-column layout, regular
//     spacing, centered panel (launcher "-20000" convention).
//   * hit-testing maps taps to entries, to the close button, or to "let the
//     game have it"; the same layout is used for drawing and hit-testing.
//   * the selected entry gets the glowing gold border (4 border strips +
//     4 soft glow strips), other entries get none.
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src -I include -I third_party -I tests/fakepl
//         tests/customcapes_ui_test.cpp src/modules/player/customcapes_ui.cpp
//         -o /tmp/customcapes_ui_test
//     /tmp/customcapes_ui_test

#include "modules/player/customcapes_ui.hpp"
#include "modules/player/customcapes_files.hpp"

#include <cstdio>
#include <string>
#include <vector>

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

// 64x32 canvas where every UV region gets its own flat color so the crop can
// be told apart from the rest of the atlas:
//   back face (1,1) 10x16  -> red   (the design)
//   front face (12,1)      -> green (lining)
//   Elytra UV (22,0)       -> blue
//   side strips / rest     -> gray / purple
std::vector<std::uint8_t> makeRegionCanvas() {
    std::vector<std::uint8_t> canvas(64u * 32u * 4u, 0);
    const auto set = [&](std::uint32_t x, std::uint32_t y, std::uint8_t r,
                         std::uint8_t g, std::uint8_t b) {
        std::uint8_t* p = &canvas[(static_cast<std::size_t>(y) * 64u + x) * 4u];
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    };

    // Default color: gray.
    for (std::size_t i = 0; i < canvas.size(); i += 4) {
        canvas[i + 0] = 90; canvas[i + 1] = 90; canvas[i + 2] = 90; canvas[i + 3] = 255;
    }
    // Back (visible) face.
    for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y)
        for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x)
            set(cc::kCapeBackX + x, cc::kCapeBackY + y, 200, 0, 0);
    // Front (lining) face.
    for (std::uint32_t y = 0; y < cc::kCapeBackHeight; ++y)
        for (std::uint32_t x = 0; x < cc::kCapeBackWidth; ++x)
            set(cc::kCapeFrontX + x, cc::kCapeFrontY + y, 0, 220, 0);
    // Elytra UV area.
    for (std::uint32_t y = cc::kElytraUvY; y < cc::kElytraUvY + cc::kElytraUvHeight; ++y)
        for (std::uint32_t x = cc::kElytraUvX; x < cc::kElytraUvX + cc::kElytraUvWidth; ++x)
            set(x, y, 0, 0, 255);
    // Side strips (stand in for the whole "rest of the atlas").
    set(0, 1, 255, 0, 255);
    set(11, 1, 255, 0, 255);
    return canvas;
}

bool pixelIs(const std::vector<std::uint8_t>& img, std::size_t i,
             std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return img[i] == r && img[i + 1] == g && img[i + 2] == b && img[i + 3] == 255;
}

void testUvCrop() {
    std::printf("UV crop: preview shows the cape face only\n");
    const std::vector<std::uint8_t> canvas = makeRegionCanvas();
    const std::vector<std::uint8_t> preview =
        cc::makeCapeFacePreview(canvas.data(), cc::kCapePreviewScale);

    check(preview.size() == cc::kCapePreviewWidth * cc::kCapePreviewHeight * 4u,
          "preview size is (10x8) x (16x8) x 4 bytes");

    const std::size_t stride = cc::kCapePreviewWidth * 4u;
    check(pixelIs(preview, 0, 200, 0, 0),
          "preview top-left is the cape face's top-left texel");
    check(pixelIs(preview, (cc::kCapePreviewHeight - 1) * stride +
                               (cc::kCapePreviewWidth - 1) * 4u, 200, 0, 0),
          "preview bottom-right is the cape face's bottom-right texel");
    check(pixelIs(preview, (cc::kCapePreviewHeight / 2) * stride +
                               (cc::kCapePreviewWidth / 2) * 4u, 200, 0, 0),
          "preview center samples the cape face");

    // Nothing from the rest of the 64x32 atlas may leak into the thumbnail.
    bool sawLining = false, sawElytra = false, sawGray = false, sawSide = false;
    for (std::size_t i = 0; i < preview.size(); i += 4) {
        if (pixelIs(preview, i, 0, 220, 0)) sawLining = true;
        if (pixelIs(preview, i, 0, 0, 255)) sawElytra = true;
        if (pixelIs(preview, i, 90, 90, 90)) sawGray = true;
        if (pixelIs(preview, i, 255, 0, 255)) sawSide = true;
    }
    check(!sawLining, "front (lining) face texels do not leak into the preview");
    check(!sawElytra, "Elytra UV texels do not leak into the preview");
    check(!sawGray, "outside-face canvas texels do not leak into the preview");
    check(!sawSide, "side/edge strip texels do not leak into the preview");
}

void testGridLayoutAndHitTest() {
    std::printf("grid layout: deterministic 4-column centered panel\n");
    CustomCapesUi ui;
    ui.clear();

    const auto& layout = CustomCapesUi::computeLayout(5);
    check(layout.cards.size() == 5, "one card per entry, 5 entries -> 5 cards");
    check(layout.panel.x < CustomCapesUi::kCenter && layout.panel.y < CustomCapesUi::kCenter,
          "panel is anchored around the screen center");
    check(layout.panel.contains(layout.panel.x + layout.panel.w * 0.5f,
                                layout.panel.y + layout.panel.h * 0.5f),
          "panel rect is coherent");

    // Row 0 fills all 4 columns, row 1 starts over at column 0.
    check(layout.cards[1].x == layout.cards[0].x + CustomCapesUi::kCardW + CustomCapesUi::kGapX,
          "horizontal spacing is uniform");
    check(layout.cards[3].x == layout.cards[0].x + 3.0f * (CustomCapesUi::kCardW + CustomCapesUi::kGapX),
          "4th card sits in column 3");
    check(layout.cards[4].x == layout.cards[0].x, "5th card wraps to column 0");
    check(layout.cards[4].y == layout.cards[0].y + CustomCapesUi::kCardH + CustomCapesUi::kGapY,
          "5th card starts the second row");

    ui.addEntry("None", "");             // index 0
    ui.addEntry("a.png", "img-a");       // 1
    ui.addEntry("b.png", "img-b");       // 2
    ui.addEntry("c.png", "img-c");       // 3
    ui.addEntry("d.png", "img-d");       // 4

    const auto layout2 = ui.layout();
    check(layout2.cards.size() == ui.entryCount(), "hit layout matches entry count");

    const auto& c0 = layout2.cards[0];
    const auto& c2 = layout2.cards[2];
    const auto& close = layout2.close;
    check(ui.hitTest(c0.x + c0.w * 0.5f, c0.y + c0.h * 0.5f) == 0,
          "tap on the None card selects None (index 0)");
    check(ui.hitTest(c2.x + c2.w * 0.5f, c2.y + c2.h * 0.5f) == 2,
          "tap on a card selects its grid index");
    check(ui.hitTest(close.x + close.w * 0.5f, close.y + close.h * 0.5f) ==
              CustomCapesUi::kHitClose,
          "tap on the close button is reported as kHitClose");
    check(ui.hitTest(0.0f, 0.0f) == CustomCapesUi::kHitOutside,
          "tap far outside the panel reaches the game (kHitOutside)");
    check(ui.hitTest(c0.x + c0.w + CustomCapesUi::kGapX * 0.5f,
                     c0.y + c0.h + CustomCapesUi::kGapY * 0.5f) ==
              CustomCapesUi::kHitOutside,
          "tap in a card gap inside the panel is not consumed");
}

void testDrawCommands() {
    std::printf("draw commands: grid cards + glowing selection border\n");
    CustomCapesUi ui;
    ui.addEntry("None", "");
    ui.addEntry("a.png", "img-a");
    ui.addEntry("b.png", "img-b");
    ui.addEntry("c.png", "img-c");

    std::vector<PLModMenu_DrawCommand> cmds;
    ui.buildDrawCommands(cmds, 2);

    std::size_t images = 0, noneTexts = 0, goldStrips = 0, glowStrips = 0;
    for (const auto& cmd : cmds) {
        if (cmd.type == PL_DRAW_IMAGE) {
            ++images;
            check(!cmd.imageId.empty(), "image command carries its registered imageId");
        }
        if (cmd.type == PL_DRAW_TEXT && cmd.text == "None") ++noneTexts;
        if (cmd.type == PL_DRAW_RECT_FILLED &&
            cmd.color == CustomCapesUi::kSelectionBorderColor) ++goldStrips;
        if (cmd.type == PL_DRAW_RECT_FILLED &&
            cmd.color == CustomCapesUi::kSelectionGlowColor) ++glowStrips;
    }
    check(images == 3, "one Image command per cape entry");
    check(noneTexts == 1, "the None card is drawn as a labeled card");
    check(goldStrips == 4, "selected card has a 4-strip bright border");
    check(glowStrips == 4, "selected card has a 4-strip soft glow");

    // A selection outside the entry range draws no highlight at all.
    cmds.clear();
    ui.buildDrawCommands(cmds, 999);
    goldStrips = 0;
    glowStrips = 0;
    for (const auto& cmd : cmds) {
        if (cmd.type == PL_DRAW_RECT_FILLED &&
            cmd.color == CustomCapesUi::kSelectionBorderColor) ++goldStrips;
        if (cmd.type == PL_DRAW_RECT_FILLED &&
            cmd.color == CustomCapesUi::kSelectionGlowColor) ++glowStrips;
    }
    check(goldStrips == 0 && glowStrips == 0,
          "no highlight when the selected index matches no card");
}

void testCapeEntryRegistersPreview() {
    std::printf("texture loader binding: addCapeEntry registers a cropped RGBA bitmap\n");
    const std::vector<std::uint8_t> canvas = makeRegionCanvas();

    CustomCapesUi ui;
    check(ui.addCapeEntry("cape13.png", canvas.data()),
          "addCapeEntry accepts a resampled 64x32 canvas");
    check(ui.entryCount() == 1, "one grid entry was appended");
    check(!ui.entries()[0].imageId.empty(),
          "the appended entry carries the registered texture id");
    check(ui.entries()[0].fileName == "cape13.png",
          "entry keeps the original file name for its label");

    CustomCapesUi uiTwo;
    check(uiTwo.addCapeEntry("other.png", canvas.data()),
          "each entry gets its own registered texture id");
    check(uiTwo.entries()[0].imageId != ui.entries()[0].imageId,
          "texture ids are unique across entries");
}

} // namespace

int main() {
    testUvCrop();
    testGridLayoutAndHitTest();
    testDrawCommands();
    testCapeEntryRegistersPreview();

    std::printf("\n%s\n", g_failures == 0 ? "all custom capes UI tests passed"
                                          : "SOME UI TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}

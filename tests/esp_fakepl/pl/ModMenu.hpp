// Host-side fake of the Preloader <pl/ModMenu.hpp> header for the Esp render
// test. Unlike tests/fakepl (whose submitDrawCommands is an inline no-op), the
// entry points this module needs are only *declared* here so the test can
// define them and capture what the module submits:
//
//   * getHudSurfaceSize() - reports the surface the overlay is projected onto
//   * submitDrawCommands() - records the draw commands instead of drawing them
//
// The DrawCommand layout mirrors the real preloader header
// (LiteLDev/preloader-android include/pl/ModMenu.hpp) so production code
// compiles unchanged against this stub.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace pl::modmenu {

enum class DrawCommandType {
    Text,
    Rect,
    Line,
    RectFilled,
    CircleFilled,
    TriangleFilled,
    Image,
};

struct DrawCommand {
    DrawCommandType type{};
    float x{};
    float y{};
    float w{};
    float h{};
    float x3{};
    float y3{};
    std::uint32_t color{};
    float size{};
    std::string text;
    std::string fontId;
    std::string imageId;
};

struct HudSurfaceSize {
    float width{};
    float height{};
};

HudSurfaceSize getHudSurfaceSize();

void submitDrawCommands(std::string_view moduleId, std::span<const DrawCommand> commands);

// The module registers the packaged pixel font through core/PixelFont.hpp so it
// can measure the names it centers; the test only needs the call to exist and
// to report success (what it does with the answer is the point of the check in
// esp_render_test.cpp).
inline bool registerFont(std::string_view, std::span<const unsigned char>) {
    return true;
}

} // namespace pl::modmenu

// Host-side fake of the Preloader <pl/ModMenu.hpp> header for tests that
// include ModuleRegistry.hpp without the game. Only what the registry header
// and the host-tested modules need to parse is declared; nothing here is
// meant to be called. The DrawCommand layout and the image/texture
// registration API mirror the real preloader (LiteLDev/preloader-android
// include/pl/ModMenu.hpp) so the Custom Capes UI code can be built and
// unit-tested on the host.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pl::modmenu {

enum class ConfigType {
    Toggle,
    SliderInt,
    SliderFloat,
    Radio,
    Color,
    Keybind,
    Text,
    Button,
};

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
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    float x3 = 0.0f, y3 = 0.0f;
    std::uint32_t color{};
    float size{};
    std::string text;
    std::string fontId;
    std::string imageId;
};

// Mirrors the real header (span-based) so the ModuleRegistry.hpp wrapper —
// which is an exact-match overload for std::vector — stays unambiguous.
inline void submitDrawCommands(std::string_view, std::span<const DrawCommand>) {}

inline bool registerImage(std::string_view, std::span<const unsigned char>,
                          int, int) {
    return true;
}

inline bool registerFont(std::string_view, std::span<const unsigned char>) {
    return true;
}

} // namespace pl::modmenu

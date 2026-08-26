// Host-side fake of the Preloader <pl/ModMenu.hpp> header for tests that
// include ModuleRegistry.hpp without the game. Only what the registry
// header needs to parse is declared; nothing here is meant to be called.
#pragma once

#include <string_view>
#include <vector>

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
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

inline void submitDrawCommands(std::string_view, const std::vector<DrawCommand>&) {}

} // namespace pl::modmenu

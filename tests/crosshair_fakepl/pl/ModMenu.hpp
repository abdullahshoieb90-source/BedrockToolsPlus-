#pragma once
// Minimal host-side types for tests/crosshair_test.cpp.
//
// crosshair_test defines its own pl::modmenu stubs (so it can capture the
// last submitted draw-command list), therefore this header intentionally
// declares only the API types and does NOT define those functions.
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
    float x{};
    float y{};
    float w{};
    float h{};
    float x3{}; // corner radius for Rect/RectFilled
    float y3{}; // extra Y coordinate (e.g. pointer/wing tip)
    std::uint32_t color{};
    float size{}; // line thickness or text size
    std::string text;
    std::string fontId;
    std::string imageId;
};

struct ModuleInfo {};
struct ButtonInfo {
    std::string buttonId;
    std::string label;
};

} // namespace pl::modmenu

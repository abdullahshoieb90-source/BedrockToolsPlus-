// Host-side fake of the Preloader <pl/ModMenu.hpp> header for tests that
// include ModuleRegistry.hpp without the game. The struct layout mirrors
// the real preloader header (the one that ships in libpreloader.so on
// Android), so production code that targets the real header compiles
// unchanged against this stub. Nothing here is meant to be called — the
// only purpose is to let modules parse when compiled on a host without
// the LeviLauncher preloader.
#pragma once

#include <cstdint>
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
    float x3 = 0.0f;        // corner radius for Rect/RectFilled
    float size = 0.0f;      // line thickness or text size
    std::uint32_t color = 0xFFFFFFFFu;
    const char* text = nullptr;
    const char* imageId = nullptr;
    const char* fontId = nullptr;
};

inline void submitDrawCommands(std::string_view, const std::vector<DrawCommand>&) {}

inline void registerImage(const char*, const void*, int, int) {}
inline void registerFont(const char*, const std::vector<unsigned char>&) {}

} // namespace pl::modmenu

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

// Overlay button API, mirroring the builder calls the real header accepts
// (see the Zoom / Command Hotkey modules for the production usage).
enum class ButtonBehavior {
    Click,
    Toggle,
};

enum class ButtonEvent {
    Click,
    StateChanged,
};

enum class ButtonStylePreset {
    Accent,
};

class ButtonBuilder {
public:
    ButtonBuilder(std::string_view id, std::string_view label)
        : mId(id), mLabel(label) {}

    ButtonBuilder& moduleId(std::string_view id) { mModuleId = id; return *this; }
    ButtonBuilder& label(std::string_view label) { mLabel = label; return *this; }
    ButtonBuilder& behavior(ButtonBehavior behavior) { mBehavior = behavior; return *this; }
    ButtonBuilder& stylePreset(ButtonStylePreset preset) { mPreset = preset; return *this; }
    ButtonBuilder& styleColors(std::uint32_t, std::uint32_t, std::uint32_t) { return *this; }
    ButtonBuilder& svgIcon(const char*, bool = true) { return *this; }
    ButtonBuilder& activeSvgIcon(const char*) { return *this; }
    ButtonBuilder& textColor(std::uint32_t) { return *this; }
    ButtonBuilder& activeTextColor(std::uint32_t) { return *this; }
    ButtonBuilder& sizeScale(float, float) { return *this; }
    ButtonBuilder& defaultVisible(bool) { return *this; }

    template <class Handler>
    ButtonBuilder& onEvent(Handler&&) { return *this; }

    bool registerButton() { return true; }

private:
    std::string_view mId;
    std::string_view mLabel;
    std::string_view mModuleId;
    ButtonBehavior mBehavior = ButtonBehavior::Click;
    ButtonStylePreset mPreset = ButtonStylePreset::Accent;
};

inline void unregisterButton(std::string_view) {}

} // namespace pl::modmenu

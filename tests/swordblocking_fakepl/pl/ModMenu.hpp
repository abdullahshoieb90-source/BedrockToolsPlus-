// Host-side fake of the Preloader <pl/ModMenu.hpp> header for
// tests/swordblocking_test.cpp.
//
// Unlike tests/fakepl/pl/ModMenu.hpp (whose stubs are no-ops for modules that
// only have to parse), this one records what the module submits, so the test
// can assert on the overlay draw commands, the button definition and the hold
// callbacks. The struct layout and the builder surface mirror the real
// preloader header (LiteLDev/preloader-android include/pl/ModMenu.hpp).
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
    float x3{};
    float y3{};
    std::uint32_t color{};
    float size{};
    std::string text;
    std::string fontId;
    std::string imageId;
};

enum class ButtonBehavior {
    Click,
    Hold,
    Toggle,
};

enum class ButtonEvent {
    Click,
    Down,
    Up,
    StateChanged,
    Scroll,
};

enum class ButtonStylePreset {
    Keycap,
    Accent,
};

struct ModuleInfo {};

struct ButtonInfo {
    std::string buttonId;
    std::string moduleId;
    std::string displayName;
    std::string label;
    ButtonBehavior behavior{ButtonBehavior::Click};
    ButtonStylePreset stylePreset{ButtonStylePreset::Keycap};
    std::uint32_t normalBgColor{};
    std::uint32_t activeBgColor{};
    std::uint32_t borderColor{};
    std::uint32_t textColor{};
    std::uint32_t activeTextColor{};
    float widthScale{};
    float heightScale{1.0f};
    bool defaultVisible{true};
    bool hasIcon{};
    std::string icon;
    std::string activeIcon;
    std::function<void(std::string_view, ButtonEvent, float)> onEvent;
};

// ---- captured state --------------------------------------------------------

inline std::vector<DrawCommand> g_lastCommands;
inline std::vector<ButtonInfo> g_buttons;
inline int g_registerCalls = 0;
inline int g_unregisterCalls = 0;
inline std::string g_lastUnregisteredId;

// ---- stubs -----------------------------------------------------------------

inline bool registerModule(const ModuleInfo&) { return true; }
inline void unregisterModule(std::string_view) {}
inline void setModuleEnabled(std::string_view, bool) {}

inline void submitDrawCommands(std::string_view, std::span<const DrawCommand> commands) {
    g_lastCommands.assign(commands.begin(), commands.end());
}

inline bool registerFont(std::string_view, std::span<const unsigned char>) { return true; }
inline bool registerImage(std::string_view, std::span<const unsigned char>, int, int) { return true; }

inline bool g_rejectButtons = false;

inline bool registerButton(const ButtonInfo& info) {
    ++g_registerCalls;
    // The real launcher can refuse a button (unknown id, overlay disabled);
    // g_rejectButtons lets a test reproduce that answer.
    if (g_rejectButtons) return false;
    g_buttons.push_back(info);
    return true;
}

inline void unregisterButton(std::string_view id) {
    ++g_unregisterCalls;
    g_lastUnregisteredId = std::string(id);
    g_buttons.clear();
}

class ModuleBuilder {
public:
    ModuleBuilder(std::string_view, std::string_view) {}
    ModuleBuilder& description(std::string) { return *this; }
    ModuleBuilder& defaultEnabled(bool) { return *this; }
    ModuleBuilder& hideInHudEditor(bool = true) { return *this; }
    template <class Handler> ModuleBuilder& onToggle(Handler&&) { return *this; }
    template <class Handler> ModuleBuilder& onConfigChanged(Handler&&) { return *this; }
    template <class Handler> ModuleBuilder& onKeybind(Handler&&) { return *this; }
    ModuleBuilder& config(std::string, std::string, ConfigType, std::string = {},
                          std::string = {}, std::string = {}, std::string = {}) {
        return *this;
    }
    bool registerModule() const { return true; }
};

class ButtonBuilder {
public:
    ButtonBuilder(std::string_view id, std::string_view displayName)
        : mInfo{} {
        mInfo.buttonId = id;
        mInfo.displayName = displayName;
    }

    ButtonBuilder& moduleId(std::string_view id) { mInfo.moduleId = id; return *this; }
    ButtonBuilder& label(std::string_view value) { mInfo.label = value; return *this; }
    ButtonBuilder& behavior(ButtonBehavior value) { mInfo.behavior = value; return *this; }
    ButtonBuilder& defaultVisible(bool value) { mInfo.defaultVisible = value; return *this; }
    ButtonBuilder& stylePreset(ButtonStylePreset value) { mInfo.stylePreset = value; return *this; }
    ButtonBuilder& styleColors(std::uint32_t normal, std::uint32_t active,
                               std::uint32_t border = 0) {
        mInfo.normalBgColor = normal;
        mInfo.activeBgColor = active;
        mInfo.borderColor = border;
        return *this;
    }
    ButtonBuilder& textColor(std::uint32_t value) { mInfo.textColor = value; return *this; }
    ButtonBuilder& activeTextColor(std::uint32_t value) { mInfo.activeTextColor = value; return *this; }
    ButtonBuilder& sizeScale(float width, float height = 1.0f) {
        mInfo.widthScale = width;
        mInfo.heightScale = height;
        return *this;
    }
    ButtonBuilder& svgIcon(std::string_view svg, bool hideLabel = true) {
        mInfo.hasIcon = true;
        mInfo.icon = std::string(svg);
        (void)hideLabel;
        return *this;
    }
    ButtonBuilder& activeSvgIcon(std::string_view svg) {
        mInfo.activeIcon = std::string(svg);
        return *this;
    }
    template <class Handler>
    ButtonBuilder& onEvent(Handler&& handler) {
        mInfo.onEvent = std::forward<Handler>(handler);
        return *this;
    }
    bool registerButton() { return pl::modmenu::registerButton(mInfo); }

private:
    ButtonInfo mInfo;
};

} // namespace pl::modmenu

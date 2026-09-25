// Host-side fake of the Preloader <pl/ModMenu.hpp> header for tests that
// include ModuleRegistry.hpp without the game. The struct layout mirrors
// the real preloader header (the one that ships in libpreloader.so on
// Android), so production code that targets the real header compiles
// unchanged against this stub. The DrawCommand layout and the image/texture
// registration API mirror the real preloader (LiteLDev/preloader-android
// include/pl/ModMenu.hpp) so the Custom Capes UI code can be built and
// unit-tested on the host. Nothing here is meant to be called — the
// only purpose is to let modules parse when compiled on a host without
// the LeviLauncher preloader.
#pragma once

#include <cstdint>
#include <functional>
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

// Mirrors the real ConfigEntry/ModuleInfo so ModuleMenu.cpp — which turns a
// module's saved config into menu controls — compiles and runs on the host.
// registerModule() records what it was handed so tests can inspect the menu the
// launcher would have shown.
struct ConfigEntry {
    std::string key;
    std::string displayName;
    ConfigType type{};
    std::string defaultValue;
    std::string minValue;
    std::string maxValue;
    std::string dependsOn;
};

struct ModuleInfo {
    std::string moduleId;
    std::string displayName;
    std::string description;
    std::string modId;
    bool defaultEnabled{};
    bool hideInHudEditor{};
    std::vector<ConfigEntry> configs;
    std::function<void(std::string_view moduleId, bool enabled)> onToggle;
    std::function<void(std::string_view moduleId, std::string_view key,
                       std::string_view value)>
        onConfigChanged;
    std::function<void(std::string_view moduleId, std::string_view key, bool isDown)> onKeybind;
};

inline std::vector<ModuleInfo>& registeredModules() {
    static std::vector<ModuleInfo> modules;
    return modules;
}

inline bool registerModule(const ModuleInfo& info) {
    registeredModules().push_back(info);
    return true;
}

inline void unregisterModule(std::string_view moduleId) {
    auto& modules = registeredModules();
    for (auto it = modules.begin(); it != modules.end(); ++it) {
        if (it->moduleId == moduleId) {
            modules.erase(it);
            return;
        }
    }
}

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

// Fluent helper mirroring the real preloader's ModuleBuilder.
class ModuleBuilder {
public:
    ModuleBuilder(std::string moduleId, std::string displayName) {
        mInfo.moduleId = std::move(moduleId);
        mInfo.displayName = std::move(displayName);
    }

    ModuleBuilder& description(std::string value) {
        mInfo.description = std::move(value);
        return *this;
    }

    ModuleBuilder& modId(std::string value) {
        mInfo.modId = std::move(value);
        return *this;
    }

    ModuleBuilder& defaultEnabled(bool value) {
        mInfo.defaultEnabled = value;
        return *this;
    }

    ModuleBuilder& hideInHudEditor(bool value = true) {
        mInfo.hideInHudEditor = value;
        return *this;
    }

    ModuleBuilder& onToggle(std::function<void(std::string_view, bool)> callback) {
        mInfo.onToggle = std::move(callback);
        return *this;
    }

    ModuleBuilder& onConfigChanged(
        std::function<void(std::string_view, std::string_view, std::string_view)> callback) {
        mInfo.onConfigChanged = std::move(callback);
        return *this;
    }

    ModuleBuilder& onKeybind(std::function<void(std::string_view, std::string_view, bool)> callback) {
        mInfo.onKeybind = std::move(callback);
        return *this;
    }

    ModuleBuilder& config(std::string key, std::string displayName, ConfigType type,
                          std::string defaultValue = {}, std::string minValue = {},
                          std::string maxValue = {}, std::string dependsOn = {}) {
        mInfo.configs.push_back(ConfigEntry{std::move(key),
                                            std::move(displayName),
                                            type,
                                            std::move(defaultValue),
                                            std::move(minValue),
                                            std::move(maxValue),
                                            std::move(dependsOn)});
        return *this;
    }

    [[nodiscard]] bool registerModule() const { return pl::modmenu::registerModule(mInfo); }

private:
    ModuleInfo mInfo;
};

} // namespace pl::modmenu

// Host-side fake of the Preloader <pl/ModMenu.hpp> header for tests that
// include ModuleRegistry.hpp without the game. The struct layout mirrors
// the real preloader header (the one that ships in libpreloader.so on
// Android), so production code that targets the real header compiles
// unchanged against this stub. The DrawCommand layout, the image/texture
// registration API and the overlay-button builder mirror the real preloader
// (LiteLDev/preloader-android include/pl/ModMenu.hpp) so module code can be
// built and unit-tested on the host. Unlike the original stub, the overlay
// button / HUD editor calls record what they were given, so tests can assert
// on the values a module submits (see recordedButtons() and friends).
#pragma once

#include <cstdint>
#include <map>
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

// Mirrors the real header (span-based) so the ModuleRegistry.hpp wrapper —
// which is an exact-match overload for std::vector — stays unambiguous.
inline std::vector<DrawCommand>& recordedDrawCommands() {
    static std::vector<DrawCommand> commands;
    return commands;
}

inline void submitDrawCommands(std::string_view, std::span<const DrawCommand> commands) {
    recordedDrawCommands().assign(commands.begin(), commands.end());
}

inline bool registerImage(std::string_view, std::span<const unsigned char>,
                          int, int) {
    return true;
}

inline bool registerFont(std::string_view, std::span<const unsigned char>) {
    return true;
}

// ---- HUD editor elements ---------------------------------------------------

// Snap target flags, combined into HudEditorElement::snapFlags.
inline constexpr unsigned HudSnapNone = 0u;
inline constexpr unsigned HudSnapGrid = 1u << 0;
inline constexpr unsigned HudSnapElements = 1u << 1;
inline constexpr unsigned HudSnapScreenCenter = 1u << 2;

struct HudEditorElement {
    std::string elementId;
    std::string displayName;
    std::string positionKeyX;
    std::string positionKeyY;
    float x{};
    float y{};
    float width{};
    float height{};
    float gridSize{};
    float snapThreshold{};
    float gridGap{};
    unsigned snapFlags{};
};

inline std::vector<HudEditorElement>& recordedHudEditorElements() {
    static std::vector<HudEditorElement> elements;
    return elements;
}

inline void submitHudEditorElements(std::string_view, std::span<const HudEditorElement> elements) {
    recordedHudEditorElements().assign(elements.begin(), elements.end());
}

struct HudSurfaceSize {
    float width{};
    float height{};
};

inline HudSurfaceSize& hudSurfaceSize() {
    static HudSurfaceSize size;
    return size;
}

inline HudSurfaceSize getHudSurfaceSize() { return hudSurfaceSize(); }

// ---- Overlay buttons ---------------------------------------------------------

enum class ButtonBehavior {
    Click,
    Toggle,
    Hold,
};

enum class ButtonEvent {
    Click,
    StateChanged,
};

enum class ButtonStylePreset {
    Accent,
};

// Everything a registered overlay button was submitted with; tests assert on
// these to verify what a module asked the launcher to draw.
struct RecordedButton {
    std::string id;
    std::string displayName;
    std::string label;
    int androidKeyCode = 0;
    ButtonBehavior behavior = ButtonBehavior::Click;
    std::string svg;
    std::string activeSvg;
    std::uint32_t textColor = 0;
    std::uint32_t activeTextColor = 0;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    bool defaultVisible = false;
};

inline std::map<std::string, RecordedButton>& recordedButtons() {
    static std::map<std::string, RecordedButton> buttons;
    return buttons;
}

// Bumped on every register/unregister so tests can tell "registered again"
// apart from "left untouched".
inline int& buttonRegistryGeneration() {
    static int generation = 0;
    return generation;
}

class ButtonBuilder {
public:
    ButtonBuilder(std::string_view id, std::string_view label)
        : mRecorded() {
        mRecorded.id.assign(id.data(), id.size());
        mRecorded.displayName = mRecorded.id;
        mRecorded.label.assign(label.data(), label.size());
    }

    ButtonBuilder& moduleId(std::string_view id) { mModuleId.assign(id.data(), id.size()); return *this; }
    ButtonBuilder& label(std::string_view label) { mRecorded.label.assign(label.data(), label.size()); return *this; }
    ButtonBuilder& androidKeyCode(int keyCode) { mRecorded.androidKeyCode = keyCode; return *this; }
    ButtonBuilder& behavior(ButtonBehavior behavior) { mRecorded.behavior = behavior; return *this; }
    ButtonBuilder& stylePreset(ButtonStylePreset preset) { mPreset = preset; return *this; }
    ButtonBuilder& styleColors(std::uint32_t, std::uint32_t, std::uint32_t) { return *this; }
    ButtonBuilder& svgIcon(const char* svg, bool = true) { mRecorded.svg = svg ? svg : ""; return *this; }
    ButtonBuilder& activeSvgIcon(const char* svg) { mRecorded.activeSvg = svg ? svg : ""; return *this; }
    ButtonBuilder& textColor(std::uint32_t color) { mRecorded.textColor = color; return *this; }
    ButtonBuilder& activeTextColor(std::uint32_t color) { mRecorded.activeTextColor = color; return *this; }
    ButtonBuilder& sizeScale(float x, float y) { mRecorded.scaleX = x; mRecorded.scaleY = y; return *this; }
    ButtonBuilder& defaultVisible(bool visible) { mRecorded.defaultVisible = visible; return *this; }

    template <class Handler>
    ButtonBuilder& onEvent(Handler&&) { return *this; }

    bool registerButton() {
        recordedButtons()[mRecorded.id] = mRecorded;
        ++buttonRegistryGeneration();
        return true;
    }

private:
    RecordedButton mRecorded;
    std::string mModuleId;
    ButtonStylePreset mPreset = ButtonStylePreset::Accent;
};

inline void unregisterButton(std::string_view id) {
    const std::string key(id.data(), id.size());
    if (recordedButtons().erase(key) != 0) ++buttonRegistryGeneration();
}

} // namespace pl::modmenu

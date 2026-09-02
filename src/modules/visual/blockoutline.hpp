#pragma once

#include "../Module.hpp"
#include <cstdint>

class BlockOutlineModule final : public Module {
public:
    BlockOutlineModule();
    ~BlockOutlineModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& json) override;
    void saveConfig(nlohmann::json& json) override;

    // Stored as AARRGGBB with alpha forced opaque. The menu exposes this as a
    // single RGB color picker (saved as "#RRGGBB", like the Hitbox colors)
    // instead of the old Red/Green/Blue sliders. The legacy per-channel keys
    // are still accepted when loading older configs. Only used while `rgb`
    // (rainbow mode) is off.
    std::uint32_t outlineColor = 0xFFFFFFFFu;  // white by default

    // Opt-in menu toggle ("Show 3D"): draws the targeted block as a
    // translucent filled box (a true 3D volume) in addition to the wireframe
    // edges. Defaults to off, so the plain wireframe is what players get
    // unless they explicitly enable the 3D box. Only the faces pointing at
    // the camera are drawn, so the tint stays on the block's surface instead
    // of bleeding through to the inside.
    bool show3d = false;

    // Rainbow mode ("Rgb" menu toggle): while enabled, the outline color
    // cycles continuously through the RGB spectrum and overrides the static
    // `outlineColor` picker. The cycling color is applied to every pass
    // (hairline, thick edges and the 3D fill).
    bool rgb = false;

    // Line size (menu slider units). 1.0 keeps the classic hairline box;
    // anything above that is drawn as real geometry because GL line width is
    // ignored by most mobile GLES drivers. The wide frame is painted as flat
    // strips lying on the visible faces of the block (never as camera-facing
    // bars floating in space), so raising the size only makes the classic
    // wireframe bolder - it cannot turn into a 3D cube. The explicit
    // "Show 3D" toggle is the only thing that produces a 3D look.
    float lineThickness = 1.0f;

private:
    void installRenderHook();

    bool m_hookInstalled = false;
    void* m_renderLevel = nullptr;
};

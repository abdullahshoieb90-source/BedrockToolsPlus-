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
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Outline pass.
    bool outline = true;
    std::uint32_t outlineColor = 0xFFFFFFFFu; // AARRGGBB; config stores RGB.
    float outlineOpacity = 1.0f;
    float lineThickness = 2.0f;               // 1 = hairline, 10 = widest.

    // Optional translucent block/face overlay.
    bool fill = false;
    std::uint32_t fillColor = 0xFF33AAFFu;
    float fillOpacity = 0.22f;
    bool fillFaceOnly = false;

    // Shared effects for both passes.
    bool rainbow = false;
    float rainbowSpeed = 0.20f; // cycles per second, clamped to 0.05..1.
    bool pulse = false;
    float pulseSpeed = 0.75f;   // cycles per second, clamped to 0.05..1.

    // Uses a no-depth material when the game exposes one. Off by default so
    // the selected block remains naturally occluded by terrain.
    bool throughWalls = false;

private:
    bool m_patched = false;
    void* m_patchTarget = nullptr;

    void applyPatch();
};

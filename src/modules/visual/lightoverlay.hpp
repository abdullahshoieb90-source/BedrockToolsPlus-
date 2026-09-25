#pragma once

#include "../Module.hpp"

#include <cstdint>

// Writes the light level of every exposed block face around the player, so
// spots where hostile mobs can spawn are visible at a glance. Purely visual and
// client-side: nothing is read from or sent to the server.
class LightOverlayModule : public Module {
public:
    LightOverlayModule();
    ~LightOverlayModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Scan radius around the player, in blocks. The cost grows with the square
    // of the horizontal radius times the vertical one, so both are clamped.
    int radiusHorizontal = 16;
    int radiusVertical = 8;
    // Label the top face only instead of all six faces.
    bool onlyTopFace = false;
    // Measure from solid blocks only, or from every non-air block.
    bool onlySolidBlocks = true;

    // Stored as 0xAARRGGBB.
    std::uint32_t safeColor = 0xFF00FF00u;
    std::uint32_t dangerColor = 0xFFFF0000u;
    // Light levels at or below this are drawn with dangerColor.
    int dangerThreshold = 7;

private:
    void applyPatch();

    bool m_patched = false;
    void* m_patchTarget = nullptr;
};

#pragma once

#include "../Module.hpp"

#include <cstdint>

// Draws the 16x16 chunk column the player is standing in as a world-space
// wireframe, plus the corner posts of the chunks around it. Purely visual and
// client-side.
class ChunkBorderModule : public Module {
public:
    ChunkBorderModule();
    ~ChunkBorderModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Distance between two grid lines, in blocks. The names describe the axis
    // the lines are spaced along: rings step up Y, posts step along X and Z.
    // A spacing of 0 switches that family of lines off.
    float vertLineSpacing = 2.0f;
    int horizLineSpacing = 2;

    // Stored as 0xAARRGGBB.
    std::uint32_t cornerColor = 0xFF0000FFu;
    std::uint32_t midColor = 0xFF00FFFFu;
    std::uint32_t adjColor = 0xFFFF0000u;

private:
    void applyPatch();

    bool m_patched = false;
    void* m_patchTarget = nullptr;
};

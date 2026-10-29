#pragma once

#include "../Module.hpp"

#include <cstdint>

// ESP
//
// Draws nearby players, mobs and (optionally) item actors as a world-space
// overlay that - unlike the Hitbox module, which culls boxes hidden behind
// terrain - renders *through walls* by default. It is the "see everything"
// counterpart to Hitbox: same sorted actor fetch, same interpolated render
// boxes, same Android-safe thick-line geometry, but with a no-depth-test
// material and no occlusion culling.
//
// Per group (players / mobs / items) there is a toggle and a color, plus
// three combinable passes sharing that color:
//   * Box     - the 12 edges of the interpolated collision box.
//   * Fill    - a translucent 6-face shell inside the box.
//   * Tracers - a line from the camera to the center of the box.
// Invisible actors are shown by default (that is the point of an ESP);
// turning Through Walls off falls back to the depth-tested materials, so
// terrain occludes the overlay again without the manual voxel culling the
// Hitbox module performs.
//
// The renderer follows the Block Outline module's structure: a RenderLevel
// hook, tessellator geometry in camera-relative space, and materials taken
// from the game's own RenderMaterialGroup.
class ESPModule final : public Module {
public:
    ESPModule();
    ~ESPModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Scan radius of the per-frame actor fetch, in blocks.
    float espRange = 64.0f;

    bool showPlayers = true;
    std::uint32_t showPlayersColor = 0xFFFF5555u; // AARRGGBB
    bool showMobs = true;
    std::uint32_t showMobsColor = 0xFFFFAA00u;
    bool showItems = false;
    std::uint32_t showItemsColor = 0xFF55FFFFu;

    // Invisibility potions/effects do not hide actors from the overlay
    // unless this is turned off.
    bool showInvisible = true;

    bool box = true;
    float lineThickness = 2.0f; // 1 = hairline, higher = real geometry

    bool fill = false;
    float fillOpacity = 0.25f;

    bool tracers = false;

    // No-depth material: the overlay stays visible through terrain.
    bool throughWalls = true;

    // Draw boxes at the interpolated render position (see hitbox_interp).
    bool smoothBoxes = true;

private:
    bool m_patched = false;
    void* m_patchTarget = nullptr;

    void applyPatch();
};

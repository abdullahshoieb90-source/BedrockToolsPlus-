#pragma once

#include "../Module.hpp"
#include "entityesp_geometry.hpp"

#include <cstdint>

namespace entityesp = bedrocktools::modules::entityesp;

// Entity ESP (Hitbox / ESP 3D)
//
// Draws a wireframe hitbox around every nearby active entity:
//   * players and mobs (monsters, animals, water creatures, ...)
//   * dropped items (ActorType::Item)
//   * projectiles (ender pearls, wind charges, arrows, snowballs,
//     fireballs, primed TNT, ...)
//   * vehicles (boats, minecarts)
//
// Each category has its own enable toggle and color. Boxes are interpolated
// between the actor's previous and current position using the partial tick
// (0..1 within the current 50 ms tick) so they track the smooth render
// position instead of stepping at 20 tps.
//
// Rendering is hooked into LevelRenderer::renderLevel (the same world
// overlay path used by the Hitbox and Block Outline modules); the module's
// onRender method is the per-frame 3D entry point and draws through the
// game's ScreenContext tessellator, so the boxes inherit the active camera
// transform.
class EntityESPModule : public Module {
public:
    EntityESPModule();
    ~EntityESPModule() override;

    // Standard module pattern.
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

    // Per-frame 3D render entry point. Called from the renderLevel hook
    // with the ScreenContext and LevelRenderer the game is rendering with;
    // this is where the hitboxes are drawn through the tessellator.
    void onRender(void* screenContext, void* levelRenderer);

    void loadConfig(const nlohmann::json& json) override;
    void saveConfig(nlohmann::json& json) override;

    // ---- Per-category filtering (enable toggle + group color) ----
    // Stored as AARRGGBB; the menu exposes each as a single RGB picker
    // (alpha comes from boxAlpha).

    bool showPlayers = true;
    std::uint32_t showPlayersColor = 0xFFDDDDDDu;

    bool showMobs = true;
    std::uint32_t showMobsColor = 0xFF55FF55u;

    bool showItems = true;
    std::uint32_t showItemsColor = 0xFFFFAA00u;

    bool showProjectiles = true;
    std::uint32_t showProjectilesColor = 0xFFFF55FFu;

    bool showVehicles = true;
    std::uint32_t showVehiclesColor = 0xFF55AAFFu;

    // ---- Customization ----
    // Line size (menu slider units). 1.0 keeps the classic hairline box;
    // anything above that is drawn as real camera-facing quads, because GL
    // line width is ignored by most mobile GLES drivers.
    float lineThickness = 1.0f;

    // Box opacity (0..1). The menu color picker provides RGB only, so the
    // alpha channel is exposed separately for full RGBA control.
    float boxAlpha = 1.0f;

    // Draw boxes through solid blocks (true ESP). When off, boxes fully
    // hidden behind solid geometry are culled.
    bool throughWalls = true;

    // Interpolate between the actor's previous and current position using
    // the partial tick. When off, boxes snap with the 20 tps updates.
    bool interpolate = true;

    // Draw a box around the local player as well (third person only).
    bool showSelf = false;

    // How far around the local player entities are fetched, in blocks.
    float fetchRange = 64.0f;

private:
    bool enabledFor(entityesp::Group group) const;
    std::uint32_t colorFor(entityesp::Group group) const;

    void installRenderHook();

    bool m_hookInstalled = false;
    void* m_renderLevel = nullptr;
};

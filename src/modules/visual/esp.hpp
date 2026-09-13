#pragma once

#include "../Module.hpp"
#include <cstdint>

class EspModule;
extern EspModule* g_espMod;

// Esp
//
// Screen-space ESP drawn through the launcher HUD overlay. Every nearby
// actor is projected from world space to the HUD surface once per frame
// (camera position + local-player yaw/pitch + a configurable vertical FOV),
// and the module submits plain draw commands for the classic ESP feature
// set: 2D / corner boxes, translucent filled boxes, tracers (snaplines),
// nametags, health bars and distance readouts.
//
// Entity selection mirrors the Hitbox module: players, mobs and items can be
// toggled independently, invisible actors are skipped, and a wall-occlusion
// test (raycast through the BlockSource) can cull actors that are fully
// hidden behind solid blocks unless "Through Walls" is enabled.
//
// All of the heavy lifting happens in onFrame (published on the render
// thread right before eglSwapBuffers), so the projection uses the camera
// position the game actually rendered with, while the local-player pointer
// is handed over from LocalPlayerTickEvent through an atomic.
class EspModule : public Module {
public:
    // Radio option order is persisted by index, so new styles must only be
    // appended at the end. Labels live in esp.cpp (kBoxStyleNames /
    // kTracerOriginNames) and are guarded by static_asserts.
    enum class BoxStyle : int { Box2D = 0, Corner = 1, Count };
    enum class TracerOrigin : int { Bottom = 0, Crosshair = 1, Count };

    EspModule();
    ~EspModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // ---- Entity filters ----------------------------------------------------
    bool showPlayers = true;
    bool showMobs = true;
    bool showItems = true;
    // Draw the local player's own ESP while the camera is in third person.
    bool showLocalPlayer = false;

    // ---- Visibility --------------------------------------------------------
    // When false (default) actors fully hidden behind solid blocks are culled;
    // when true every fetched actor is drawn regardless of occlusion.
    bool throughWalls = false;
    float range = 64.0f; // fetch radius in blocks (8 .. 256)

    // ---- Box ---------------------------------------------------------------
    bool box = true;
    BoxStyle boxStyle = BoxStyle::Box2D;
    bool boxFilled = false;
    uint32_t boxColor = 0xFFFFFFFF;
    uint32_t boxFilledColor = 0xFFFFFFFF;
    float boxFilledOpacity = 0.25f;
    float boxThickness = 1.5f;
    bool rgb = false;      // animated rainbow box color (overrides boxColor)
    float rgbSpeed = 0.3f; // full hue cycles per second (0.05 .. 1)

    // ---- Tracers -----------------------------------------------------------
    bool tracer = false;
    TracerOrigin tracerOrigin = TracerOrigin::Bottom;
    uint32_t tracerColor = 0xFFFFFFFF;

    // ---- Nametag -----------------------------------------------------------
    bool nametag = true;
    uint32_t nametagColor = 0xFFFFFFFF;
    float nametagScale = 1.0f; // 0.5 .. 2 (multiplier on the base 14px text)

    // ---- Health / Distance ---------------------------------------------------
    bool health = true;
    bool distance = true;

    // ---- Projection ----------------------------------------------------------
    float fov = 60.0f; // vertical field of view in degrees (30 .. 120)

private:
    bool m_perspectiveHooked = false;
};

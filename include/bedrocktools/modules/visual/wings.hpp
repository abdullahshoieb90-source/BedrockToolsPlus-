#pragma once

#include "modules/Module.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Wings - world-space overlay version
//
// The previous implementation patched SerializedSkinImpl (mSkinImage,
// mGeometryData, mDefaultGeometryName) to inject a custom geometry. Bedrock
// has removed support for custom geometry on classic skins, so that approach
// made the player disappear when the module was enabled.
//
// This version draws the wings as a world-space overlay attached to the local
// player via a RenderLevel hook + tessellator, instead of touching the skin at
// all. The flap animation is preserved as:
//
//   angle = amplitude * sin(flapTime * baseRate * flapSpeed)
//
// where amplitude = 35 degrees, baseRate = 6 rad/s at flapSpeed == 1.0.
// flapSpeed is exposed as "Flap Speed" in the launcher menu in [0.1, 10.0].
//
// Rendering:
//   * Hook RenderLevel (same pattern as Hitbox/Breadcrumbs/BlockOutline)
//   * Track local player AABB, yaw, and flap phase from LocalPlayerTick
//   * In the hook, compute two quads (right/left wing) attached to the back,
//     rotated around Z by +/- flapAngle, then yaw-rotated to face with the
//     player, and rendered with Tessellator + selection_box material.
//
// The module never touches skin memory, so it cannot make the player vanish.
class WingsModule : public Module {
public:
    WingsModule();
    ~WingsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from the LocalPlayerTickEvent subscription.
    void onLocalPlayerTick(void* player);

    // Advances the flap clock by dt seconds. Called automatically from the
    // tick with real elapsed time; also public so host tests can drive a
    // deterministic clock.
    void advanceFlapAnimation(float dtSeconds);

    // Directory the module watches; kept for menu description compatibility.
    const std::string& wingsDirectory() const { return m_wingsDir; }

    // Flap speed multiplier shown in the launcher menu (0.1 = slow,
    // 10 = very fast, 1.0 = default).
    float m_flapSpeed = 1.0f;

    // Test / rendering helpers
    float flapTime() const { return m_flapTime; }
    float currentFlapAngleDegrees() const;
    float currentFlapAngleRadians() const;

    // Constants exposed for tests
    static constexpr float kFlapAmplitudeDegrees = 35.0f;
    static constexpr float kFlapBaseRate = 6.0f; // rad/s at speed 1.0
    static constexpr float kWingWidth = 0.5f;    // blocks
    static constexpr float kWingHeight = 0.7f;   // blocks

private:
    void applyPatch();

    std::string m_wingsDir;

    // Animation state (clock driven per tick).
    float m_flapTime = 0.0f;
    bool m_flapClockStarted = false;
    std::chrono::steady_clock::time_point m_lastFlapTick;

    // Render hook state
    bool m_patched = false;
    void* m_patchTarget = nullptr;
    void* m_tessBeginAddr = nullptr;
    void* m_tessColorAddr = nullptr;
    void* m_tessVertexAddr = nullptr;
    void* m_renderMaterialGroupAddr = nullptr;
    void* m_renderMeshAddr = nullptr;
    void* m_renderMesh2Addr = nullptr;
};

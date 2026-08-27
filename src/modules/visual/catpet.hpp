#pragma once

#include "modules/Module.hpp"
#include "modules/visual/catpet_shape.hpp"

#include <chrono>
#include <mutex>
#include <string>

// Cat Pet - a chibi voxel cat that follows you around.
//
// The cat is drawn as a world-space overlay via the same RenderLevel +
// tessellator pattern the Wings module uses; it never touches skins, actors
// or server state and is fully client-side.
//
// Follow behaviour (tick thread): the cat chases a "heel spot" behind-left of
// the local player (catpet::heelTarget). It walks when it is a little behind,
// sprints when far, stops inside a small deadzone, teleports if left more
// than ~12 blocks behind, and turns to look at its owner while resting
// (catpet::stepCatFollow).
//
// Animation (catpet::computeCatPose, all continuous blends):
//   - trot/run cycle: diagonal leg pairs, gallop bounce, body pitch/roll,
//     stride frequency follows the chase speed
//   - idle: breathing bob, slow tail S-sway, look-around, head tilt,
//     occasional one-ear twitches and blinks
//   - sit: after a few idle seconds the cat sits (chest up, hind legs
//     tucked, tail wrapped with a lazy tip flick); stands instantly on move
//
// Rendering interpolates the tick-sampled position/yaw by the partial-tick
// fraction and extrapolates the animation clocks, exactly like the Wings
// module, so the cat stays smooth at any frame rate.
class CatPetModule : public Module {
public:
    CatPetModule();
    ~CatPetModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from the LocalPlayerTickEvent subscription.
    void onLocalPlayerTick(void* player);

    // Advances the animation clocks by dtSeconds given the cat's current
    // chase speed (blocks/s). Public so host tests can drive a deterministic
    // clock.
    void advanceCatAnimation(float dtSeconds, float chaseSpeed);

    // Current pose from the tick clocks (host tests) and the render-thread
    // version that extrapolates by the time since the last tick.
    bedrocktools::modules::catpet::CatPose currentPose() const;
    bedrocktools::modules::catpet::CatPose currentPoseInterpolated() const;

    // Test helpers.
    float moveBlend() const { return m_moveBlend; }
    float sitBlend() const { return m_sitBlend; }
    float idleTime() const { return m_idleTime; }

    // --- Settings shown in the launcher menu ---
    // Pet size multiplier; 1.0 is a one-block-tall-ish kitten, the default is
    // intentionally big and huggable.
    float m_scale = 1.75f;
    // Coat picker (radio). m_catStyle is the serialized id, m_catStyleIndex
    // the resolved table index the render thread reads.
    std::string m_catStyle = "orange";
    int m_catStyleIndex = 0;
    // Sit down after a few seconds of standing still.
    bool m_sitWhenIdle = true;

    // Tuning constants.
    static constexpr float kMoveBlendFullSpeed = 3.2f;   // blocks/s => move = 1
    static constexpr float kMoveAttackRate = 14.0f;      // 1/s blend up
    static constexpr float kMoveDecayRate = 8.0f;        // 1/s blend down
    static constexpr float kSitDelaySeconds = 3.5f;      // idle time before sitting
    static constexpr float kSitAttackRate = 2.2f;        // 1/s ease into the sit
    static constexpr float kSitDecayRate = 10.0f;        // 1/s jump up when moving
    static constexpr float kIdleSpeedThreshold = 0.15f;  // blocks/s counts as idle

private:
    void applyPatch();

    // Animation state (advanced per tick, read interpolated by the render
    // hook on another thread).
    mutable std::mutex m_animationMutex;
    float m_animTime = 0.0f;      // idle clock (seconds)
    float m_stridePhase = 0.0f;   // accumulated walk phase (radians)
    float m_strideRate = 0.0f;    // rad/s at the last tick (for extrapolation)
    float m_moveBlend = 0.0f;     // smoothed 0..1 stand->run
    float m_sitBlend = 0.0f;      // smoothed 0..1 stand->sit
    float m_idleTime = 0.0f;      // consecutive idle seconds
    bool m_clockStarted = false;
    std::chrono::steady_clock::time_point m_lastTick;

    // Render hook state.
    bool m_patched = false;
    void* m_patchTarget = nullptr;
};

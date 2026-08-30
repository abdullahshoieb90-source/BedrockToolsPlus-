#pragma once

#include "../Module.hpp"
#include "freelook_logic.hpp"

#include <bedrocktools/sdk/Types.hpp>

#include <chrono>
#include <cstdint>

class FreeLookModule : public Module {
public:
    // Menu settings.
    bool  m_holdMode = true;      // true: Free Look while the keybind is held
    bool  m_smoothReturn = true;  // animate the camera home on release
    float m_returnSpeed = 0.45f;  // per-tick (20 tps) return lerp factor
    int   m_maxYaw = 180;         // camera yaw swing limit in degrees
    int   m_maxPitch = 90;        // camera pitch swing limit in degrees
    bool  m_overlayToggle = true; // show the Free Look overlay button

    FreeLookModule();
    ~FreeLookModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onKeybindEvent(const std::string& key, bool isDown) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void updateOverlayButton();

    // Entry points used by the hook and event plumbing in freelook.cpp.
    // All of them run on the game thread.

    // The whole body of the LocalPlayer::applyTurnDelta detour: decides what
    // the camera is allowed to see of `delta` and books it into the swing.
    // Returns the delta to hand to the original function — for a turn that is
    // none of Free Look's business that is `delta` itself, unchanged.
    bedrocktools::sdk::Vec2 filterTurnDelta(void* player, const bedrocktools::sdk::Vec2& delta);
    // True while turn deltas for `player` are Free Look's business.
    bool shouldInterceptTurn(void* player) const;
    void onPreTick(void* player);
    void onPostTick(void* player);

    void* player() const { return m_player; }
    const freelook::Angles& lockedAngles() const { return m_core.locked(); }
    // How far the camera currently sits from the locked body angle.
    const freelook::Turn& cameraSwing() const { return m_core.swing(); }

    // Compensating deltas fed back into the camera during the release.
    // Recorded because the camera angle itself is write-only from here (the
    // game owns it); the host tests assert on this instead of on the game.
    const freelook::Turn& lastCameraDelta() const { return m_lastCameraDelta; }
    int cameraDeltaCount() const { return m_cameraDeltaCount; }
    void resetCameraDeltaLog() {
        m_lastCameraDelta = freelook::Turn{};
        m_cameraDeltaCount = 0;
    }

private:
    void syncRequestActive();
    void handlePlayerChanged(void* player);
    void pushSettings();
    // Hands one delta to the game's own applyTurnDelta, bypassing our hook.
    void sendCameraDelta(void* player, const freelook::Turn& delta);
    // Forces the movement input scheme to the player-relative one while the
    // camera is decoupled from the body (`directing`), restoring the saved
    // scheme once it isn't. Null-safe: ticks without a move input component
    // (host tests, teardown) are simply skipped.
    void applyMovementLock(void* player, bool directing);

    void* m_player = nullptr;
    bool m_keyHeld = false;      // raw keybind down state (hold mode)
    bool m_toggled = false;      // toggle-mode state
    bool m_buttonActive = false; // overlay button state
    bool m_turnHooked = false;
    std::chrono::steady_clock::time_point m_lastTick{};
    bool m_lastTickValid = false;
    freelook::Turn m_lastCameraDelta{};
    int m_cameraDeltaCount = 0;
    freelook::Core m_core;
    // Keeps the movement input player-relative while the camera is being
    // steered away from the body.
    freelook::MovementFrameLock m_movementLock;
};

#pragma once

#include "../Module.hpp"
#include "freelook_logic.hpp"

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
    bool shouldRedirectTurn(void* player) const;
    bool useMeasuredTurns() const { return m_core.useMeasuredTurns(); }
    void onTurnObserved(void* player, float argX, float argY, float dPitch, float dYaw);
    void onTurnMeasured(float argX, float argY, float dPitch, float dYaw);
    void onTurnRaw(float argX, float argY);
    void onPreTick(void* player);
    void onPostTick(void* player);

    void* player() const { return m_player; }
    const freelook::Angles& lockedAngles() const { return m_core.locked(); }

private:
    void syncRequestActive();
    void handlePlayerChanged(void* player);
    void restoreLockedRotation();
    void pushSettings();

    void* m_player = nullptr;
    bool m_keyHeld = false;      // raw keybind down state (hold mode)
    bool m_toggled = false;      // toggle-mode state
    bool m_buttonActive = false; // overlay button state
    bool m_turnHooked = false;
    std::chrono::steady_clock::time_point m_lastTick{};
    bool m_lastTickValid = false;
    freelook::Core m_core;
};

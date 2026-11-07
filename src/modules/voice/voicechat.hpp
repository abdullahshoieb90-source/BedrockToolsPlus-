#pragma once

#include "../Module.hpp"
#include "voicechat_engine.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

class VoiceChatModule final : public Module {
public:
    VoiceChatModule();
    ~VoiceChatModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    bool onKeyEvent(int key, bool isDown) override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    bedrocktools::voice::VoiceChatEngine m_engine;

    mutable std::mutex m_configMutex;
    std::string m_server;
    std::string m_room = "friends";
    std::string m_roomKey;
    std::string m_playerName; // empty = use the current Minecraft name
    std::string m_gameName;
    int m_mode = 0; // 0 = Room, 1 = Proximity
    bool m_pushToTalk = true;
    int m_talkKeybind = 50; // Android KEYCODE_V
    float m_maxDistance = 32.0f;
    float m_inputVolume = 1.0f;
    float m_outputVolume = 1.0f;
    bool m_showStatus = true;
    float m_hudPosX = 12.0f;
    float m_hudPosY = 12.0f;

    std::atomic_bool m_keyboardTalk{false};
    std::atomic_bool m_buttonTalk{false};
    bool m_restartPending = false;
    std::chrono::steady_clock::time_point m_restartAt{};

    bedrocktools::voice::VoiceChatSettings settingsSnapshot();
    void scheduleSettingsUpdate();
    void updateTalkState();
    void syncTalkButton();
    void clearOverlay();
};

extern VoiceChatModule* g_voiceChat;

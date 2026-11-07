#pragma once

#include "voicechat_audio.hpp"
#include "voicechat_protocol.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace bedrocktools::voice {

enum class VoiceMode {
    Room,
    Proximity,
};

enum class ConnectionState {
    Disabled,
    Connecting,
    Connected,
    Error,
};

struct VoiceChatSettings {
    std::string server;
    std::uint16_t port = 39741;
    std::string room = "friends";
    std::string roomKey;
    std::string playerName = "Player";
    VoiceMode mode = VoiceMode::Room;
    float maxDistance = 32.0f;
    float inputVolume = 1.0f;
    float outputVolume = 1.0f;
};

struct VoiceChatStatus {
    ConnectionState state = ConnectionState::Disabled;
    std::string message = "Voice chat is disabled";
    std::uint16_t peerCount = 0; // includes the local player once connected
    std::uint16_t activeSpeakers = 0;
};

// Combines the encrypted UDP relay client, bounded per-speaker jitter buffers,
// PCM mixing and Android audio. All public methods are thread-safe; game/UI
// callbacks never perform DNS, blocking network I/O or audio-device setup.
class VoiceChatEngine {
public:
    VoiceChatEngine();
    ~VoiceChatEngine();

    VoiceChatEngine(const VoiceChatEngine&) = delete;
    VoiceChatEngine& operator=(const VoiceChatEngine&) = delete;

    void start(const VoiceChatSettings& settings, void* javaVm);
    void stop();
    void updateSettings(const VoiceChatSettings& settings);

    void setLocalPosition(Position position);
    void setTalkActive(bool active);

    VoiceChatStatus status() const;
    bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace bedrocktools::voice

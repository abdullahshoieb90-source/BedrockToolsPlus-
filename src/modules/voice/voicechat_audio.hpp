#pragma once

#include "voicechat_protocol.hpp"

#include <functional>
#include <memory>
#include <string>

namespace bedrocktools::voice {

// Owns the Android AudioRecord and AudioTrack worker threads. No microphone
// object exists while capture is inactive, which makes push-to-talk release
// the microphone instead of merely discarding recorded frames.
class AudioEngine {
public:
    using CaptureCallback = std::function<void(const PcmFrame&)>;
    using MixCallback = std::function<void(PcmFrame&)>;

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool start(void* javaVm, CaptureCallback capture, MixCallback mix);
    void stop();

    void setCaptureActive(bool active);
    void setInputGain(float gain);

    bool running() const;
    bool captureActive() const;
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace bedrocktools::voice

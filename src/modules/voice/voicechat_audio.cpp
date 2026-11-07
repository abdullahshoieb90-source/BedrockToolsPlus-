#include "voicechat_audio.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace bedrocktools::voice {

struct AudioEngine::Impl {
    std::atomic_bool running{false};
    std::atomic_bool capturing{false};
    std::atomic<float> inputGain{1.0f};

    mutable std::mutex stateMutex;
    std::condition_variable wake;
    CaptureCallback captureCallback;
    MixCallback mixCallback;
    std::thread captureThread;
    std::thread playbackThread;
    void* javaVm = nullptr;

    mutable std::mutex errorMutex;
    std::string captureError;
    std::string playbackError;

    void setError(std::string& target, std::string value) {
        std::lock_guard lock(errorMutex);
        target = std::move(value);
    }

    std::string combinedError() const {
        std::lock_guard lock(errorMutex);
        if (!captureError.empty() && !playbackError.empty())
            return captureError + "; " + playbackError;
        return !captureError.empty() ? captureError : playbackError;
    }

#if defined(__ANDROID__)
    class ScopedJniEnv {
    public:
        explicit ScopedJniEnv(JavaVM* vm) : m_vm(vm) {
            if (!vm) return;
            const jint state = vm->GetEnv(reinterpret_cast<void**>(&m_env), JNI_VERSION_1_6);
            if (state == JNI_OK && m_env) return;
            if (state != JNI_EDETACHED) return;
            JavaVMAttachArgs args{JNI_VERSION_1_6,
                                  const_cast<char*>("BedrockToolsPlus/Voice"), nullptr};
            if (vm->AttachCurrentThread(&m_env, &args) == JNI_OK && m_env) m_attached = true;
        }

        ~ScopedJniEnv() {
            if (m_attached && m_vm) m_vm->DetachCurrentThread();
        }

        JNIEnv* get() const { return m_env; }

    private:
        JavaVM* m_vm = nullptr;
        JNIEnv* m_env = nullptr;
        bool m_attached = false;
    };

    static bool clearException(JNIEnv* env) {
        if (!env || !env->ExceptionCheck()) return false;
        env->ExceptionClear();
        return true;
    }

    static void applyInputGain(PcmFrame& frame, float gain) {
        gain = std::clamp(gain, 0.0f, 2.0f);
        if (gain == 1.0f) return;
        for (auto& sample : frame) {
            const long value = std::lround(static_cast<float>(sample) * gain);
            sample = static_cast<std::int16_t>(std::clamp(value, -32768L, 32767L));
        }
    }

    void waitForCaptureRetry(std::chrono::milliseconds delay = std::chrono::seconds(1)) {
        std::unique_lock lock(stateMutex);
        wake.wait_for(lock, delay, [&] {
            return !running.load(std::memory_order_acquire) ||
                   !capturing.load(std::memory_order_acquire);
        });
    }

    void captureLoop() {
        ScopedJniEnv jni(static_cast<JavaVM*>(javaVm));
        JNIEnv* env = jni.get();
        if (!env) {
            setError(captureError, "Cannot attach the microphone thread to Android");
            return;
        }

        while (running.load(std::memory_order_acquire)) {
            {
                std::unique_lock lock(stateMutex);
                wake.wait(lock, [&] {
                    return !running.load(std::memory_order_acquire) ||
                           capturing.load(std::memory_order_acquire);
                });
            }
            if (!running.load(std::memory_order_acquire)) break;
            if (!capturing.load(std::memory_order_acquire)) continue;

            jclass cls = env->FindClass("android/media/AudioRecord");
            if (!cls || clearException(env)) {
                setError(captureError, "Android AudioRecord is unavailable");
                break;
            }
            jmethodID init = env->GetMethodID(cls, "<init>", "(IIIII)V");
            jmethodID startRecording = env->GetMethodID(cls, "startRecording", "()V");
            jmethodID stopRecording = env->GetMethodID(cls, "stop", "()V");
            jmethodID release = env->GetMethodID(cls, "release", "()V");
            jmethodID getState = env->GetMethodID(cls, "getState", "()I");
            jmethodID read = env->GetMethodID(cls, "read", "([SII)I");
            jmethodID minBuffer = env->GetStaticMethodID(cls, "getMinBufferSize", "(III)I");
            if (clearException(env) || !init || !startRecording || !stopRecording || !release ||
                !getState || !read || !minBuffer) {
                env->DeleteLocalRef(cls);
                setError(captureError, "Android AudioRecord methods are unavailable");
                break;
            }

            // VOICE_COMMUNICATION enables the device's hardware echo
            // cancellation/noise suppression when available. MONO + PCM16 at
            // 16 kHz matches the wire format without resampling.
            constexpr jint voiceCommunication = 7;
            constexpr jint channelInMono = 16;
            constexpr jint pcm16 = 2;
            jint bufferBytes = env->CallStaticIntMethod(cls, minBuffer,
                static_cast<jint>(kSampleRate), channelInMono, pcm16);
            if (clearException(env) || bufferBytes <= 0)
                bufferBytes = static_cast<jint>(kPcmBytesPerFrame * 4);
            else
                bufferBytes = std::max<jint>(bufferBytes * 2,
                                             static_cast<jint>(kPcmBytesPerFrame * 4));

            jobject recorder = env->NewObject(cls, init, voiceCommunication,
                static_cast<jint>(kSampleRate), channelInMono, pcm16, bufferBytes);
            if (!recorder || clearException(env)) {
                env->DeleteLocalRef(cls);
                setError(captureError,
                    "Microphone permission is off. Enable it for LeviLauncher in Android settings.");
                waitForCaptureRetry();
                continue;
            }

            const jint state = env->CallIntMethod(recorder, getState);
            if (clearException(env) || state != 1) { // STATE_INITIALIZED
                env->CallVoidMethod(recorder, release);
                clearException(env);
                env->DeleteLocalRef(recorder);
                env->DeleteLocalRef(cls);
                setError(captureError,
                    "Microphone unavailable. Grant microphone permission to LeviLauncher.");
                waitForCaptureRetry();
                continue;
            }

            jshortArray javaSamples = env->NewShortArray(static_cast<jsize>(kSamplesPerFrame));
            if (!javaSamples || clearException(env)) {
                env->CallVoidMethod(recorder, release);
                clearException(env);
                env->DeleteLocalRef(recorder);
                env->DeleteLocalRef(cls);
                setError(captureError, "Cannot allocate the microphone audio buffer");
                break;
            }

            env->CallVoidMethod(recorder, startRecording);
            if (clearException(env)) {
                env->CallVoidMethod(recorder, release);
                clearException(env);
                env->DeleteLocalRef(javaSamples);
                env->DeleteLocalRef(recorder);
                env->DeleteLocalRef(cls);
                setError(captureError,
                    "Microphone permission is off. Enable it for LeviLauncher in Android settings.");
                waitForCaptureRetry();
                continue;
            }
            setError(captureError, "");

            while (running.load(std::memory_order_acquire) &&
                   capturing.load(std::memory_order_acquire)) {
                const jint count = env->CallIntMethod(recorder, read, javaSamples, 0,
                                                      static_cast<jint>(kSamplesPerFrame));
                if (clearException(env)) break;
                if (count == static_cast<jint>(kSamplesPerFrame)) {
                    // The user may have released Talk while read() was blocked;
                    // discard that final frame instead of sending extra audio.
                    if (!capturing.load(std::memory_order_acquire) ||
                        !running.load(std::memory_order_acquire)) {
                        break;
                    }
                    PcmFrame frame{};
                    env->GetShortArrayRegion(javaSamples, 0,
                        static_cast<jsize>(frame.size()), frame.data());
                    if (clearException(env)) break;
                    applyInputGain(frame, inputGain.load(std::memory_order_relaxed));
                    if (captureCallback) captureCallback(frame);
                } else if (count < 0) {
                    // ERROR_DEAD_OBJECT and friends: recreate AudioRecord on
                    // the next activation instead of spinning on a dead one.
                    break;
                }
            }

            env->CallVoidMethod(recorder, stopRecording);
            clearException(env);
            env->CallVoidMethod(recorder, release);
            clearException(env);
            env->DeleteLocalRef(javaSamples);
            env->DeleteLocalRef(recorder);
            env->DeleteLocalRef(cls);
            if (capturing.load(std::memory_order_acquire) &&
                running.load(std::memory_order_acquire)) {
                // A dead audio device or read error must not cause a tight
                // create/read/fail loop while the user keeps Talk held down.
                waitForCaptureRetry(std::chrono::milliseconds(250));
            }
        }
    }

    void playbackLoop() {
        ScopedJniEnv jni(static_cast<JavaVM*>(javaVm));
        JNIEnv* env = jni.get();
        if (!env) {
            setError(playbackError, "Cannot attach the voice output thread to Android");
            return;
        }

        jclass cls = env->FindClass("android/media/AudioTrack");
        if (!cls || clearException(env)) {
            setError(playbackError, "Android AudioTrack is unavailable");
            return;
        }
        jmethodID init = env->GetMethodID(cls, "<init>", "(IIIIII)V");
        jmethodID play = env->GetMethodID(cls, "play", "()V");
        jmethodID stopTrack = env->GetMethodID(cls, "stop", "()V");
        jmethodID release = env->GetMethodID(cls, "release", "()V");
        jmethodID getState = env->GetMethodID(cls, "getState", "()I");
        jmethodID write = env->GetMethodID(cls, "write", "([SII)I");
        jmethodID minBuffer = env->GetStaticMethodID(cls, "getMinBufferSize", "(III)I");
        if (clearException(env) || !init || !play || !stopTrack || !release || !getState ||
            !write || !minBuffer) {
            env->DeleteLocalRef(cls);
            setError(playbackError, "Android AudioTrack methods are unavailable");
            return;
        }

        constexpr jint streamMusic = 3;
        constexpr jint channelOutMono = 4;
        constexpr jint pcm16 = 2;
        constexpr jint modeStream = 1;
        jint bufferBytes = env->CallStaticIntMethod(cls, minBuffer,
            static_cast<jint>(kSampleRate), channelOutMono, pcm16);
        if (clearException(env) || bufferBytes <= 0)
            bufferBytes = static_cast<jint>(kPcmBytesPerFrame * 4);
        else
            bufferBytes = std::max<jint>(bufferBytes * 2,
                                         static_cast<jint>(kPcmBytesPerFrame * 4));

        jobject track = env->NewObject(cls, init, streamMusic, static_cast<jint>(kSampleRate),
                                       channelOutMono, pcm16, bufferBytes, modeStream);
        jshortArray javaSamples = track
            ? env->NewShortArray(static_cast<jsize>(kSamplesPerFrame)) : nullptr;
        if (!track || !javaSamples || clearException(env)) {
            if (track) env->DeleteLocalRef(track);
            if (javaSamples) env->DeleteLocalRef(javaSamples);
            env->DeleteLocalRef(cls);
            setError(playbackError, "Cannot initialize Android voice playback");
            return;
        }

        const jint trackState = env->CallIntMethod(track, getState);
        if (clearException(env) || trackState != 1) { // STATE_INITIALIZED
            env->CallVoidMethod(track, release);
            clearException(env);
            env->DeleteLocalRef(javaSamples);
            env->DeleteLocalRef(track);
            env->DeleteLocalRef(cls);
            setError(playbackError, "Android voice output is uninitialized");
            return;
        }

        env->CallVoidMethod(track, play);
        if (clearException(env)) {
            env->CallVoidMethod(track, release);
            clearException(env);
            env->DeleteLocalRef(javaSamples);
            env->DeleteLocalRef(track);
            env->DeleteLocalRef(cls);
            setError(playbackError, "Cannot start Android voice playback");
            return;
        }
        setError(playbackError, "");

        using clock = std::chrono::steady_clock;
        auto nextFrame = clock::now();
        while (running.load(std::memory_order_acquire)) {
            PcmFrame mixed{};
            if (mixCallback) mixCallback(mixed);
            env->SetShortArrayRegion(javaSamples, 0, static_cast<jsize>(mixed.size()),
                                     mixed.data());
            if (clearException(env)) break;
            const jint written = env->CallIntMethod(track, write, javaSamples, 0,
                                                    static_cast<jint>(mixed.size()));
            if (clearException(env) || written < 0) break;
            nextFrame += std::chrono::milliseconds(20);
            std::unique_lock lock(stateMutex);
            wake.wait_until(lock, nextFrame, [&] {
                return !running.load(std::memory_order_acquire);
            });
            if (nextFrame < clock::now() - std::chrono::milliseconds(100))
                nextFrame = clock::now();
        }

        env->CallVoidMethod(track, stopTrack);
        clearException(env);
        env->CallVoidMethod(track, release);
        clearException(env);
        env->DeleteLocalRef(javaSamples);
        env->DeleteLocalRef(track);
        env->DeleteLocalRef(cls);
        if (running.load(std::memory_order_acquire))
            setError(playbackError, "Android voice playback stopped unexpectedly");
    }
#endif
};

AudioEngine::AudioEngine() : m_impl(std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() { stop(); }

bool AudioEngine::start(void* javaVm, CaptureCallback capture, MixCallback mix) {
    if (m_impl->running.load(std::memory_order_acquire)) return true;
#if defined(__ANDROID__)
    if (!javaVm) {
        m_impl->setError(m_impl->captureError, "Android JavaVM is unavailable");
        return false;
    }
    m_impl->javaVm = javaVm;
    m_impl->captureCallback = std::move(capture);
    m_impl->mixCallback = std::move(mix);
    m_impl->capturing.store(false, std::memory_order_release);
    m_impl->running.store(true, std::memory_order_release);
    m_impl->captureThread = std::thread([impl = m_impl.get()] { impl->captureLoop(); });
    m_impl->playbackThread = std::thread([impl = m_impl.get()] { impl->playbackLoop(); });
    return true;
#else
    (void)javaVm;
    (void)capture;
    (void)mix;
    m_impl->setError(m_impl->captureError, "Voice audio is available on Android only");
    return false;
#endif
}

void AudioEngine::stop() {
    if (!m_impl->running.exchange(false, std::memory_order_acq_rel)) return;
    m_impl->capturing.store(false, std::memory_order_release);
    m_impl->wake.notify_all();
    if (m_impl->captureThread.joinable()) m_impl->captureThread.join();
    if (m_impl->playbackThread.joinable()) m_impl->playbackThread.join();
    m_impl->captureCallback = {};
    m_impl->mixCallback = {};
    m_impl->javaVm = nullptr;
}

void AudioEngine::setCaptureActive(bool active) {
    if (!m_impl->running.load(std::memory_order_acquire)) return;
    m_impl->capturing.store(active, std::memory_order_release);
    m_impl->wake.notify_all();
}

void AudioEngine::setInputGain(float gain) {
    m_impl->inputGain.store(std::clamp(gain, 0.0f, 2.0f), std::memory_order_relaxed);
}

bool AudioEngine::running() const {
    return m_impl->running.load(std::memory_order_acquire);
}

bool AudioEngine::captureActive() const {
    return m_impl->capturing.load(std::memory_order_acquire);
}

std::string AudioEngine::error() const {
    return m_impl->combinedError();
}

} // namespace bedrocktools::voice

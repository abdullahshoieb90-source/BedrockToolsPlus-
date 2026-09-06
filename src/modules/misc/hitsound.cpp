#include "hitsound.hpp"
#include "hitsound_files.hpp"

#include "../../config/ConfigManager.hpp"
#include "../../launcher/ExternalButtonRefresh.hpp"
#include <bedrocktools/events/EventBus.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <jni.h>
#endif

HitSoundModule* g_hitSound = nullptr;

namespace {

using namespace std::chrono;

std::string hitsoundsDirectoryForConfig() {
    const std::string configPath = bedrocktools::config::ConfigManager::get().getConfigPath();
    const std::size_t lastSlash = configPath.find_last_of('/');
    std::string dir = (lastSlash != std::string::npos) ? configPath.substr(0, lastSlash)
                                                       : "/sdcard/games/BedrockToolsPlus";
    return dir + "/hitsounds";
}

// A swing is only interesting for this long after it happens. This covers the
// singleplayer case (the hurt-time is set on the very next tick) and leaves
// generous slack for laggy servers to confirm the hit; anything older is
// either a whiffed/blocked/rejected hit (must stay silent) or a stale actor
// pointer whose memory may have been recycled.
constexpr auto kHitConfirmWindow = milliseconds(1000);

// Fastest melee cadence (auto-swing) is a few hits per second, so the number
// of unconfirmed swings at any moment is tiny; cap the list regardless.
constexpr std::size_t kMaxPendingHits = 8;

// hurtTime counts down from the maximum hurt flash (a handful of ticks) after
// each hit; values outside this range mean the pointer is not a live actor,
// so never read or trust them.
constexpr int kHurtTimeMin = 0;
constexpr int kHurtTimeMax = 100;

// ---------------------------------------------------------------------------
// Android audio engine, built on android.media.SoundPool — the platform API
// designed for low-latency game sound effects. The selected file is decoded
// once (SoundPool.load() returns immediately and the decode runs on
// SoundPool's own loader thread), so the hot per-hit path is a single cheap
// play() call: no file I/O, no codec setup and no native allocation ever
// happens on the game thread. SoundPool also manages the overlapping-stream
// cap itself (stealing the oldest stream), so there is no player list to
// prune on later ticks. Everything is serialized behind one mutex so the
// attack hook and the config path can safely share the engine even if the
// engine dispatches them from different threads. The whole engine is a no-op
// on non-Android builds.
// ---------------------------------------------------------------------------

#if defined(__ANDROID__)

std::mutex g_audioMutex;
JavaVM* g_vm = nullptr;

jclass g_spClass = nullptr; // global ref to android.media.SoundPool
jmethodID g_spInit = nullptr;
jmethodID g_spLoad = nullptr;
jmethodID g_spPlay = nullptr;
jmethodID g_spUnload = nullptr;
jmethodID g_spRelease = nullptr;

jobject g_soundPool = nullptr; // global ref to the SoundPool instance
int g_soundId = 0;             // id of the loaded hit sound inside the pool (0 = none)
std::string g_loadedPath;      // path currently queued/loaded in the pool

// A hit sound is a short sample fired a few times per second at most, so a
// small stream cap is plenty; when it is reached SoundPool steals the oldest
// stream, which sounds right for overlapping hit dings and keeps a fast
// attack stream (or creative-mode click spam) from stacking decoders.
constexpr jint kMaxStreams = 4;

// AudioManager.STREAM_MUSIC so hit sounds follow the game's media volume.
constexpr jint kStreamTypeMusic = 3;

// load()/play() priority; only one sample lives in the pool at a time.
constexpr jint kPriority = 1;

void clearJniException(JNIEnv* env) {
    if (env && env->ExceptionCheck()) env->ExceptionClear();
}

void setAudioJavaVm(void* vm) {
    std::lock_guard<std::mutex> lock(g_audioMutex);
    g_vm = static_cast<JavaVM*>(vm);
}

// Attaches the calling thread to the JVM for the duration of the scope and
// detaches it again afterwards, leaving the caller's thread state exactly as
// it was found (same pattern as the launcher's ExternalButtonRefresh).
class ScopedJniEnv {
public:
    explicit ScopedJniEnv(JavaVM* vm) {
        if (!vm) return;
        mVm = vm;
        const jint state = vm->GetEnv(reinterpret_cast<void**>(&mEnv), JNI_VERSION_1_6);
        if (state == JNI_OK && mEnv) return;
        if (state != JNI_EDETACHED) return;
        JavaVMAttachArgs attachArgs{
            JNI_VERSION_1_6, const_cast<char*>("BedrockToolsPlus/HitSound"), nullptr};
        if (vm->AttachCurrentThread(&mEnv, &attachArgs) == JNI_OK && mEnv) mAttached = true;
    }

    ~ScopedJniEnv() {
        if (mAttached && mVm) mVm->DetachCurrentThread();
    }

    JNIEnv* get() const { return mEnv; }

private:
    JavaVM* mVm = nullptr;
    JNIEnv* mEnv = nullptr;
    bool mAttached = false;
};

// Converts a native UTF-8 file path to a UTF-16 jstring payload. NewStringUTF
// expects modified UTF-8, which mangles supplementary-plane characters such
// as emoji in file names; NewString over plain UTF-16 is lossless.
std::vector<jchar> utf8ToJChars(const std::string& text) {
    std::vector<jchar> out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i++]);
        if (c < 0x80) {
            out.push_back(static_cast<jchar>(c));
            continue;
        }
        int extra = 0;
        std::uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07u;
        } else {
            continue; // invalid leading byte: drop it rather than misdecode
        }
        if (i + static_cast<std::size_t>(extra) > text.size()) break;
        bool valid = true;
        for (int k = 0; k < extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(text[i++]);
            if ((cc & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (!valid) continue;
        if (cp < 0x10000) {
            out.push_back(static_cast<jchar>(cp));
        } else {
            cp -= 0x10000;
            out.push_back(static_cast<jchar>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<jchar>(0xDC00 + (cp & 0x3FF)));
        }
    }
    return out;
}

bool ensureSoundPool(JNIEnv* env) {
    if (g_soundPool) return true;
    clearJniException(env);

    if (!g_spClass) {
        jclass local = env->FindClass("android/media/SoundPool");
        if (!local || env->ExceptionCheck()) {
            clearJniException(env);
            return false;
        }
        g_spClass = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        if (!g_spClass) return false;

        g_spInit = env->GetMethodID(g_spClass, "<init>", "(III)V");
        g_spLoad = env->GetMethodID(g_spClass, "load", "(Ljava/lang/String;I)I");
        g_spPlay = env->GetMethodID(g_spClass, "play", "(IFFIIF)I");
        g_spUnload = env->GetMethodID(g_spClass, "unload", "(I)Z");
        g_spRelease = env->GetMethodID(g_spClass, "release", "()V");
        if (!g_spInit || !g_spLoad || !g_spPlay || !g_spUnload || !g_spRelease ||
            env->ExceptionCheck()) {
            clearJniException(env);
            return false;
        }
    }

    // SoundPool(maxStreams, streamType, srcQuality). The plain constructor is
    // deprecated on the Java side in favour of SoundPool.Builder, but it is
    // the only direct (non-builder) constructor and works on every API level,
    // which matters when calling it through JNI.
    jobject local = env->NewObject(g_spClass, g_spInit, kMaxStreams, kStreamTypeMusic, static_cast<jint>(0));
    if (!local || env->ExceptionCheck()) {
        clearJniException(env);
        return false;
    }
    g_soundPool = env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
    if (!g_soundPool) {
        clearJniException(env);
        return false;
    }
    return true;
}

void unloadSoundLocked(JNIEnv* env) {
    if (env && g_soundPool && g_soundId != 0 && g_spUnload) {
        env->CallBooleanMethod(g_soundPool, g_spUnload, g_soundId);
        clearJniException(env);
    }
    g_soundId = 0;
    g_loadedPath.clear();
}

// Kicks off decoding `path` into the pool. SoundPool.load() is asynchronous:
// it queues the file and returns immediately, so this never stalls the
// caller. g_loadedPath is updated even when load() reports failure (it
// returns 0 for undecodable files) so a broken file is not re-queued on every
// single hit.
void loadSoundLocked(JNIEnv* env, const std::string& path) {
    if (!env || path.empty()) return;
    const std::vector<jchar> widePath = utf8ToJChars(path);
    jstring jpath = env->NewString(widePath.data(), static_cast<jsize>(widePath.size()));
    if (!jpath) {
        clearJniException(env);
        return;
    }
    const jint id = env->CallIntMethod(g_soundPool, g_spLoad, jpath, kPriority);
    clearJniException(env);
    env->DeleteLocalRef(jpath);
    g_soundId = (id > 0) ? static_cast<int>(id) : 0;
    g_loadedPath = path;
}

// Public audio entry points used by the module (each acquires the mutex so
// the calling thread's identity never matters).

// Decodes `path` ahead of time so hits only pay for play(). Already-loaded
// paths (and "None", i.e. an empty path) collapse to a cheap mutex+compare.
void audioLoadFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_audioMutex);
    if (!g_vm || path == g_loadedPath) return;
    ScopedJniEnv env(g_vm);
    if (!env.get() || !ensureSoundPool(env.get())) return;
    unloadSoundLocked(env.get());
    loadSoundLocked(env.get(), path);
}

// Hot per-hit path: play() just posts a start request to SoundPool's event
// thread — microseconds of work, regardless of how big the audio file is.
// Returns quietly while the sample is still decoding right after a (re)load
// or when nothing is selected; the next hit plays normally.
void audioPlay(float volume) {
    std::lock_guard<std::mutex> lock(g_audioMutex);
    if (!g_vm || !g_soundPool || g_soundId == 0 || !g_spPlay) return;
    ScopedJniEnv env(g_vm);
    if (!env.get()) return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    // play(soundID, leftVolume, rightVolume, priority, loop, rate) — loop 0
    // means "play once", rate 1.0 is the original pitch.
    env.get()->CallIntMethod(g_soundPool, g_spPlay, g_soundId, volume, volume, kPriority,
                             static_cast<jint>(0), 1.0f);
    clearJniException(env.get());
}

void audioUnload() {
    std::lock_guard<std::mutex> lock(g_audioMutex);
    if (!g_vm) {
        g_soundId = 0;
        g_loadedPath.clear();
        return;
    }
    ScopedJniEnv env(g_vm);
    unloadSoundLocked(env.get());
}

void audioReleaseAll() {
    std::lock_guard<std::mutex> lock(g_audioMutex);
    if (g_vm && g_soundPool) {
        ScopedJniEnv env(g_vm);
        if (env.get()) {
            if (g_spRelease) {
                env.get()->CallVoidMethod(g_soundPool, g_spRelease);
                clearJniException(env.get());
            }
            env.get()->DeleteGlobalRef(g_soundPool);
        }
    }
    g_soundPool = nullptr;
    g_soundId = 0;
    g_loadedPath.clear();
}

#else // !defined(__ANDROID__)

void setAudioJavaVm(void*) {}
void audioLoadFile(const std::string&) {}
void audioPlay(float) {}
void audioUnload() {}
void audioReleaseAll() {}

#endif

} // namespace

HitSoundModule::HitSoundModule()
    : Module("Hit Sound",
             "Plays a sound from the hitsounds folder only when a mob or player actually takes damage from your hit.") {
    g_hitSound = this;
}

HitSoundModule::~HitSoundModule() {
    audioReleaseAll();
    if (g_hitSound == this) g_hitSound = nullptr;
}

void HitSoundModule::onInit() {
    if (m_dir.empty()) m_dir = hitsoundsDirectoryForConfig();
    ensureSoundsDirectory();
    m_files = hitsound::scanSoundFiles(m_dir);
    refreshSelectionPath();
    setAudioJavaVm(bedrocktools::launcher::javaVm());

    bedrocktools::events::bus().subscribe<bedrocktools::events::AttackEvent>(
        [](auto& event) {
            if (g_hitSound && g_hitSound->enabled) g_hitSound->onAttack(event.target);
        });

    // Damage confirmation runs here: in singleplayer the victim's hurt-time is
    // already set on the tick after the swing, and in multiplayer it rises a
    // few ticks later when the server confirms the hit.
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto&) {
            if (g_hitSound && g_hitSound->enabled) g_hitSound->onTickCheck();
        });
}

void HitSoundModule::onEnable() {
    m_pendingHits.clear();
    audioLoadFile(m_currentPath);
}

void HitSoundModule::onDisable() {
    m_pendingHits.clear();
    audioUnload();
}

void HitSoundModule::onAttack(bedrocktools::sdk::Actor* target) {
    if (!enabled) return;
    if (!target || m_currentPath.empty()) return;

    const auto now = steady_clock::now();

    // The attack hook fires BEFORE the original attack runs, so this is the
    // victim's pre-swing state: if the hit actually lands its hurt-time gets
    // bumped to the top of its countdown, which onTickCheck() watches for.
    const int baseline = target->hurtTime();
    if (baseline < kHurtTimeMin || baseline > kHurtTimeMax) return;

    // Rapid repeated swings at the same target are normal (auto-swing): fold
    // them into the same entry instead of stacking duplicates that would play
    // the sound twice for one confirmed hit.
    for (auto& pending : m_pendingHits) {
        if (pending.target == target) {
            pending.baselineHurtTime = baseline;
            pending.at = now;
            return;
        }
    }

    if (m_pendingHits.size() >= kMaxPendingHits) m_pendingHits.erase(m_pendingHits.begin());
    m_pendingHits.push_back(PendingHit{target, baseline, now});
}

void HitSoundModule::onTickCheck() {
    if (m_pendingHits.empty()) return;

    const auto now = steady_clock::now();
    bool played = false;

    for (auto it = m_pendingHits.begin(); it != m_pendingHits.end();) {
        if (now - it->at > kHitConfirmWindow) {
            it = m_pendingHits.erase(it);
            continue;
        }

        const int hurtTime = it->target->hurtTime();

        // Implausible value: the actor object is gone and its memory has been
        // recycled. Drop the entry instead of trusting the read.
        if (hurtTime < kHurtTimeMin || hurtTime > kHurtTimeMax) {
            it = m_pendingHits.erase(it);
            continue;
        }

        // hurtTime counts down after a hit, so a fresh hit only ever shows up
        // as a jump upwards. This keeps the follow-up ticks (where it counts
        // back down through the same value we started at) from refiring.
        if (hurtTime > it->baselineHurtTime) {
            // Keep the entry (baseline moved up to the current value) so
            // further hits on the same victim keep triggering until the
            // window elapses.
            it->baselineHurtTime = hurtTime;
            it->at = now;
            played = true;
            ++it;
        } else {
            // Remember the latest value: it ticks down while the flash fades,
            // and updating the baseline makes the next jump-up unambiguous.
            it->baselineHurtTime = hurtTime;
            ++it;
        }
    }

    if (played) {
        // Safety net: if the sound is not in the pool yet (e.g. the module was
        // enabled before the JVM handle was available) queue the load now.
        // audioLoadFile() is a cheap no-op when the path is already loaded,
        // and the decode itself runs off the game thread.
        audioLoadFile(m_currentPath);
        audioPlay(m_volume);
    }
}

void HitSoundModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    if (m_dir.empty()) m_dir = hitsoundsDirectoryForConfig();
    const int previousIndex = m_selectedIndex;

    if (j.contains("m_sound")) {
        int parsedIndex = m_selectedIndex;
        std::string parsedName;
        if (j["m_sound"].is_string()) {
            hitsound::parseRadioValue(j["m_sound"].get<std::string>(), parsedIndex, parsedName);
        } else if (j["m_sound"].is_number_integer()) {
            parsedIndex = j["m_sound"].get<int>();
        }
        m_files = hitsound::scanSoundFiles(m_dir);
        m_selectedIndex = hitsound::resolveSelectionIndex(parsedIndex, parsedName, m_files);
    }

    if (j.contains("m_volume")) {
        m_volume = j["m_volume"].get<float>();
        if (m_volume < 0.0f) m_volume = 0.0f;
        if (m_volume > 1.0f) m_volume = 1.0f;
    }

    if (m_selectedIndex != previousIndex) refreshSelectionPath();
}

void HitSoundModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    if (m_dir.empty()) m_dir = hitsoundsDirectoryForConfig();
    m_files = hitsound::scanSoundFiles(m_dir);
    if (m_selectedIndex > static_cast<int>(m_files.size())) m_selectedIndex = 0;
    refreshSelectionPath();
    j["m_sound"] = hitsound::makeRadioValue(m_selectedIndex, m_files);
    j["m_volume"] = m_volume;
}

void HitSoundModule::ensureSoundsDirectory() {
    std::error_code ec;
    if (std::filesystem::is_directory(m_dir, ec)) return;
    if (!std::filesystem::create_directories(m_dir, ec) || ec) return;
    writeSampleWav(m_dir + "/Sample Hit.wav");
}

void HitSoundModule::writeSampleWav(const std::string& path) const {
    const std::vector<std::uint8_t> wav = hitsound::makeSampleHitWav();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
}

// Derives the absolute path of the selected entry (or clears it for "None").
// Kept small instead of storing a copy of the name so index/list changes can
// never leave a stale path behind. When the module is enabled the new
// selection is also (pre)loaded into the SoundPool so the next hit is lag-free.
void HitSoundModule::refreshSelectionPath() {
    m_currentPath.clear();
    if (m_selectedIndex > 0 && m_selectedIndex <= static_cast<int>(m_files.size())) {
        m_currentPath = m_dir + "/" + m_files[static_cast<std::size_t>(m_selectedIndex - 1)];
    }
    if (enabled) audioLoadFile(m_currentPath);
}

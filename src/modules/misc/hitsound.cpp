#include "hitsound.hpp"
#include "hitsound_files.hpp"

#include "../../config/ConfigManager.hpp"
#include "../../launcher/ExternalButtonRefresh.hpp"
#include <bedrocktools/events/EventBus.hpp>

#include <chrono>
#include <cmath>
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

// ---------------------------------------------------------------------------
// Android audio engine. Every MediaPlayer lives behind one mutex so the
// attack hook (which starts playback) and the tick hook (which reaps finished
// players) can safely share the active list even if the engine dispatches
// them from different threads. The engine is a no-op on non-Android builds.
// ---------------------------------------------------------------------------

#if defined(__ANDROID__)

std::mutex g_audioMutex;
JavaVM* g_vm = nullptr;

jclass g_mpClass = nullptr; // global ref to android.media.MediaPlayer
jmethodID g_mpInit = nullptr;
jmethodID g_mpSetDataSource = nullptr;
jmethodID g_mpPrepare = nullptr;
jmethodID g_mpStart = nullptr;
jmethodID g_mpStop = nullptr;
jmethodID g_mpRelease = nullptr;
jmethodID g_mpIsPlaying = nullptr;
jmethodID g_mpSetVolume = nullptr;

// Hard cap so a fast attack stream (or creative-mode click spam) can never
// stack unbounded decoders. Hit sounds are short, so 3 overlapping players
// is plenty; when the cap is reached the oldest player is cut off.
constexpr std::size_t kMaxPlayers = 3;

// Safety net for reaping: if isPlaying() ever reports true forever (it should
// not after PlaybackCompleted), force-release the player after this age.
constexpr double kMaxPlayerSeconds = 8.0;

struct ActivePlayer {
    jobject ref = nullptr; // global ref to android.media.MediaPlayer
    steady_clock::time_point startedAt;
};

std::vector<ActivePlayer> g_players;

void clearJniException(JNIEnv* env) {
    if (env && env->ExceptionCheck()) env->ExceptionClear();
}

void setAudioJavaVm(void* vm) {
    std::lock_guard<std::mutex> lock(g_audioMutex);
    g_vm = static_cast<JavaVM*>(vm);
}

bool ensureAudioApi(JNIEnv* env) {
    if (g_mpClass && g_mpInit) return true;
    clearJniException(env);
    jclass local = env->FindClass("android/media/MediaPlayer");
    if (!local || env->ExceptionCheck()) {
        clearJniException(env);
        return false;
    }
    g_mpClass = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    if (!g_mpClass) return false;

    g_mpInit = env->GetMethodID(g_mpClass, "<init>", "()V");
    g_mpSetDataSource = env->GetMethodID(g_mpClass, "setDataSource", "(Ljava/lang/String;)V");
    g_mpPrepare = env->GetMethodID(g_mpClass, "prepare", "()V");
    g_mpStart = env->GetMethodID(g_mpClass, "start", "()V");
    g_mpStop = env->GetMethodID(g_mpClass, "stop", "()V");
    g_mpRelease = env->GetMethodID(g_mpClass, "release", "()V");
    g_mpIsPlaying = env->GetMethodID(g_mpClass, "isPlaying", "()Z");
    g_mpSetVolume = env->GetMethodID(g_mpClass, "setVolume", "(FF)V");
    if (!g_mpInit || !g_mpSetDataSource || !g_mpPrepare || !g_mpStart || !g_mpStop ||
        !g_mpRelease || !g_mpIsPlaying || !g_mpSetVolume || env->ExceptionCheck()) {
        clearJniException(env);
        return false;
    }
    return true;
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

void releasePlayer(JNIEnv* env, jobject ref) {
    if (!env || !ref) return;
    clearJniException(env);
    if (g_mpClass && g_mpStop) {
        env->CallVoidMethod(ref, g_mpStop);
        clearJniException(env);
    }
    if (g_mpClass && g_mpRelease) {
        env->CallVoidMethod(ref, g_mpRelease);
        clearJniException(env);
    }
    env->DeleteGlobalRef(ref);
}

void pruneFinishedPlayers(JNIEnv* env) {
    if (!env) return;
    const auto now = steady_clock::now();
    for (std::size_t i = 0; i < g_players.size();) {
        bool finished = false;
        if (g_mpIsPlaying) {
            const jboolean playing = env->CallBooleanMethod(g_players[i].ref, g_mpIsPlaying);
            clearJniException(env);
            finished = !playing;
        }
        const double age = duration<double>(now - g_players[i].startedAt).count();
        if (age > kMaxPlayerSeconds) finished = true;
        if (finished) {
            releasePlayer(env, g_players[i].ref);
            g_players[i] = g_players.back();
            g_players.pop_back();
        } else {
            ++i;
        }
    }
}

void startPlayer(JNIEnv* env, const std::string& path, float volume) {
    if (!env || !g_mpClass) return;
    clearJniException(env);

    const std::vector<jchar> widePath = utf8ToJChars(path);
    jstring jpath = env->NewString(widePath.data(), static_cast<jsize>(widePath.size()));
    jobject player = env->NewObject(g_mpClass, g_mpInit);
    if (!player) {
        clearJniException(env);
        if (jpath) env->DeleteLocalRef(jpath);
        return;
    }

    bool ok = true;
    if (jpath) env->CallVoidMethod(player, g_mpSetDataSource, jpath);
    if (env->ExceptionCheck()) ok = false;
    clearJniException(env);
    if (ok) {
        env->CallVoidMethod(player, g_mpPrepare);
        if (env->ExceptionCheck()) ok = false;
        clearJniException(env);
    }
    if (jpath) {
        env->DeleteLocalRef(jpath);
        jpath = nullptr;
    }
    if (!ok) {
        env->DeleteLocalRef(player);
        return;
    }

    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    env->CallVoidMethod(player, g_mpSetVolume, volume, volume);
    clearJniException(env);
    env->CallVoidMethod(player, g_mpStart);
    clearJniException(env);

    jobject global = env->NewGlobalRef(player);
    env->DeleteLocalRef(player);
    if (!global) return;

    if (g_players.size() >= kMaxPlayers) {
        releasePlayer(env, g_players.front().ref);
        g_players.erase(g_players.begin());
    }
    g_players.push_back(ActivePlayer{global, steady_clock::now()});
}

// Public audio entry points used by the module (each acquires the mutex so
// the calling thread's identity never matters).
void audioPlayFile(const std::string& path, float volume) {
    std::lock_guard<std::mutex> lock(g_audioMutex);
    if (!g_vm || path.empty()) return;
    ScopedJniEnv env(g_vm);
    if (!env.get()) return;
    if (!ensureAudioApi(env.get())) return;
    pruneFinishedPlayers(env.get());
    startPlayer(env.get(), path, volume);
}

void audioPrune() {
    std::lock_guard<std::mutex> lock(g_audioMutex);
    if (!g_vm || g_players.empty()) return;
    ScopedJniEnv env(g_vm);
    if (!env.get()) return;
    if (g_mpClass) pruneFinishedPlayers(env.get());
}

void audioReleaseAll() {
    std::lock_guard<std::mutex> lock(g_audioMutex);
    if (!g_vm) {
        g_players.clear();
        return;
    }
    ScopedJniEnv env(g_vm);
    for (const ActivePlayer& player : g_players) releasePlayer(env.get(), player.ref);
    g_players.clear();
}

#else // !defined(__ANDROID__)

void setAudioJavaVm(void*) {}
void audioPlayFile(const std::string&, float) {}
void audioPrune() {}
void audioReleaseAll() {}

#endif

} // namespace

HitSoundModule::HitSoundModule()
    : Module("Hit Sound",
             "Plays a sound from the hitsounds folder when you hit a mob or player.") {
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

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto&) {
            if (g_hitSound && g_hitSound->enabled) g_hitSound->onTick();
        });

    bedrocktools::events::bus().subscribe<bedrocktools::events::AttackEvent>(
        [](auto& event) {
            if (g_hitSound && g_hitSound->enabled) g_hitSound->onAttack(event.target);
        });
}

void HitSoundModule::onEnable() {
}

void HitSoundModule::onDisable() {
    audioReleaseAll();
}

void HitSoundModule::onTick() {
    audioPrune();
}

void HitSoundModule::onAttack(void* target) {
    if (!enabled) return;
    if (!target || m_currentPath.empty()) return;

    // Debounce: never fire twice for the same press even if the engine calls
    // the attack path more than once, while still letting repeated swings
    // (auto-swing, ~4 per second) retrigger the sound.
    static steady_clock::time_point lastPlay;
    const auto now = steady_clock::now();
    if (now - lastPlay < milliseconds(40)) return;
    lastPlay = now;

    audioPlayFile(m_currentPath, m_volume);
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
// never leave a stale path behind.
void HitSoundModule::refreshSelectionPath() {
    m_currentPath.clear();
    if (m_selectedIndex > 0 && m_selectedIndex <= static_cast<int>(m_files.size())) {
        m_currentPath = m_dir + "/" + m_files[static_cast<std::size_t>(m_selectedIndex - 1)];
    }
}

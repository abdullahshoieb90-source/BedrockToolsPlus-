// End-to-end host test for the Hit Sound module's game-thread path.
//
// hitsound_test.cpp covers the pure helpers; this one compiles the real module
// translation unit and drives it the way the game does — attack hook, local
// player tick, config load — with the three things it needs from the platform
// faked:
//
//   * Actor::fetchNearbyActorsSorted, which the arrow path resolves through
//     bedrocktools::memory::resolve (stubbed here to hand back a list the test
//     controls, exactly like the game returns it: a std::vector by value whose
//     buffer the module has to release),
//   * the actors themselves: raw buffers laid out per the SDK offsets, so the
//     module's position / previous-position / hurt-time / category reads run
//     for real instead of being simulated,
//   * android.media.SoundPool through tests/fakejni, which records play()
//     calls instead of making noise.
//
// This is the regression test for "the hit sound does not play when my arrow
// hits a mob or a player": the shot is detected from the actors around the
// player, the landing from the victim's hurt-time, and the sound has to play
// exactly once — while a whiff, a skeleton's arrow and plain ambient damage
// stay silent.
//
// Build and run standalone (see scripts/run_tests.sh):
//     g++ -std=c++20 -I src -I include -I tests/fakejson -I tests/fakejni
//         tests/hitsound_module_test.cpp src/core/events/Events.cpp
//         -o /tmp/hitsound_module_test
//     /tmp/hitsound_module_test

#include "fakejni/jni.h"

// The module under test is compiled for the Android path so the SoundPool
// engine is the real one.
#define __ANDROID__ 1
#include "modules/misc/hitsound.cpp"

#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++g_failures;
    }
}

namespace sdk = bedrocktools::sdk;
namespace offs = bedrocktools::sdk::offsets;

// ---------------------------------------------------------------------------
// Fake actors: the SDK reads fields at fixed offsets, so a zeroed buffer with
// a state-vector component attached is a complete actor as far as the module
// is concerned.
// ---------------------------------------------------------------------------

class FakeActor {
public:
    FakeActor() {
        std::memset(m_bytes, 0, sizeof(m_bytes));
        std::memset(m_stateVector, 0, sizeof(m_stateVector));
        *reinterpret_cast<void**>(m_bytes + offs::Actor::mStateVectorComponent) = m_stateVector;
    }

    void* ptr() { return static_cast<void*>(m_bytes); }
    sdk::Actor* actor() { return static_cast<sdk::Actor*>(ptr()); }
    sdk::Player* player() { return static_cast<sdk::Player*>(ptr()); }

    void setPosition(sdk::Vec3 value) { writeStateVector(offs::StateVectorComponent::mPosition, value); }
    void setPreviousPosition(sdk::Vec3 value) {
        writeStateVector(offs::StateVectorComponent::mPreviousPosition, value);
    }
    void setHurtTime(int value) { write(m_bytes, offs::Actor::mHurtTime, value); }
    void setCategories(std::uint32_t value) { write(m_bytes, offs::Actor::mCategories, value); }

    // Distance-sorted list entry, as the game's fetch returns it.
    DistanceSortedActor entry() const { return DistanceSortedActor{const_cast<char*>(m_bytes), 0.0f, 0.0f}; }

private:
    template <class T>
    static void write(char* base, std::size_t offset, T value) {
        *reinterpret_cast<T*>(base + offset) = value;
    }

    template <class T>
    void writeStateVector(std::size_t offset, T value) {
        *reinterpret_cast<T*>(m_stateVector + offset) = value;
    }

    alignas(16) char m_bytes[1024];
    alignas(16) char m_stateVector[64];
};

// What the level currently has near the player; the fake fetch returns it.
std::vector<DistanceSortedActor> g_nearby;

std::vector<DistanceSortedActor> fakeFetchNearbyActorsSorted(void*, const void*, int) {
    return g_nearby; // by value, like the real function
}

// ---------------------------------------------------------------------------
// Fake android.media.SoundPool
// ---------------------------------------------------------------------------

_jclass g_soundPoolClass;
_jobject g_soundPoolInstance;
_jobject g_pathString;
_jmethodID g_methodInit;
_jmethodID g_methodLoad;
_jmethodID g_methodPlay;
_jmethodID g_methodUnload;
_jmethodID g_methodRelease;
JNIEnv g_env{};
JavaVM g_fakeVm{};

int g_playCalls = 0;
int g_loadCalls = 0;
float g_lastVolume = -1.0f;

jclass fakeFindClass(JNIEnv*, const char* name) {
    return name && std::strcmp(name, "android/media/SoundPool") == 0 ? &g_soundPoolClass : nullptr;
}

jmethodID fakeGetMethodID(JNIEnv*, jclass, const char* name, const char*) {
    if (!name) return nullptr;
    if (std::strcmp(name, "<init>") == 0) return &g_methodInit;
    if (std::strcmp(name, "load") == 0) return &g_methodLoad;
    if (std::strcmp(name, "play") == 0) return &g_methodPlay;
    if (std::strcmp(name, "unload") == 0) return &g_methodUnload;
    if (std::strcmp(name, "release") == 0) return &g_methodRelease;
    return nullptr;
}

jobject fakeNewObject(JNIEnv*, jclass, jmethodID, va_list) { return &g_soundPoolInstance; }
jobject fakeNewGlobalRef(JNIEnv*, jobject object) { return object; }
jobject fakeNewLocalRef(JNIEnv*, jobject object) { return object; }
jstring fakeNewString(JNIEnv*, const jchar*, jsize) { return &g_pathString; }
jboolean fakeExceptionCheck(JNIEnv*) { return JNI_FALSE; }
void fakeExceptionClear(JNIEnv*) {}
void fakeDeleteLocalRef(JNIEnv*, jobject) {}
void fakeDeleteGlobalRef(JNIEnv*, jobject) {}
jint fakeGetEnv(JavaVM*, void** env, jint) {
    *env = &g_env;
    return JNI_OK;
}

jint fakeCallIntMethod(JNIEnv*, jobject, jmethodID method, va_list args) {
    if (method == &g_methodLoad) {
        va_arg(args, jstring);
        va_arg(args, jint);
        ++g_loadCalls;
        return 7; // a valid sound id, so play() is allowed
    }
    if (method == &g_methodPlay) {
        va_arg(args, jint); // sound id
        const auto left = static_cast<float>(va_arg(args, double));
        va_arg(args, double); // right volume
        va_arg(args, jint);   // priority
        va_arg(args, jint);   // loop
        va_arg(args, double); // rate
        ++g_playCalls;
        g_lastVolume = left;
        return 1;
    }
    return 0;
}

jboolean fakeCallBooleanMethod(JNIEnv*, jobject, jmethodID method, va_list args) {
    if (method == &g_methodUnload) va_arg(args, jint);
    return JNI_TRUE;
}

void fakeCallVoidMethod(JNIEnv*, jobject, jmethodID, va_list) {}

void setupFakeJni() {
    g_env.FindClassFn = fakeFindClass;
    g_env.GetMethodIDFn = fakeGetMethodID;
    g_env.NewObjectVFn = fakeNewObject;
    g_env.NewGlobalRefFn = fakeNewGlobalRef;
    g_env.NewLocalRefFn = fakeNewLocalRef;
    g_env.NewStringFn = fakeNewString;
    g_env.ExceptionCheckFn = fakeExceptionCheck;
    g_env.ExceptionClearFn = fakeExceptionClear;
    g_env.DeleteLocalRefFn = fakeDeleteLocalRef;
    g_env.DeleteGlobalRefFn = fakeDeleteGlobalRef;
    g_env.CallIntMethodVFn = fakeCallIntMethod;
    g_env.CallBooleanMethodVFn = fakeCallBooleanMethod;
    g_env.CallVoidMethodVFn = fakeCallVoidMethod;
    g_fakeVm.GetEnvFn = fakeGetEnv;
}

} // namespace

// The module resolves the nearby-actor fetch through this; the rest of the
// signature table is not used by the paths under test.
namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId id) {
    if (id == SignatureId::ActorFetchNearbyActorsSorted) {
        return reinterpret_cast<std::uintptr_t>(&fakeFetchNearbyActorsSorted);
    }
    return 0;
}
} // namespace bedrocktools::memory

namespace bedrocktools::launcher {
void* javaVm() { return &g_fakeVm; }
} // namespace bedrocktools::launcher

// Only the two members the module touches; the real ConfigManager owns a
// writer thread the tests do not need.
namespace bedrocktools::config {
ConfigManager::~ConfigManager() = default;
std::string ConfigManager::getConfigPath() const { return m_configPath; }
void ConfigManager::setConfigPath(const std::string& path) { m_configPath = path; }
} // namespace bedrocktools::config

namespace {

constexpr std::uint32_t kMob = offs::ActorCategories::IsMob;
constexpr std::uint32_t kPlayer = offs::ActorCategories::IsPlayer;
constexpr std::uint32_t kProjectile = offs::ActorCategories::IsProjectile;

std::string radioValue(int index) {
    return std::to_string(index) + ",None,Sample Hit.wav";
}

void loadSettings(HitSoundModule& module, int index, bool arrowHits) {
    nlohmann::json j;
    j["m_sound"] = radioValue(index);
    j["m_volume"] = 0.5f;
    j["m_arrowHits"] = arrowHits;
    module.loadConfig(j);
}

} // namespace

int main() {
    setupFakeJni();

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "hitsound_module_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    bedrocktools::config::ConfigManager::get().setConfigPath((root / "config.json").string());

    std::printf("module setup\n");
    HitSoundModule module;
    module.onInit();
    check(std::filesystem::exists(root / "hitsounds" / "Sample Hit.wav"),
          "onInit creates the hitsounds folder with a sample");

    loadSettings(module, 1, true);
    module.setMasterEnabled(true);
    check(g_loadCalls == 1, "enabling the module preloads the selected sound");

    FakeActor player;
    player.setPosition(sdk::Vec3{0.0f, 64.0f, 0.0f});
    player.setPreviousPosition(sdk::Vec3{0.0f, 64.0f, 0.0f});
    player.setCategories(kPlayer);

    FakeActor arrow;
    arrow.setCategories(kProjectile);

    auto* playerPtr = player.player();

    // Every section gets its own victim. A melee swing keeps its target on the
    // confirmation list for a second of real time, so reusing one actor across
    // sections would let a leftover melee entry answer for the arrow path.
    struct Victim {
        FakeActor actor;
        Victim() {
            actor.setPosition(sdk::Vec3{10.0f, 64.0f, 0.0f});
            actor.setPreviousPosition(sdk::Vec3{10.0f, 64.0f, 0.0f});
            actor.setCategories(kMob);
        }
        DistanceSortedActor entry() { return actor.entry(); }
        sdk::Actor* ptr() { return actor.actor(); }
        void hurt(int value) { actor.setHurtTime(value); }
    };

    std::printf("melee hit\n");
    Victim meleeVictim;
    g_nearby = {player.entry(), meleeVictim.entry()};
    g_playCalls = 0;
    module.onAttack(meleeVictim.ptr());
    module.onTickCheck(playerPtr);
    check(g_playCalls == 0, "a swing whose damage has not landed is silent");

    meleeVictim.hurt(10);
    module.onTickCheck(playerPtr);
    check(g_playCalls == 1, "a confirmed melee hit plays the sound");
    check(g_lastVolume > 0.49f && g_lastVolume < 0.51f, "at the configured volume");

    module.onTickCheck(playerPtr);
    check(g_playCalls == 1, "the hurt flash counting down does not play again");

    std::printf("arrow hit\n");
    Victim arrowVictim;
    g_nearby = {player.entry(), arrowVictim.entry(), arrow.entry()};
    g_playCalls = 0;

    // The bow is released: the arrow appears at the player with no motion
    // recorded for it yet, exactly as the game spawns it.
    arrow.setPosition(sdk::Vec3{0.3f, 64.4f, 0.0f});
    arrow.setPreviousPosition(sdk::Vec3{0.3f, 64.4f, 0.0f});
    module.onTickCheck(playerPtr);

    // The next tick shows it flying away from the player: that is the shot.
    arrow.setPosition(sdk::Vec3{3.5f, 64.4f, 0.0f});
    module.onTickCheck(playerPtr);
    check(g_playCalls == 0, "releasing the arrow is not a hit");

    // The arrow lands: it is gone from the world and the mob flashes.
    arrowVictim.hurt(10);
    g_nearby = {player.entry(), arrowVictim.entry()};
    module.onTickCheck(playerPtr);
    check(g_playCalls == 1, "the arrow landing on a mob plays the sound");
    module.onTickCheck(playerPtr);
    check(g_playCalls == 1, "one arrow plays once");

    std::printf("arrow that is not a hit\n");
    Victim quietVictim;
    // Ambient damage with no shot of ours in flight.
    g_nearby = {player.entry(), quietVictim.entry()};
    g_playCalls = 0;
    module.onTickCheck(playerPtr);
    quietVictim.hurt(10);
    module.onTickCheck(playerPtr);
    check(g_playCalls == 0, "damage with no shot in flight stays silent");

    // A skeleton's arrow: it spawns away from the player and flies towards us.
    Victim skeletonVictim;
    arrow.setPosition(sdk::Vec3{4.0f, 64.0f, 0.0f});
    arrow.setPreviousPosition(sdk::Vec3{7.0f, 64.0f, 0.0f});
    g_nearby = {player.entry(), skeletonVictim.entry(), arrow.entry()};
    g_playCalls = 0;
    module.onTickCheck(playerPtr);
    skeletonVictim.hurt(10);
    g_nearby = {player.entry(), skeletonVictim.entry()};
    module.onTickCheck(playerPtr);
    check(g_playCalls == 0, "an arrow shot at us does not play the sound");

    // An arrow that hits a block: nothing flashes, nothing plays.
    Victim blockVictim;
    arrow.setPosition(sdk::Vec3{1.0f, 64.4f, 0.0f});
    arrow.setPreviousPosition(sdk::Vec3{0.0f, 64.4f, 0.0f});
    g_nearby = {player.entry(), blockVictim.entry(), arrow.entry()};
    g_playCalls = 0;
    module.onTickCheck(playerPtr);
    g_nearby = {player.entry(), blockVictim.entry()};
    module.onTickCheck(playerPtr);
    check(g_playCalls == 0, "an arrow that hits a block stays silent");

    std::printf("arrow hits toggle\n");
    loadSettings(module, 1, false);
    nlohmann::json saved;
    module.saveConfig(saved);
    check(saved.contains("m_arrowHits"), "the toggle is part of the saved config");
    check(!saved["m_arrowHits"].get<bool>(), "and keeps its value");

    Victim toggledVictim;
    arrow.setPosition(sdk::Vec3{1.0f, 64.4f, 0.0f});
    arrow.setPreviousPosition(sdk::Vec3{0.0f, 64.4f, 0.0f});
    g_nearby = {player.entry(), toggledVictim.entry(), arrow.entry()};
    g_playCalls = 0;
    module.onTickCheck(playerPtr);
    toggledVictim.hurt(10);
    g_nearby = {player.entry(), toggledVictim.entry()};
    module.onTickCheck(playerPtr);
    check(g_playCalls == 0, "with Arrow Hits off an arrow landing plays nothing");

    Victim meleeOnlyVictim;
    g_nearby = {player.entry(), meleeOnlyVictim.entry()};
    g_playCalls = 0;
    module.onAttack(meleeOnlyVictim.ptr());
    module.onTickCheck(playerPtr);
    meleeOnlyVictim.hurt(10);
    module.onTickCheck(playerPtr);
    check(g_playCalls == 1, "melee still plays with Arrow Hits off");

    std::printf("no sound selected\n");
    loadSettings(module, 0, true);
    Victim silentVictim;
    g_nearby = {player.entry(), silentVictim.entry()};
    g_playCalls = 0;
    module.onAttack(silentVictim.ptr());
    module.onTickCheck(playerPtr);
    silentVictim.hurt(10);
    module.onTickCheck(playerPtr);
    check(g_playCalls == 0, "None keeps the vanilla behaviour for melee");

    g_nearby = {player.entry(), silentVictim.entry(), arrow.entry()};
    module.onTickCheck(playerPtr);
    check(g_playCalls == 0, "None keeps the vanilla behaviour for arrows");

    module.setMasterEnabled(false);
    ec.clear();
    std::filesystem::remove_all(root, ec);

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("all hit sound module tests passed\n");
        return 0;
    }
    std::printf("%d hit sound module test(s) failed\n", g_failures);
    return 1;
}

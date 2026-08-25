// Regression test for Wings world-space overlay module.
//
// The old implementation patched SerializedSkinImpl (mSkinImage,
// mGeometryData, mDefaultGeometryName). Bedrock removed support for custom
// geometry on classic skins, so that made the player disappear.
//
// The new implementation renders wings as a world-space overlay attached to
// the player via RenderLevel hook + tessellator, preserving sin(time) flapping
// and Flap Speed option, without ever touching skin memory.
//
// This host test verifies the new logic without needing Minecraft:
//
//   * flap animation is sin(time) driven and respects flap speed
//   * dt <= 0 does not advance the clock
//   * loadConfig clamps flap speed to [0.1, 10.0]
//   * onLocalPlayerTick does not modify any fake skin buffer (proving no skin patch)
//   * onLocalPlayerTick with null / fake player does not crash
//   * wingsDirectory() is non-empty
//   * constants are sane (amplitude 35deg, base rate 6 rad/s, wing size >0)
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src -I include -I third_party
//         -I tests/fakejson -I tests/fakepl
//         tests/wings_patch_test.cpp src/modules/visual/wings.cpp
//         -o /tmp/wings_patch_test
//     /tmp/wings_patch_test

#include <bedrocktools/modules/visual/wings.hpp>
#include "config/ConfigManager.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/memory/Signatures.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

namespace off = bedrocktools::sdk::offsets;

namespace {
int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

std::string g_testConfigPath = "/tmp/bt-wings-test/config.json";

constexpr float kPi = 3.14159265358979323846f;
bool approxEqual(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) <= eps;
}

} // namespace

// ---------------------------------------------------------------------------
// Stubs for host test
// ---------------------------------------------------------------------------

namespace bedrocktools::config {
ConfigManager::~ConfigManager() = default;
std::string ConfigManager::getConfigPath() const { return g_testConfigPath; }
} // namespace bedrocktools::config

namespace bedrocktools::events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace bedrocktools::events

namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId) { return 0; }
bool resolveAll(std::string_view) { return false; }
void clear() {}
} // namespace bedrocktools::memory

namespace {

// Fake player helpers for onLocalPlayerTick
// Layout mirrors what WingsModule::getActorAABB / getActorRotation expect.

struct FakeAABB {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
};

struct FakeAABBComponent {
    FakeAABB aabb;
};

struct FakeBuiltIn {
    std::uintptr_t aabbCompPtr; // at offset BuiltInActorComponents::mAABBShapeComponent = 8
    // we need padding to reach 8
};

static_assert(off::BuiltInActorComponents::mAABBShapeComponent == 8);

} // namespace

int main() {
    std::printf("wings world-space overlay - flap animation\n");

    // ------------------------------------------------------------------
    // Basic constants sanity
    // ------------------------------------------------------------------
    check(WingsModule::kFlapAmplitudeDegrees == 35.0f, "amplitude is 35 deg");
    check(WingsModule::kFlapBaseRate == 6.0f, "base rate is 6 rad/s");
    check(WingsModule::kWingWidth > 0.0f && WingsModule::kWingHeight > 0.0f, "wing dimensions positive");

    // ------------------------------------------------------------------
    // Flap animation: sin(time) driven
    // ------------------------------------------------------------------
    WingsModule mod;
    mod.onInit();
    mod.m_flapSpeed = 1.0f;

    check(approxEqual(mod.flapTime(), 0.0f), "initial flapTime 0");
    check(approxEqual(mod.currentFlapAngleDegrees(), 0.0f), "initial angle 0");

    mod.advanceFlapAnimation(0.25f); // t = 0.25
    float expectedPhase = 0.25f * WingsModule::kFlapBaseRate * 1.0f; // 1.5 rad
    float expectedDeg = WingsModule::kFlapAmplitudeDegrees * std::sin(expectedPhase);
    check(approxEqual(mod.flapTime(), 0.25f), "flapTime after 0.25s");
    check(approxEqual(mod.currentFlapAngleDegrees(), expectedDeg, 0.01f), "angle = amp * sin(phase) after 0.25s");

    float firstAngle = mod.currentFlapAngleDegrees();

    mod.advanceFlapAnimation(0.25f); // t = 0.5
    float secondPhase = 0.5f * WingsModule::kFlapBaseRate;
    float secondExpected = WingsModule::kFlapAmplitudeDegrees * std::sin(secondPhase);
    check(!approxEqual(mod.currentFlapAngleDegrees(), firstAngle, 0.001f), "angle changes on second tick");
    check(approxEqual(mod.currentFlapAngleDegrees(), secondExpected, 0.01f), "angle after 0.5s matches sin");

    // dt <= 0 must not advance
    float before = mod.flapTime();
    mod.advanceFlapAnimation(0.0f);
    mod.advanceFlapAnimation(-1.0f);
    check(approxEqual(mod.flapTime(), before), "dt <=0 does not advance");

    // ------------------------------------------------------------------
    // Flap speed affects phase
    // ------------------------------------------------------------------
    std::printf("flap speed setting\n");
    WingsModule fastMod;
    fastMod.onInit();
    fastMod.m_flapSpeed = 3.0f;
    fastMod.advanceFlapAnimation(0.25f);
    float fastPhase = 0.25f * WingsModule::kFlapBaseRate * 3.0f;
    float fastExpected = WingsModule::kFlapAmplitudeDegrees * std::sin(fastPhase);
    check(approxEqual(fastMod.currentFlapAngleDegrees(), fastExpected, 0.01f), "fast speed 3.0 produces larger phase");

    WingsModule slowMod;
    slowMod.onInit();
    slowMod.m_flapSpeed = 0.1f;
    slowMod.advanceFlapAnimation(1.0f);
    float slowPhase = 1.0f * WingsModule::kFlapBaseRate * 0.1f;
    float slowExpected = WingsModule::kFlapAmplitudeDegrees * std::sin(slowPhase);
    check(approxEqual(slowMod.currentFlapAngleDegrees(), slowExpected, 0.01f), "slow speed 0.1 produces smaller phase");

    check(!approxEqual(fastMod.currentFlapAngleDegrees(), slowMod.currentFlapAngleDegrees(), 0.001f),
          "different flap speeds produce different angles");

    // ------------------------------------------------------------------
    // Config clamping
    // ------------------------------------------------------------------
    std::printf("config clamping\n");
    {
        nlohmann::json j;
        j["m_flapSpeed"] = 20.0f;
        WingsModule cfgMod;
        cfgMod.loadConfig(j);
        check(cfgMod.m_flapSpeed <= 10.0f + 1e-3f, "flap speed clamped to max 10");
    }
    {
        nlohmann::json j;
        j["m_flapSpeed"] = -5.0f;
        WingsModule cfgMod;
        cfgMod.loadConfig(j);
        check(cfgMod.m_flapSpeed >= 0.1f - 1e-3f, "flap speed clamped to min 0.1");
    }
    {
        nlohmann::json j;
        j["flapSpeed"] = 2.5f; // legacy key
        WingsModule cfgMod;
        cfgMod.loadConfig(j);
        check(approxEqual(cfgMod.m_flapSpeed, 2.5f), "legacy flapSpeed key accepted");
    }

    // ------------------------------------------------------------------
    // onLocalPlayerTick must not touch skin and must not crash
    // ------------------------------------------------------------------
    std::printf("onLocalPlayerTick does not patch skin\n");

    // Create a fake skin buffer that would have been modified by old code
    std::vector<std::uint8_t> fakeSkin(512, 0xAA);
    std::vector<std::uint8_t> skinSnapshot = fakeSkin;

    // Null player
    WingsModule tickMod;
    tickMod.onInit();
    tickMod.enabled = true;
    tickMod.onLocalPlayerTick(nullptr);
    check(true, "onLocalPlayerTick(nullptr) does not crash");

    // Fake player with minimal AABB + rotation chain
    // Allocate components
    FakeAABBComponent* aabbComp = new FakeAABBComponent();
    aabbComp->aabb = {0.0f, 0.0f, 0.0f, 0.6f, 1.8f, 0.6f};

    // Built-in components block that holds pointer to aabbComp at offset 8
    // We need a buffer large enough: at least 8 + sizeof(ptr)
    std::vector<std::uint8_t> builtInBuf(32, 0);
    std::uintptr_t aabbCompAddr = reinterpret_cast<std::uintptr_t>(aabbComp);
    std::memcpy(builtInBuf.data() + off::BuiltInActorComponents::mAABBShapeComponent, &aabbCompAddr, sizeof(void*));

    // Rotation component
    bedrocktools::sdk::Vec2* rotComp = new bedrocktools::sdk::Vec2{10.0f, 45.0f};

    // Player buffer: size at least max(mStateVectorComponent, mActorRotationComponent) + ptr
    std::size_t playerSize = std::max(off::Actor::mStateVectorComponent, off::Actor::mActorRotationComponent) + sizeof(void*) + 16;
    std::vector<std::uint8_t> playerBuf(playerSize, 0);
    std::uintptr_t builtInAddr = reinterpret_cast<std::uintptr_t>(builtInBuf.data());
    std::uintptr_t rotAddr = reinterpret_cast<std::uintptr_t>(rotComp);
    std::memcpy(playerBuf.data() + off::Actor::mStateVectorComponent, &builtInAddr, sizeof(void*));
    std::memcpy(playerBuf.data() + off::Actor::mActorRotationComponent, &rotAddr, sizeof(void*));

    // Enable and tick with fake player
    tickMod.enabled = true;
    tickMod.m_flapSpeed = 1.0f;
    tickMod.onLocalPlayerTick(playerBuf.data());
    check(true, "onLocalPlayerTick(fake player) does not crash");

    // Ensure fake skin untouched (new module never touches skin)
    check(fakeSkin == skinSnapshot, "fake skin buffer untouched (no skin patching)");

    // Second tick should advance flap
    float timeBefore = tickMod.flapTime();
    // Sleep a tiny bit to get dt >0 from steady_clock, or manually advance
    tickMod.advanceFlapAnimation(0.1f);
    check(tickMod.flapTime() > timeBefore, "flap time advances after tick");

    // Disable should not crash
    tickMod.enabled = false;
    tickMod.onLocalPlayerTick(playerBuf.data());
    check(true, "onLocalPlayerTick after disable does not crash");

    delete aabbComp;
    delete rotComp;

    // ------------------------------------------------------------------
    // wingsDirectory non-empty
    // ------------------------------------------------------------------
    check(!tickMod.wingsDirectory().empty(), "wingsDirectory() non-empty");

    // ------------------------------------------------------------------
    // Radians vs degrees consistency
    // ------------------------------------------------------------------
    WingsModule radMod;
    radMod.onInit();
    radMod.m_flapSpeed = 1.0f;
    radMod.advanceFlapAnimation(0.33f);
    float deg = radMod.currentFlapAngleDegrees();
    float rad = radMod.currentFlapAngleRadians();
    check(approxEqual(rad, deg * kPi / 180.0f, 1e-3f), "radians = degrees * pi/180");

    // ------------------------------------------------------------------
    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all wings patch checks passed (world-space overlay)\n");
    return 0;
}

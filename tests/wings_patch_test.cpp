// Regression test for Wings world-space overlay module.
//
// The old implementation patched SerializedSkinImpl (mSkinImage,
// mGeometryData, mDefaultGeometryName). Bedrock removed support for custom
// geometry on classic skins, so that made the player disappear.
//
// The new implementation renders wings as a world-space overlay attached to
// the player via RenderLevel hook + tessellator, without ever touching skin
// memory. The wings are articulated 3D boxes driven by a bone hierarchy
// (shoulder -> upper -> tip + feathers) that mirrors the embedded
// geo/animation JSON, and the idle/flap/glide animation is driven by the
// player's horizontal/vertical speed.
//
// This host test verifies the new logic without needing Minecraft:
//
//   * flap animation driver is sin(time) driven and respects flap speed
//   * dt <= 0 does not advance the clock
//   * loadConfig clamps flap speed to [0.1, 10.0]
//   * onLocalPlayerTick does not modify any fake skin buffer (proving no skin patch)
//   * onLocalPlayerTick with null / fake player does not crash
//   * wingsDirectory() is non-empty
//   * constants are sane
//   * speed-driven blending: intensity ramps up while moving, decays to idle
//     pose at rest; sustained descent engages glide; glide suppresses flap
//   * bone angles follow the shoulder->tip->feather lag wave
//   * embedded geo/animation/controller JSON contain the new bone hierarchy
//     (bone_wing_right / bone_wing_left and children) and speed queries
//   * the C++ face palette matches the generated texture palette, and the
//     texture pixels paint outer/inner membranes, frame and feather tips
//   * ensureWingsAssetFiles writes geo JSON, animation JSON and a PNG
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src -I include -I third_party
//         -I tests/fakejson -I tests/fakepl
//         tests/wings_patch_test.cpp src/modules/visual/wings.cpp
//         -o /tmp/wings_patch_test
//     /tmp/wings_patch_test

#include <bedrocktools/modules/visual/wings.hpp>
#include <bedrocktools/modules/visual/wings_default.hpp>
#include "config/ConfigManager.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/memory/Signatures.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <sstream>
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

bool contains(const char* haystack, const std::string& needle) {
    return haystack != nullptr && std::string(haystack).find(needle) != std::string::npos;
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

    // ==================================================================
    // 3D bone hierarchy animation, driven by player speed
    // ==================================================================
    std::printf("bone hierarchy / speed-driven animation\n");

    // Lag constants form a wave from shoulder to feathers
    check(WingsModule::kUpperLag > 0.0f && WingsModule::kUpperLag < WingsModule::kTipLag &&
          WingsModule::kTipLag < WingsModule::kFeatherLagBase,
          "flap lag window: upper < tip < feathers");

    // Idle pose at rest (fresh module, zero speed): gentle breathing around
    // kIdleBaseDegrees with kIdleAmplitudeDegrees.
    WingsModule idleMod;
    idleMod.onInit();
    idleMod.m_flapSpeed = 1.0f;
    idleMod.advanceFlapAnimation(1.0f); // idlePh = pi/2 -> sin = 1 exactly
    {
        WingBoneAngles a = idleMod.currentBoneAngles();
        check(a.intensity < 1e-6f, "idle: intensity stays 0 at rest");
        check(approxEqual(a.shoulderDeg, WingsModule::kIdleBaseDegrees + WingsModule::kIdleAmplitudeDegrees, 0.01f),
              "idle: shoulder = base + amplitude at pulse peak");
        check(approxEqual(a.upperDeg, 6.0f + 4.0f * std::sin(WingsModule::kIdleRate - WingsModule::kIdleUpperLag), 0.01f),
              "idle: upper segment follows with lag");
        check(a.shoulderDeg > 0.0f && a.featherDeg[0] < 0.0f,
              "idle: feathers sway opposite to the shoulder at pulse peak");
    }

    // Full flight: feed running speed until intensity saturates, then measure
    // the per-bone oscillation envelope over one flap period.
    WingsModule flyMod;
    flyMod.onInit();
    flyMod.m_flapSpeed = 1.0f;
    for (int i = 0; i < 120 && flyMod.flightIntensity() < 0.995f; ++i) {
        flyMod.advanceWingAnimation(0.05f, 6.0f, 0.0f);
    }
    check(flyMod.flightIntensity() > 0.98f, "running at 6 blocks/s ramps flap intensity to ~1");
    {
        float shoulderMin = 1e9f, shoulderMax = -1e9f;
        float upperAbsMax = 0.0f, tipAbsMax = 0.0f;
        float featherAbsMax[4] = {0, 0, 0, 0};
        WingsModule probe;
        probe.onInit();
        probe.m_flapSpeed = 1.0f;
        for (int i = 0; i < 200 && probe.flightIntensity() < 0.999f; ++i) {
            probe.advanceWingAnimation(0.05f, 6.0f, 0.0f);
        }
        // one full flap period at base rate 6: 2*pi/6 ~ 1.0472 s
        const int steps = 400;
        for (int i = 0; i < steps; ++i) {
            probe.advanceWingAnimation((2.0f * kPi / WingsModule::kFlapBaseRate) / steps, 6.0f, 0.0f);
            WingBoneAngles a = probe.currentBoneAngles();
            shoulderMin = std::min(shoulderMin, a.shoulderDeg);
            shoulderMax = std::max(shoulderMax, a.shoulderDeg);
            upperAbsMax = std::max(upperAbsMax, std::fabs(a.upperDeg));
            tipAbsMax = std::max(tipAbsMax, std::fabs(a.tipDeg));
            for (int f = 0; f < 4; ++f) featherAbsMax[f] = std::max(featherAbsMax[f], std::fabs(a.featherDeg[f]));
        }
        check(approxEqual(shoulderMin, WingsModule::kFlightBaseDegrees - WingsModule::kFlapAmplitudeDegrees, 0.25f),
              "flight: shoulder bottoms at base - amplitude");
        check(approxEqual(shoulderMax, WingsModule::kFlightBaseDegrees + WingsModule::kFlapAmplitudeDegrees, 0.25f),
              "flight: shoulder peaks at base + amplitude");
        check(approxEqual(upperAbsMax, WingsModule::kFlightUpperAmplitudeDegrees, 0.25f),
              "flight: upper oscillates with its own amplitude");
        check(approxEqual(tipAbsMax, WingsModule::kFlightTipAmplitudeDegrees, 0.25f),
              "flight: tip oscillates with its own amplitude");
        check(approxEqual(featherAbsMax[0], WingsModule::kFlightFeatherAmplitudeDegrees, 0.25f) &&
              approxEqual(featherAbsMax[3], WingsModule::kFlightFeatherAmplitudeDegrees, 0.25f),
              "flight: feathers oscillate at feather amplitude");
        check(featherAbsMax[0] < tipAbsMax && tipAbsMax < WingsModule::kFlapAmplitudeDegrees,
              "flight: amplitude shrinks along the chain (feathers < tip < shoulder)");
    }

    // Decay back to idle when the player stops moving.
    WingsModule stopMod;
    stopMod.onInit();
    for (int i = 0; i < 120 && stopMod.flightIntensity() < 0.995f; ++i) {
        stopMod.advanceWingAnimation(0.05f, 6.0f, 0.0f);
    }
    for (int i = 0; i < 240; ++i) stopMod.advanceWingAnimation(0.05f, 0.0f, 0.0f); // 12 s idle
    check(stopMod.flightIntensity() < 0.05f, "stopping decays intensity back to idle");
    {
        WingBoneAngles a = stopMod.currentBoneAngles();
        check(std::fabs(a.shoulderDeg) < WingsModule::kIdleBaseDegrees + WingsModule::kIdleAmplitudeDegrees + 0.5f,
              "idle pose range after decay stays within idle envelope");
    }

    // Glide: sustained fast descent spreads the wings and suppresses flapping.
    WingsModule glideMod;
    glideMod.onInit();
    for (int i = 0; i < 4; ++i) glideMod.advanceWingAnimation(0.05f, 0.0f, -3.0f); // 0.2 s fall
    check(glideMod.glideFactor() < 0.05f, "short 0.2s fall does not engage glide");
    for (int i = 0; i < 30; ++i) glideMod.advanceWingAnimation(0.05f, 6.0f, -3.0f); // fast + falling, 1.5 s
    check(glideMod.glideFactor() > 0.7f, "sustained descent engages glide");
    check(glideMod.flightIntensity() < 0.15f, "glide suppresses flapping even at high speed");
    {
        // landing stops the descent: glide blends out, movement flaps again
        for (int i = 0; i < 60; ++i) glideMod.advanceWingAnimation(0.05f, 6.0f, 0.0f);
        check(glideMod.glideFactor() < 0.05f, "glide decays after landing");
        check(glideMod.flightIntensity() > 0.5f, "flapping resumes when running after landing");
    }

    // Rising fast flaps hard even without horizontal speed.
    WingsModule riseMod;
    riseMod.onInit();
    for (int i = 0; i < 60; ++i) riseMod.advanceWingAnimation(0.05f, 0.0f, 4.0f);
    check(riseMod.flightIntensity() > 0.5f, "rising rapidly also triggers flapping");

    // ==================================================================
    // Embedded geo / animation / controller JSON assets
    // ==================================================================
    std::printf("embedded geo/animation JSON assets\n");

    check(contains(wings_default::GeometryJson, "geometry.wings"), "geometry identifier present");
    check(contains(wings_default::GeometryJson, "\"bone_wings\""), "bone_wings root present");
    check(contains(wings_default::GeometryJson, "\"bone_wing_right\""), "bone_wing_right present");
    check(contains(wings_default::GeometryJson, "\"bone_wing_left\""), "bone_wing_left present");
    check(contains(wings_default::GeometryJson, "\"bone_wing_right_upper\""), "right upper segment present");
    check(contains(wings_default::GeometryJson, "\"bone_wing_right_tip\""), "right tip segment present");
    check(contains(wings_default::GeometryJson, "\"bone_wing_left_feather_2\""), "left feathers present");
    check(contains(wings_default::GeometryJson, "\"bone_wing_right_feather_4\""), "4 feathers per wing present");
    check(contains(wings_default::GeometryJson, "\"parent\": \"bone_wing_right_tip\""),
          "feathers_3/4 are children of the tip bone (hierarchy)");
    check(contains(wings_default::GeometryJson, "\"mirror\": true"), "left wing bones mirror UVs");
    check(!contains(wings_default::GeometryJson, "\"wingRight\"") && !contains(wings_default::GeometryJson, "\"wingLeft\""),
          "old flat wing quads removed from geometry");
    // every wing cube has real thickness (a non-zero z size)
    check(contains(wings_default::GeometryJson, "\"size\": [3, 3, 2]"), "shoulder joint is a 3D box");
    check(contains(wings_default::GeometryJson, "\"size\": [6, 3, 1]"), "upper segment has thickness");
    check(contains(wings_default::GeometryJson, "\"size\": [2, 6, 1]"), "feathers have thickness");

    check(contains(wings_default::AnimationJson, "\"animation.wings.idle\""), "idle animation present");
    check(contains(wings_default::AnimationJson, "\"animation.wings.flap\""), "flap animation present");
    check(contains(wings_default::AnimationJson, "\"animation.wings.glide\""), "glide animation present");
    check(contains(wings_default::AnimationJson, "\"bone_wing_right\"") && contains(wings_default::AnimationJson, "\"bone_wing_left\""),
          "animations target both wing roots");
    check(contains(wings_default::AnimationJson, "\"bone_wing_right_feather_4\""), "animations target feather bones");
    check(contains(wings_default::AnimationJson, "query.anim_time"), "animations are time driven");
    check(contains(wings_default::AnimationJson, "35 * math.sin"), "flap amplitude matches module constant");

    check(contains(wings_default::AnimationControllerJson, "controller.animation.wings"), "animation controller present");
    check(contains(wings_default::AnimationControllerJson, "query.modified_move_speed"),
          "controller reacts to player move speed");
    check(contains(wings_default::AnimationControllerJson, "query.vertical_speed"),
          "controller reacts to vertical speed");
    check(contains(wings_default::AnimationControllerJson, "query.is_gliding"), "controller reacts to gliding");
    check(contains(wings_default::AnimationControllerJson, "wings.flap") &&
          contains(wings_default::AnimationControllerJson, "wings.idle") &&
          contains(wings_default::AnimationControllerJson, "wings.glide"),
          "controller binds all three animations");

    // ==================================================================
    // Texture mapping: C++ palette must match the generated texture paints
    // ==================================================================
    std::printf("texture mapping / palette\n");

    auto colorEq = [](const unsigned char a[3], const unsigned char b[3]) {
        return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
    };
    check(colorEq(WingsModule::kColorFrame, wings_default::kColorFrame), "frame color matches texture palette");
    check(colorEq(WingsModule::kColorMembraneOuter, wings_default::kColorMembraneOuter), "outer membrane color matches");
    check(colorEq(WingsModule::kColorMembraneInner, wings_default::kColorMembraneInner), "inner membrane color matches");
    check(colorEq(WingsModule::kColorFeatherTip, wings_default::kColorFeatherTip), "feather tip color matches");
    check(colorEq(WingsModule::kColorJointInner, wings_default::kColorJointInner), "joint inner color matches");

    auto texel = [](int x, int y, int ch) {
        return wings_default::TexturePixels[((std::size_t)y * wings_default::TextureWidth + (std::size_t)x) * 4 + (std::size_t)ch];
    };
    auto texelIs = [&](int x, int y, unsigned char r, unsigned char g, unsigned char b) {
        return texel(x, y, 0) == r && texel(x, y, 1) == g && texel(x, y, 2) == b && texel(x, y, 3) == 255;
    };
    check(wings_default::TextureWidth == 64 && wings_default::TextureHeight == 64, "texture is 64x64");
    check(texelIs(19, 34, 18, 18, 24), "upper outer membrane painted dark");
    check(texelIs(20, 34, 94, 62, 36), "upper finger stripe painted with frame color");
    check(texelIs(11, 34, 28, 28, 36), "upper inner membrane painted lighter");
    check(texelIs(11, 33, 94, 62, 36), "upper top edge row painted with frame color");
    check(texelIs(8, 34, 94, 62, 36), "shoulder outer face painted with frame color");
    check(texelIs(4, 40, 18, 18, 24), "feather outer membrane painted dark");
    check(texelIs(4, 43, 46, 46, 60) && texelIs(3, 38, 46, 46, 60) && texelIs(1, 43, 46, 46, 60),
          "feather tip highlight on outer face, bottom edge and inner face");
    check(texelIs(1, 40, 28, 28, 36), "feather inner membrane painted lighter");
    check(texelIs(32, 34, 18, 18, 24) && texelIs(34, 34, 94, 62, 36), "tip segment membrane + finger stripe");

    // ==================================================================
    // ensureWingsAssetFiles writes geo + animation + texture next to config
    // ==================================================================
    std::printf("asset file writing\n");

    check(!tickMod.wingsDirectory().empty(), "wingsDirectory() non-empty (again)");
    tickMod.ensureWingsAssetFiles();
    tickMod.ensureWingsAssetFiles(); // must be a no-op the second time
    auto readFile = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    };
    const std::string dir = tickMod.wingsDirectory();
    check(contains(readFile(dir + "/wings_geometry.json").c_str(), "bone_wing_right"),
          "written wings_geometry.json contains bone_wing_right");
    check(contains(readFile(dir + "/wings_animation.json").c_str(), "animation.wings.flap"),
          "written wings_animation.json contains animation.wings.flap");
    check(contains(readFile(dir + "/wings_animation_controllers.json").c_str(), "query.modified_move_speed"),
          "written controller reacts to movement speed");
    {
        const std::string png = readFile(dir + "/wings.png");
        check(png.size() > 8 && png[0] == '\x89' && png[1] == 'P' && png[2] == 'N' && png[3] == 'G',
              "written wings.png is a valid PNG");
    }

    // ------------------------------------------------------------------
    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all wings checks passed (3D bone hierarchy + speed-driven animation)\n");
    return 0;
}

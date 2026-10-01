// Regression test for Hitbox partial-tick interpolation.
//
// The module used to draw the raw collision AABB, which is a 20 Hz tick
// sample, while the game draws the entity mesh at an interpolated render
// position (renderPos = lerp(prev, cur, partialTick)). A moving player's box
// therefore snapped a whole tick of movement ahead of the smoothly rendered
// body. The module now moves every box onto that render position, driven by
// the measured tick clock.
//
// This host test builds fake actor memory (StateVectorComponent + the
// AABBShapeComponent next to it) and drives the production code paths, so it
// covers the geometry helpers, the tick clock, the render hook output and the
// config plumbing without needing Minecraft.
//
// Build: g++ -std=c++20 -I include -I src -I tests/fakepl -I tests/fakejson
//        tests/hitbox_patch_test.cpp -o /tmp/hitbox_patch_test
// Run:   /tmp/hitbox_patch_test

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bedrocktools/events/EventBus.hpp"
#include "bedrocktools/memory/Signatures.hpp"
#include "bedrocktools/sdk/Offsets.hpp"
#include "bedrocktools/sdk/Types.hpp"

// Host stubs. No signature resolves, so the module takes its null-safe
// fallbacks (no materials, no occlusion, no drawing through the real game).
namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId) { return 0; }
} // namespace bedrocktools::memory

namespace bedrocktools::events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace bedrocktools::events

// Include the production implementation so this test can invoke its internal
// helpers without adding a test-only API to the module (same pattern as
// tests/blockoutline_test.cpp).
#include "modules/visual/hitbox.cpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (condition) {
        std::printf("  ok   %s\n", message);
    } else {
        std::printf("  FAIL %s\n", message);
        ++g_failures;
    }
}

bool near(float a, float b, float epsilon = 0.0001f) {
    return std::fabs(a - b) <= epsilon;
}

template <typename T, std::size_t N>
void writeAt(std::array<std::byte, N>& storage, std::size_t offset, const T& value) {
    std::memcpy(storage.data() + offset, &value, sizeof(T));
}

// Actor memory as the module reads it:
//   actor + Actor::mStateVectorComponent                        -> StateVectorComponent
//   actor + Actor::mStateVectorComponent + mAABBShapeComponent  -> AABBShapeComponent
// The component pointers sit next to each other in the actor, which is what
// the offsets of the repo describe (0x208 state vector, 0x210 AABB shape).
struct FakeActor {
    struct StateVector {
        bedrocktools::sdk::Vec3 pos{0.0f, 0.0f, 0.0f};
        bedrocktools::sdk::Vec3 prev{0.0f, 0.0f, 0.0f};
        bedrocktools::sdk::Vec3 delta{0.0f, 0.0f, 0.0f};
    };
    struct Shape {
        bedrocktools::sdk::AABB aabb{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    };

    FakeActor() {
        const auto stateAddr = reinterpret_cast<std::uintptr_t>(&state);
        const auto shapeAddr = reinterpret_cast<std::uintptr_t>(&shape);
        writeAt(bytes, bedrocktools::sdk::offsets::Actor::mStateVectorComponent, stateAddr);
        writeAt(bytes,
                bedrocktools::sdk::offsets::Actor::mStateVectorComponent +
                    bedrocktools::sdk::offsets::BuiltInActorComponents::mAABBShapeComponent,
                shapeAddr);
    }

    void* handle() { return bytes.data(); }

    alignas(16) std::array<std::byte, 0x230> bytes{};
    StateVector state{};
    Shape shape{};
};

// Tesselator capture: the render hook hands us world-space vertices relative
// to the camera, which this test keeps at the origin.
std::vector<bedrocktools::sdk::Vec3> g_vertices;
int g_beginCalls = 0;

void fakeTessBegin(void*, void*, int, int, int) {
    ++g_beginCalls;
    g_vertices.clear();
}
void fakeTessColor(void*, float, float, float, float) {}
void fakeTessVertex(void*, float x, float y, float z) {
    g_vertices.emplace_back(bedrocktools::sdk::Vec3{x, y, z});
}
void fakeRenderMesh(void*, void*, void*, char*) {}

DistanceSortedActor g_fetched[2];
int g_fetchedCount = 0;
ActorVec fakeFetchNearby(void*, void*, int) {
    return ActorVec{g_fetched, g_fetched + g_fetchedCount, g_fetched + 2};
}

struct DrawBounds {
    bool valid = false;
    float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
    float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
};

DrawBounds capturedBounds() {
    DrawBounds bounds;
    for (const auto& v : g_vertices) {
        if (!bounds.valid) {
            bounds = DrawBounds{true, v.x, v.y, v.z, v.x, v.y, v.z};
            continue;
        }
        bounds.minX = std::fmin(bounds.minX, v.x);
        bounds.minY = std::fmin(bounds.minY, v.y);
        bounds.minZ = std::fmin(bounds.minZ, v.z);
        bounds.maxX = std::fmax(bounds.maxX, v.x);
        bounds.maxY = std::fmax(bounds.maxY, v.y);
        bounds.maxZ = std::fmax(bounds.maxZ, v.z);
    }
    return bounds;
}

// Runs the production render hook with a minimal ScreenContext /
// LevelRenderer stand-in and returns the bounds of the drawn box.
DrawBounds drawOneFrame(FakeActor& actor, FakeActor& localPlayer) {
    alignas(16) std::array<std::byte, 0xC0> screenContext{};
    alignas(16) std::array<std::byte, 0x1100> levelRenderer{};
    alignas(16) std::array<std::byte, 0x1050> rendererPlayer{};
    alignas(16) std::array<float, 4> colorHolder{0.0f, 0.0f, 0.0f, 0.0f};

    writeAt(screenContext, bedrocktools::sdk::offsets::ScreenContext::mTessellator,
            reinterpret_cast<void*>(0x1234));
    writeAt(screenContext, bedrocktools::sdk::offsets::ScreenContext::mColorHolder,
            static_cast<void*>(colorHolder.data()));
    writeAt(levelRenderer, bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer,
            static_cast<void*>(rendererPlayer.data()));
    // Camera at the origin so captured vertices are world-space coordinates.
    writeAt(rendererPlayer, bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos,
            bedrocktools::sdk::Vec3{0.0f, 0.0f, 0.0f});

    g_vertices.clear();
    g_beginCalls = 0;
    g_fetched[0] = DistanceSortedActor{actor.handle(), 1.0f, 0.0f};
    g_fetchedCount = 1;

    g_localPlayerPtr = localPlayer.handle();
    s_tessBegin = fakeTessBegin;
    s_tessColor = fakeTessColor;
    s_tessVertex = fakeTessVertex;
    s_renderMesh = fakeRenderMesh;
    s_actorFetchNearby = fakeFetchNearby;

    _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
    return capturedBounds();
}

} // namespace

int main() {
    using namespace bedrocktools::sdk::offsets;

    std::printf("hitbox interpolated box geometry\n");

    FakeActor actor;
    actor.shape.aabb = {{10.0f, 64.0f, 10.0f}, {10.6f, 65.8f, 10.6f}};
    actor.state.pos = {10.0f, 64.0f, 10.0f};
    actor.state.prev = {9.5f, 64.0f, 10.0f};

    const AABB raw = getActorAABB(actor.handle());
    check(near(raw.min.x, 10.0f) && near(raw.max.x, 10.6f), "raw tick AABB reads from the component");

    const AABB atTick = getInterpolatedAABB(actor.handle(), 0.0f);
    check(near(atTick.min.x, 9.5f) && near(atTick.max.x, 10.1f),
          "at the tick the box sits on the previous tick position (the mesh position)");

    const AABB atHalf = getInterpolatedAABB(actor.handle(), 0.5f);
    check(near(atHalf.min.x, 9.75f) && near(atHalf.max.x, 10.35f),
          "halfway through the tick the box is halfway between the samples");

    const AABB atEnd = getInterpolatedAABB(actor.handle(), 1.0f);
    check(atEnd.min.x == raw.min.x && atEnd.max.x == raw.max.x,
          "alpha 1 is byte-for-byte the old tick AABB");

    check(atHalf.min.y == raw.min.y && atHalf.max.y == raw.max.y &&
              atHalf.min.z == raw.min.z && atHalf.max.z == raw.max.z,
          "only the moving axis is shifted");

    // Teleports and bad reads must fall back to the authoritative tick box.
    actor.state.prev = {10.0f, 64.0f, 60.0f};
    const AABB teleported = getInterpolatedAABB(actor.handle(), 0.0f);
    check(teleported.min.z == raw.min.z && teleported.max.z == raw.max.z,
          "implausible tick delta falls back to the raw tick AABB");
    actor.state.prev = {9.5f, 64.0f, 10.0f};

    // A position that does not belong to the box (wrong build / stale
    // component) must not drag the box anywhere.
    actor.state.pos = {10.3f, 80.0f, 10.3f};
    actor.state.prev = {10.0f, 80.0f, 10.3f};
    const AABB mismatched = getInterpolatedAABB(actor.handle(), 0.0f);
    check(mismatched.min.y == raw.min.y && mismatched.max.y == raw.max.y,
          "position outside the box falls back to the raw tick AABB");
    actor.state.pos = {10.0f, 64.0f, 10.0f};
    actor.state.prev = {9.5f, 64.0f, 10.0f};

    alignas(16) std::array<std::byte, 0x230> empty{};
    const AABB missing = getInterpolatedAABB(empty.data(), 0.5f);
    check(missing.min.x == 0.0f && missing.max.x == 0.0f,
          "missing components fall back to an empty box instead of crashing");

    std::printf("hitbox tick clock\n");

    FakeActor localPlayer;
    localPlayer.shape.aabb = {{0.0f, 64.0f, 0.0f}, {0.6f, 65.8f, 0.6f}};
    localPlayer.state.pos = {0.0f, 64.0f, 0.0f};
    localPlayer.state.prev = {0.0f, 64.0f, 0.0f};

    HitboxModule mod;
    check(mod.smoothBoxes, "smooth boxes on by default");

    s_lastTickTimeValid = false;
    check(currentPartialTick() == 1.0f, "no tick sample yet -> raw tick boxes");

    s_lastTickTime = std::chrono::steady_clock::now();
    s_tickInterval = 0.05f;
    s_lastTickTimeValid = true;
    const float fresh = currentPartialTick();
    check(fresh >= 0.0f && fresh < 0.1f, "fresh tick -> near 0 (box on the previous sample)");

    s_lastTickTime = std::chrono::steady_clock::now() - std::chrono::milliseconds(25);
    const float halfway = currentPartialTick();
    check(halfway > 0.4f && halfway < 0.7f, "halfway through the tick -> ~0.5");

    s_lastTickTime = std::chrono::steady_clock::now() - std::chrono::milliseconds(400);
    check(currentPartialTick() == 1.0f, "stale tick (hitch) is clamped to raw tick boxes");

    mod.smoothBoxes = false;
    s_lastTickTime = std::chrono::steady_clock::now() - std::chrono::milliseconds(25);
    check(currentPartialTick() == 1.0f, "menu toggle off -> raw tick boxes");
    mod.smoothBoxes = true;

    // The tick callback owns the clock: it measures the real interval instead
    // of assuming 20 Hz, and it never interpolates across a disabled gap.
    mod.enabled = true;
    s_lastTickTimeValid = false;
    s_tickInterval = 0.05f;
    s_hitboxTickCallback(actor.handle());
    check(s_lastTickTimeValid && g_localPlayerPtr == actor.handle(),
          "tick callback starts the clock and records the player");

    s_lastTickTime = std::chrono::steady_clock::now() - std::chrono::milliseconds(20);
    s_hitboxTickCallback(actor.handle());
    check(s_tickInterval > 0.015f && s_tickInterval < 0.03f,
          "measured interval replaces the 0.05 assumption");

    s_lastTickTime = std::chrono::steady_clock::now() - std::chrono::milliseconds(900);
    s_hitboxTickCallback(actor.handle());
    check(s_tickInterval > 0.015f && s_tickInterval < 0.03f,
          "a 0.9 s hitch is not adopted as the tick interval");

    mod.enabled = false;
    s_hitboxTickCallback(actor.handle());
    check(!s_lastTickTimeValid, "disabled module clears the clock");

    std::printf("hitbox render hook output\n");

    mod.enabled = true;
    mod.smoothBoxes = true;
    mod.showItems = true;
    s_lastTickTime = std::chrono::steady_clock::now();
    s_tickInterval = 0.05f;
    s_lastTickTimeValid = true;

    const DrawBounds drawnAtTick = drawOneFrame(actor, localPlayer);
    check(drawnAtTick.valid && near(drawnAtTick.minX, 9.5f) && near(drawnAtTick.maxX, 10.1f),
          "render hook draws the box at the interpolated position");

    // Mid-tick: the box must be between the two tick samples, not snapped.
    s_lastTickTime = std::chrono::steady_clock::now() - std::chrono::milliseconds(25);
    const DrawBounds drawnMidTick = drawOneFrame(actor, localPlayer);
    check(drawnMidTick.valid && drawnMidTick.minX > 9.55f && drawnMidTick.minX < 9.95f &&
              drawnMidTick.minX < drawnAtTick.minX + 0.001f + 0.4f,
          "render hook interpolates between the tick samples");

    // Toggle off: the drawn box is the raw tick AABB again.
    mod.smoothBoxes = false;
    s_lastTickTime = std::chrono::steady_clock::now();
    const DrawBounds drawnOff = drawOneFrame(actor, localPlayer);
    check(drawnOff.valid && near(drawnOff.minX, 10.0f) && near(drawnOff.maxX, 10.6f),
          "smooth boxes off -> raw tick AABB is drawn");
    mod.smoothBoxes = true;

    std::printf("hitbox config round-trip\n");

    {
        HitboxModule configured;
        nlohmann::json saved;
        configured.saveConfig(saved);
        check(saved.contains("smoothBoxes") && saved["smoothBoxes"].get<bool>(),
              "saveConfig persists smoothBoxes (on by default)");

        nlohmann::json incoming;
        incoming["smoothBoxes"] = false;
        configured.loadConfig(incoming);
        check(!configured.smoothBoxes, "loadConfig reads smoothBoxes");

        HitboxModule legacy;
        nlohmann::json oldConfig;
        oldConfig["showPlayers"] = false;
        legacy.loadConfig(oldConfig);
        check(legacy.smoothBoxes, "config saved before the option existed keeps smoothing on");
    }

    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all hitbox interpolation checks passed\n");
    return 0;
}

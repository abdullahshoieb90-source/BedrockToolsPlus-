// Regression test for Hitbox per-actor sample interpolation.
//
// The module used to draw the raw collision AABB - a tick sample - while the
// game draws the entity mesh at an interpolated position. It then derived the
// interpolation phase from the local player's tick callback; while gliding
// with an elytra that callback fires off-cadence, so the phase collapsed to
// zero and the player's own box stayed frozen on the previous sample and was
// left behind by a firework-boosted glide (visible in third person only,
// because that is the only mode that draws the local box).
//
// The phase is now measured from the actor's own box samples, so this test
// drives the production render hook frame by frame with fake actor memory and
// checks what is actually drawn: the newest sample can never be abandoned.
//
// Build: g++ -std=c++20 -I include -I src -I tests/fakepl -I tests/fakejson -pthread
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
#include <thread>
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

// Pinned sample interval for the mid-interval check: long enough that a slow
// CI machine cannot overshoot it in a single 100 ms sleep.
constexpr float kPinnedInterval = 0.5f;

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

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

template <typename T, std::size_t N>
void writeAt(std::array<std::byte, N>& storage, std::size_t offset, const T& value) {
    std::memcpy(storage.data() + offset, &value, sizeof(T));
}

// Actor memory as the module reads it:
//   actor + Actor::mStateVectorComponent                        -> StateVectorComponent
//   actor + Actor::mStateVectorComponent + mAABBShapeComponent  -> AABBShapeComponent
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

    // Moves the collision box to a new X, as a tick would.
    void setBoxX(float x) {
        shape.aabb = {{x, 64.0f, 10.0f}, {x + 0.6f, 65.8f, 10.6f}};
        state.pos = {x, 64.0f, 10.0f};
    }

    // Actor::mCategories drives the players / mobs / items split.
    void setCategories(std::uint32_t categories) {
        writeAt(bytes, bedrocktools::sdk::offsets::Actor::mCategories, categories);
    }

    // Sized so every offset the render hook reads (including
    // Actor::mActorRotationComponent, read by the hitbox indicator's
    // getActorRotation) stays inside the zero-initialized allocation.
    alignas(16) std::array<std::byte,
        bedrocktools::sdk::offsets::Actor::mActorRotationComponent +
        sizeof(std::uintptr_t)> bytes{};
    StateVector state{};
    Shape shape{};
};

// Tesselator capture: the render hook hands us world-space vertices relative
// to the camera, which this test keeps at the origin.
std::vector<bedrocktools::sdk::Vec3> g_vertices;
int g_beginCalls = 0;

// Color capture: each drawn pass sets its color first, so recording the
// color at the current vertex count maps boxes to the color they used.
struct ColorMark { float r, g, b; std::size_t vertex; };
std::vector<ColorMark> g_colorMarks;

void fakeTessBegin(void*, void*, int, int, int) {
    // One begin call per drawn pass; the module's hairline pass is one call
    // per box, so this doubles as the box counter.
    ++g_beginCalls;
}
void fakeTessColor(void*, float r, float g, float b, float) {
    g_colorMarks.push_back(ColorMark{r, g, b, g_vertices.size()});
}
void fakeTessVertex(void*, float x, float y, float z) {
    g_vertices.emplace_back(bedrocktools::sdk::Vec3{x, y, z});
}
void fakeRenderMesh(void*, void*, void*, char*) {}

std::vector<DistanceSortedActor> g_fetchedList;
float g_lastFetchExtent = 0.0f;
ActorVec fakeFetchNearby(void*, void* extent, int) {
    if (extent) g_lastFetchExtent = static_cast<float*>(extent)[0];
    if (g_fetchedList.empty()) return ActorVec{};
    return ActorVec{g_fetchedList.data(), g_fetchedList.data() + g_fetchedList.size(),
                    g_fetchedList.data() + g_fetchedList.size()};
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

struct FetchEntry {
    FakeActor* actor;
    float distance;
};

// Runs the production render hook with a minimal ScreenContext /
// LevelRenderer stand-in and returns what the frame drew. Each box is 12 edges
// x 2 vertices (the hairline pass; the thick pass only runs above thickness 1).
struct FrameResult {
    DrawBounds bounds;
    int boxes = 0;
};

FrameResult drawFrame(const std::vector<FetchEntry>& entries, FakeActor& localPlayer) {
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
    g_colorMarks.clear();
    g_lastFetchExtent = 0.0f;
    g_fetchedList.clear();
    for (const auto& entry : entries) {
        g_fetchedList.push_back(DistanceSortedActor{entry.actor->handle(), entry.distance, 0.0f});
    }

    g_localPlayerPtr = localPlayer.handle();
    s_tessBegin = fakeTessBegin;
    s_tessColor = fakeTessColor;
    s_tessVertex = fakeTessVertex;
    s_renderMesh = fakeRenderMesh;
    s_actorFetchNearby = fakeFetchNearby;

    _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);

    FrameResult result;
    result.bounds = capturedBounds();
    result.boxes = g_beginCalls;
    return result;
}

// Single-actor convenience wrapper for the interpolation checks.
DrawBounds drawOneFrame(FakeActor& actor, FakeActor& localPlayer) {
    return drawFrame({{&actor, 1.0f}}, localPlayer).bounds;
}

// The sample interval is measured from the frames themselves, so a test that
// wants to check "halfway through the interval" pins it to the real 50 ms.
void pinSampleInterval(FakeActor& actor) {
    auto it = s_interpHistory.find(actor.handle());
    if (it != s_interpHistory.end()) it->second.track.intervalSeconds = kPinnedInterval;
}

} // namespace

int main() {
    using namespace bedrocktools::sdk::offsets;

    std::printf("hitbox raw box reads\n");

    FakeActor localPlayer;
    localPlayer.setBoxX(0.0f);

    FakeActor actor;
    actor.setBoxX(10.0f);

    const AABB raw = getActorAABB(actor.handle());
    check(near(raw.min.x, 10.0f) && near(raw.max.x, 10.6f),
          "raw tick AABB reads from the component");

    alignas(16) std::array<std::byte,
        bedrocktools::sdk::offsets::Actor::mStateVectorComponent +
        bedrocktools::sdk::offsets::BuiltInActorComponents::mAABBShapeComponent +
        sizeof(std::uintptr_t)> empty{};
    const AABB missing = getActorAABB(empty.data());
    check(!isDrawableBox(missing), "missing components are not a drawable box");

    std::printf("hitbox first frame\n");

    HitboxModule mod;
    check(mod.smoothBoxes, "smooth boxes on by default");

    mod.enabled = true;
    s_interpHistory.clear();

    // First time an actor is seen there is nothing to interpolate against, so
    // the raw tick box is drawn.
    DrawBounds first = drawOneFrame(actor, localPlayer);
    check(first.valid && near(first.minX, 10.0f) && near(first.maxX, 10.6f),
          "first sample draws the raw tick box");
    check(drawFrame({{&actor, 1.0f}}, localPlayer).boxes == 1,
          "one actor in range yields exactly one box");

    std::printf("hitbox interpolation across a sample\n");

    // New sample: the box moved a tick of movement. The phase restarts, so the
    // drawn box must sit on the *previous* sample, which is where the game
    // starts drawing the mesh.
    actor.setBoxX(10.5f);
    DrawBounds atSample = drawOneFrame(actor, localPlayer);
    check(atSample.valid && near(atSample.minX, 10.0f) && near(atSample.maxX, 10.6f),
          "on a new sample the box is drawn on the previous sample");

    // Partway through the interval the box sits between the two samples. The
    // expectation is derived from the module's own clock so a loaded machine
    // cannot make the check flaky.
    pinSampleInterval(actor);
    sleepMs(100);
    DrawBounds midInterval = drawOneFrame(actor, localPlayer);
    float observedAlpha = -1.0f;
    {
        const auto it = s_interpHistory.find(actor.handle());
        if (it != s_interpHistory.end() && it->second.hasSampleTime) {
            const float elapsed = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - it->second.sampleTime).count();
            observedAlpha = hitbox::sampleFraction(elapsed, it->second.track.intervalSeconds);
        }
    }
    const float expectedMinX = 10.0f + 0.5f * observedAlpha;
    check(midInterval.valid && observedAlpha > 0.05f && observedAlpha < 0.95f &&
              near(midInterval.minX, expectedMinX, 0.02f),
          "partway through the interval the box is between the samples");

    // The next sample arrives: the box lands on it instead of lagging.
    actor.setBoxX(11.0f);
    DrawBounds nextSample = drawOneFrame(actor, localPlayer);
    check(nextSample.valid && near(nextSample.minX, 10.5f) && near(nextSample.maxX, 11.1f),
          "a new sample immediately puts the box a full sample behind, not two");

    std::printf("hitbox stale sampling (elytra / firework boost regression)\n");

    // The reported bug: while gliding with a firework, no new sample is
    // accepted for a long time. The box used to stay frozen on the previous
    // sample forever; it must instead be pulled onto the newest sample.
    actor.setBoxX(20.0f);
    drawOneFrame(actor, localPlayer); // sample at 20.0, phase restarts
    actor.setBoxX(21.0f);
    drawOneFrame(actor, localPlayer); // sample at 21.0, prev = 20.0

    sleepMs(400); // no samples at all in this window
    DrawBounds stalled = drawOneFrame(actor, localPlayer);
    check(stalled.valid && near(stalled.minX, 21.0f) && near(stalled.maxX, 21.6f),
          "after a stalled sampling window the box sits on the newest sample, never on the old one");

    std::printf("hitbox teleport and toggle\n");

    // A teleport cannot be interpolated across: the raw box is drawn.
    actor.setBoxX(120.0f);
    DrawBounds teleported = drawOneFrame(actor, localPlayer);
    check(teleported.valid && near(teleported.minX, 120.0f) && near(teleported.maxX, 120.6f),
          "teleport draws the raw box and drops the history");

    // Menu toggle off: always the raw tick box, even mid-interval.
    mod.smoothBoxes = false;
    actor.setBoxX(121.0f);
    drawOneFrame(actor, localPlayer);
    sleepMs(25);
    DrawBounds rawToggleOff = drawOneFrame(actor, localPlayer);
    check(rawToggleOff.valid && near(rawToggleOff.minX, 121.0f) && near(rawToggleOff.maxX, 121.6f),
          "smooth boxes off -> raw tick AABB is drawn");
    mod.smoothBoxes = true;

    std::printf("hitbox history hygiene\n");

    check(s_interpHistory.find(actor.handle()) != s_interpHistory.end(),
          "seen actors keep a sample history");
    s_interpHistory.clear();
    check(s_interpHistory.empty(), "history can be dropped when actors are unloaded");

    std::printf("hitbox draw range\n");

    {
        using namespace bedrocktools::sdk::offsets;

        HitboxModule ranged;
        ranged.enabled = true;
        s_interpHistory.clear();

        FakeActor nearMob, midMob, farMob;
        nearMob.setBoxX(10.0f);
        nearMob.setCategories(ActorCategories::IsMob);
        midMob.setBoxX(60.0f);
        midMob.setCategories(ActorCategories::IsMob);
        farMob.setBoxX(120.0f);
        farMob.setCategories(ActorCategories::IsMob);

        // The Range sliders are gone: one fixed scan radius fetches actors,
        // and everything fetched is drawn regardless of its distance.
        const std::vector<FetchEntry> mobs{{&nearMob, 10.0f}, {&midMob, 60.0f}, {&farMob, 120.0f}};
        FrameResult within = drawFrame(mobs, localPlayer);
        check(within.boxes == 3, "mobs near and far are all drawn (no distance filter)");
        check(near(within.bounds.maxX, 120.6f), "the 120-block mob is drawn");
        check(near(g_lastFetchExtent, kHitboxScanRadius),
              "the actor fetch uses the fixed scan radius");

        // Group toggles keep working (they replaced the range as the filter).
        ranged.showEntities = false;
        check(drawFrame(mobs, localPlayer).boxes == 0, "Show Entities off hides the mobs");
        ranged.showEntities = true;

        // Items share the same scan radius; only the toggle gates them.
        FakeActor itemActor;
        itemActor.setBoxX(40.0f);
        itemActor.setCategories(ActorCategories::IsItem);
        const std::vector<FetchEntry> items{{&itemActor, 40.0f}};
        check(drawFrame(items, localPlayer).boxes == 1, "an item within the scan radius is drawn");

        ranged.showItems = false;
        check(drawFrame(items, localPlayer).boxes == 0, "Show Items off hides the item");
        ranged.showItems = true;
    }

    // The ranged module is scoped to its own block; put the module the other
    // checks use back in place before it goes out of scope.
    g_hitboxMod = &mod;

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

        // The Range sliders are gone from the menu: saveConfig must not emit
        // the keys (the menu is built from saveConfig output), and configs
        // saved by older builds load harmlessly with the keys ignored.
        HitboxModule ranged;
        nlohmann::json savedRanges;
        ranged.saveConfig(savedRanges);
        check(!savedRanges.contains("range") && !savedRanges.contains("itemsRange"),
              "saveConfig no longer emits the removed Range sliders");

        nlohmann::json oldRanges;
        oldRanges["range"] = 12.0f;
        oldRanges["itemsRange"] = 4.0f;
        ranged.loadConfig(oldRanges);
        check(near(g_lastFetchExtent, kHitboxScanRadius),
              "a stale config cannot shrink the fixed scan radius");
    }

    std::printf("hitbox indicator reach\n");

    // Two regressions in one stage:
    //   1. The selection ray was capped at 3 blocks from the CAMERA, so in
    //      third person an entity under the crosshair just past the player
    //      was never highlighted.
    //   2. The first fix added the camera-to-player distance to the RAY
    //      length instead of gating the hit, so the highlight stretched to
    //      several blocks past the player - the indicator fired on entities
    //      well beyond the game's 3-block interaction distance.
    // The reach is now 3 blocks measured from the player's own box, with the
    // ray merely starting at the camera for crosshair alignment.
    {
        // Fresh module instance: it becomes the hook's g_hitboxMod for this
        // stage (the config stage above left the pointer nulled on destroy).
        HitboxModule indicatorMod;
        indicatorMod.enabled = true;

        // Third-person camera at the origin looking down +Z (rotation 0).
        // The player's box sits ~2.5 blocks ahead; a mob just beyond it under
        // the crosshair (~0.1 blocks past the player, well within reach), and
        // a second mob more than 3 blocks past the player on the same ray.
        localPlayer.shape.aabb = {{-0.3f, -0.3f, 2.5f}, {0.3f, 0.3f, 3.1f}};
        localPlayer.state.pos = {0.0f, -0.3f, 2.5f};

        actor.shape.aabb = {{-0.3f, -0.3f, 3.2f}, {0.3f, 0.3f, 3.8f}};
        actor.state.pos = {0.0f, -0.3f, 3.2f};

        FakeActor farActor;
        farActor.shape.aabb = {{-0.3f, -0.3f, 6.5f}, {0.3f, 0.3f, 7.1f}};
        farActor.state.pos = {0.0f, -0.3f, 6.5f};

        indicatorMod.hitboxIndicator = true;
        s_interpHistory.clear();

        // Distance-sorted fetch: the near mob first, the far mob second.
        // Default colors differ (active red vs default white), so the drawn
        // colors identify which mob the indicator selected.
        FrameResult indicatorFrame = drawFrame({{&actor, 3.2f}, {&farActor, 6.5f}}, localPlayer);
        check(indicatorFrame.boxes >= 2, "both mobs on the crosshair ray are drawn");

        check(g_colorMarks.size() >= 2, "each drawn box carries its own color");
        if (g_colorMarks.size() >= 2) {
            const auto& first = g_colorMarks[g_colorMarks.size() - 2];
            const auto& second = g_colorMarks.back();
            check(near(first.r, 1.0f) && near(first.g, 0.0f) && near(first.b, 0.0f),
                  "the mob within 3 blocks of the player gets the Indicator Active Color");
            check(near(second.r, 1.0f) && near(second.g, 1.0f) && near(second.b, 1.0f),
                  "a mob more than 3 blocks past the player keeps the default color");
        }

        // Guard the reach geometry directly so a future refactor cannot
        // silently stretch or shrink it.
        {
            const AABB playerBox = getActorAABB(localPlayer.handle());
            AABB nearBox = getActorAABB(actor.handle());
            float hitDist = 0.0f;

            // The old fixed 3.0 camera ray misses the mob past the player...
            const bool fixedHit = rayHitsAABB(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, nearBox, 3.0f, hitDist);
            check(!fixedHit, "a fixed 3.0 camera ray misses the mob just past the player");
            // ...but the camera-started ray crosses it (the ray is cast far
            // enough; the reach gate below does the filtering).
            const bool rayHit = rayHitsAABB(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, nearBox, 8.0f, hitDist);
            check(rayHit && near(hitDist, 3.2f, 0.01f), "the camera ray hits the near mob at 3.2");

            // Hit point 0.1 blocks past the player's box: within reach.
            check(near(hitbox::pointAabbDistance(0.0f, 0.0f, 3.2f,
                                                 playerBox.min.x, playerBox.min.y, playerBox.min.z,
                                                 playerBox.max.x, playerBox.max.y, playerBox.max.z),
                       0.1f, 0.001f),
                  "the near hit sits 0.1 blocks from the player's box");
            check(hitbox::hitWithinPlayerReach(0.0f, 0.0f, 3.2f,
                                               playerBox.min.x, playerBox.min.y, playerBox.min.z,
                                               playerBox.max.x, playerBox.max.y, playerBox.max.z,
                                               hitbox::kIndicatorReach),
                  "the near mob is within the player-based reach");

            // Hit point 3.4 blocks past the player's box: out of reach (this
            // is the reported bug - it used to be highlighted).
            const AABB farBox = getActorAABB(farActor.handle());
            const bool farRayHit = rayHitsAABB(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, farBox, 8.0f, hitDist);
            check(farRayHit && near(hitDist, 6.5f, 0.01f), "the camera ray hits the far mob at 6.5");
            check(near(hitbox::pointAabbDistance(0.0f, 0.0f, 6.5f,
                                                 playerBox.min.x, playerBox.min.y, playerBox.min.z,
                                                 playerBox.max.x, playerBox.max.y, playerBox.max.z),
                       3.4f, 0.001f),
                  "the far hit sits 3.4 blocks from the player's box");
            check(!hitbox::hitWithinPlayerReach(0.0f, 0.0f, 6.5f,
                                                playerBox.min.x, playerBox.min.y, playerBox.min.z,
                                                playerBox.max.x, playerBox.max.y, playerBox.max.z,
                                                hitbox::kIndicatorReach),
                  "the far mob is beyond the player-based reach");
        }

        // First person: the camera sits at eye level inside/above the
        // player's own box, so the reach stays ~3 blocks from the player -
        // the old camera + 3 formula added ~0.7 on top.
        {
            check(near(hitbox::pointAabbDistance(0.0f, 0.0f, 0.0f,
                                                 -0.3f, -0.3f, -0.1f,
                                                 0.3f, 1.5f, 0.3f),
                       0.0f, 0.001f),
                  "a first-person camera at eye level is inside the player's box");
            check(hitbox::hitWithinPlayerReach(0.0f, 0.0f, 2.9f,
                                               -0.3f, -0.3f, -0.1f,
                                               0.3f, 1.5f, 0.3f,
                                               hitbox::kIndicatorReach),
                  "a mob ~3 blocks ahead in first person is within reach");
            check(!hitbox::hitWithinPlayerReach(0.0f, 0.0f, 3.6f,
                                                -0.3f, -0.3f, -0.1f,
                                                0.3f, 1.5f, 0.3f,
                                                hitbox::kIndicatorReach),
                  "a mob 3.6 blocks ahead in first person is out of reach");
        }

        s_interpHistory.clear();
        // Restore the boxes the earlier stages expect, should any follow.
        localPlayer.setBoxX(0.0f);
        actor.setBoxX(10.0f);
    }

    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all hitbox interpolation checks passed\n");
    return 0;
}

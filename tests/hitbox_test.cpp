// Integration-style host test for the Hitbox module.
//
// It drives the real render hook and the real tick callback with small
// in-memory Minecraft stand-ins (actor, entity context components, block
// source, screen context, level renderer) and verifies that the module emits
// the expected tessellator batches, that its filters and the wall-occlusion
// cull behave, and that its settings survive a config round-trip.
//
// Build: g++ -std=c++20 -I include -I src -I tests/fakepl -I tests/fakejson
//        tests/hitbox_test.cpp -o /tmp/hitbox_test
// Run:   /tmp/hitbox_test

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "bedrocktools/events/EventBus.hpp"
#include "bedrocktools/events/LocalPlayerTickEvent.hpp"
#include "bedrocktools/memory/Signatures.hpp"
#include "bedrocktools/sdk/Offsets.hpp"
#include "bedrocktools/sdk/Types.hpp"

namespace {

using bedrocktools::sdk::Vec2;
using bedrocktools::sdk::Vec3;

struct Batch {
    int mode = -1;
    int reservedVertices = 0;
    int emittedVertices = 0;
};

std::vector<Batch> g_batches;
Batch g_currentBatch;

// Stand-in world: a map of "solid" block coordinates used by the occlusion
// ray cast.
std::vector<std::array<int, 3>> g_solidBlocks;
int g_solidBlockQueries = 0;

void* g_fetchedActors[8] = {nullptr};
int g_fetchedActorCount = 0;
std::vector<void*> g_fetchedStorage;
bool g_reportPlayer = false;
bool g_reportInvisible = false;
int g_perspective = 0;

void fakeRenderLevel(void*, void*, void*) {}

void fakeTessBegin(void*, void*, int mode, int vertexCount, int) {
    g_currentBatch = {mode, vertexCount, 0};
}
void fakeTessColor(void*, float, float, float, float) {}
void fakeTessVertex(void*, float, float, float) { ++g_currentBatch.emittedVertices; }
void fakeRenderMesh(void*, void*, void*, char*) { g_batches.push_back(g_currentBatch); }

bool fakeIsPlayer(void*) { return g_reportPlayer; }
bool fakeIsInvisible(void*) { return g_reportInvisible; }

// Mirrors the production ActorVec / DistanceSortedActor layout.
struct FakeSortedActor {
    void* actor;
    float distance;
    float pad;
};
struct FakeActorVec {
    FakeSortedActor* begin;
    FakeSortedActor* end;
    FakeSortedActor* cap;
};

FakeActorVec fakeFetchNearby(void*, void*, int) {
    g_fetchedStorage.clear();
    for (int i = 0; i < g_fetchedActorCount; ++i) {
        g_fetchedStorage.push_back(g_fetchedActors[i]);
    }
    FakeActorVec out{};
    if (g_fetchedStorage.empty()) return out;
    // The production code only reads the actor pointer out of each element,
    // so a vector of pointers is enough to stand in for the sorted list.
    out.begin = reinterpret_cast<FakeSortedActor*>(g_fetchedStorage.data());
    out.end = out.begin + g_fetchedStorage.size();
    out.cap = out.end;
    return out;
}

bool fakeIsSolidBlockingBlock(void*, const void* pos) {
    ++g_solidBlockQueries;
    const auto* blockPos = static_cast<const int*>(pos);
    for (const auto& solid : g_solidBlocks) {
        if (solid[0] == blockPos[0] && solid[1] == blockPos[1] && solid[2] == blockPos[2]) return true;
    }
    return false;
}

int fakeGetPerspective(void*) { return g_perspective; }

std::uintptr_t fakeResolve(bedrocktools::memory::SignatureId id) {
    using bedrocktools::memory::SignatureId;
    switch (id) {
        case SignatureId::RenderLevel: return reinterpret_cast<std::uintptr_t>(&fakeRenderLevel);
        case SignatureId::TessellatorBegin: return reinterpret_cast<std::uintptr_t>(&fakeTessBegin);
        case SignatureId::TessellatorColor: return reinterpret_cast<std::uintptr_t>(&fakeTessColor);
        case SignatureId::TessellatorVertex: return reinterpret_cast<std::uintptr_t>(&fakeTessVertex);
        case SignatureId::MeshHelpersRenderMeshImmediately2: return reinterpret_cast<std::uintptr_t>(&fakeRenderMesh);
        case SignatureId::ActorIsPlayer: return reinterpret_cast<std::uintptr_t>(&fakeIsPlayer);
        case SignatureId::ActorIsInvisible: return reinterpret_cast<std::uintptr_t>(&fakeIsInvisible);
        case SignatureId::ActorFetchNearbyActorsSorted: return reinterpret_cast<std::uintptr_t>(&fakeFetchNearby);
        case SignatureId::BlockSourceIsSolidBlockingBlock: return reinterpret_cast<std::uintptr_t>(&fakeIsSolidBlockingBlock);
        case SignatureId::GetPerspective: return reinterpret_cast<std::uintptr_t>(&fakeGetPerspective);
        default: return 0;
    }
}

template <typename T, std::size_t N>
void writeAt(std::array<std::byte, N>& storage, std::size_t offset, const T& value) {
    std::memcpy(storage.data() + offset, &value, sizeof(T));
}

} // namespace

namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId id) { return fakeResolve(id); }
} // namespace bedrocktools::memory

namespace bedrocktools::events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace bedrocktools::events

// Include the production implementation so this test can drive its internal
// render hook and tick callback without adding a test-only API to the module.
#include "modules/visual/hitbox.cpp"

namespace {

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) {
        std::printf("  ok   %s\n", message.c_str());
    } else {
        std::printf("  FAIL %s\n", message.c_str());
        ++g_failures;
    }
}

bool near(float a, float b, float epsilon = 0.0001f) { return std::fabs(a - b) <= epsilon; }

// Builds a fake actor whose AABBShapeComponent reports the given box.
// The actor keeps its built-in component pointers in an inline array that
// starts at Actor::mStateVectorComponent (StateVectorComponent, then
// AABBShapeComponent, then ActorRotationComponent), which is why
// Actor::mActorRotationComponent == mStateVectorComponent + 16.
struct FakeActor {
    alignas(std::max_align_t) std::array<std::byte, 1024> actor{};
    alignas(std::max_align_t) std::array<std::byte, 64> stateVector{};
    alignas(std::max_align_t) std::array<std::byte, 64> aabbShape{};
    alignas(std::max_align_t) std::array<std::byte, 16> rotation{};

    FakeActor(const Vec3& min, const Vec3& max, std::uint32_t categories = 0, const Vec2& rot = {0.0f, 0.0f}) {
        bedrocktools::sdk::AABB box{min, max};
        writeAt(aabbShape, bedrocktools::sdk::offsets::AABBShapeComponent::mAABB, box);
        writeAt(actor, bedrocktools::sdk::offsets::Actor::mStateVectorComponent,
                static_cast<void*>(stateVector.data()));
        writeAt(actor,
                bedrocktools::sdk::offsets::Actor::mStateVectorComponent +
                    bedrocktools::sdk::offsets::BuiltInActorComponents::mAABBShapeComponent,
                static_cast<void*>(aabbShape.data()));
        writeAt(rotation, 0, rot);
        writeAt(actor, bedrocktools::sdk::offsets::Actor::mActorRotationComponent,
                static_cast<void*>(rotation.data()));
        writeAt(actor, bedrocktools::sdk::offsets::Actor::mCategories, categories);
    }

    void attachWorld(void* dimension) {
        writeAt(actor, bedrocktools::sdk::offsets::Actor::mDimension,
                reinterpret_cast<std::uintptr_t>(dimension));
    }

    void* ptr() { return actor.data(); }
};

} // namespace

int main() {
    using namespace bedrocktools::sdk::offsets;

    std::printf("hitbox render integration\n");

    alignas(std::max_align_t) std::array<std::byte, 256> screenContext{};
    alignas(std::max_align_t) std::array<std::byte, 0x500> levelRenderer{};
    alignas(std::max_align_t) std::array<std::byte, 0x1100> playerRenderer{};
    alignas(std::max_align_t) std::array<float, 4> colorHolder{0.2f, 0.3f, 0.4f, 0.5f};
    std::uint64_t tessellator = 0;

    writeAt(screenContext, ScreenContext::mTessellator, static_cast<void*>(&tessellator));
    writeAt(screenContext, ScreenContext::mColorHolder, static_cast<void*>(colorHolder.data()));
    writeAt(levelRenderer, LevelRenderer::mLevelRendererPlayer, static_cast<void*>(playerRenderer.data()));

    // The local player stands at (10.0, 64.0, 10.0) with the standard 1.8 tall
    // box; the camera sits at its eyes in first person.
    FakeActor localPlayer({10.0f, 64.0f, 10.0f}, {10.6f, 65.8f, 10.6f});
    const Vec3 firstPersonCamera{10.3f, 65.62f, 10.3f};
    writeAt(playerRenderer, LevelRendererPlayer::mCamPos, firstPersonCamera);

    // A mob five blocks away, in clear air.
    FakeActor mob({15.0f, 64.0f, 10.0f}, {15.9f, 66.0f, 10.9f}, ActorCategories::IsMob);

    // A dimension whose BlockSource the wall-occlusion ray cast can query.
    alignas(std::max_align_t) std::array<std::byte, 512> dimension{};
    alignas(std::max_align_t) std::array<std::byte, 64> blockSource{};
    writeAt(dimension, Dimension::mBlockSource, static_cast<void*>(blockSource.data()));
    localPlayer.attachWorld(dimension.data());
    mob.attachWorld(dimension.data());

    {
        HitboxModule module;
        module.onInit();
        module.setMasterEnabled(true);
        check(g_localPlayerPtr == nullptr, "no player is tracked before the first tick");

        // Tick the local player so the module picks it up.
        {
            bedrocktools::events::LocalPlayerTickEvent tickEvent{
                reinterpret_cast<bedrocktools::sdk::Player*>(localPlayer.ptr())};
            bedrocktools::events::bus().publish(tickEvent);
        }
        check(g_localPlayerPtr == localPlayer.ptr(), "the tick callback records the local player");

        g_fetchedActors[0] = mob.ptr();
        g_fetchedActorCount = 1;
        g_reportPlayer = false;
        g_reportInvisible = false;
        g_solidBlocks.clear();

        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.size() == 1, "one nearby mob produces exactly one box batch");
        check(!g_batches.empty() && g_batches[0].emittedVertices == 24,
              "a box emits its 12 edges as 24 line vertices");
        check(near(colorHolder[0], 0.2f) && near(colorHolder[3], 0.5f),
              "renderer restores the ScreenContext color state");

        // Eye and look lines are extra passes on top of the box.
        module.showEyeLine = true;
        module.showLookLine = true;
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.size() == 3, "eye line and look line add their own batches");

        module.showEyeLine = false;
        module.showLookLine = false;

        // showEntities off hides the mob.
        module.showEntities = false;
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.empty(), "disabling showEntities hides nearby mobs");
        module.showEntities = true;

        // Invisible actors are skipped.
        g_reportInvisible = true;
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.empty(), "invisible actors are skipped");
        g_reportInvisible = false;

        // The local player's own box is hidden in first person.
        g_fetchedActorCount = 0;
        s_perspective = 0;
        s_perspectiveKnown = true;
        module.show3rdPerson = true;
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.empty(), "first person never draws the local player's own box");

        // Third person with a pulled-back camera draws it.
        const Vec3 thirdPersonCamera{10.3f, 67.5f, 14.0f};
        writeAt(playerRenderer, LevelRendererPlayer::mCamPos, thirdPersonCamera);
        s_perspective = 1;
        s_perspectiveKnown = true;
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.size() == 1, "third person draws the local player's own box");
        module.show3rdPerson = false;
        writeAt(playerRenderer, LevelRendererPlayer::mCamPos, firstPersonCamera);

        // Wall occlusion: a solid block between the camera and the mob culls it.
        g_fetchedActors[0] = mob.ptr();
        g_fetchedActorCount = 1;
        g_solidBlocks.push_back({12, 65, 10});
        g_solidBlockQueries = 0;
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_solidBlockQueries > 0, "occlusion ray cast consults the block source");
        check(g_batches.empty(), "a mob fully hidden behind a solid block is culled");

        // Removing the wall brings it back.
        g_solidBlocks.clear();
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.size() == 1, "the mob is drawn again once the wall is gone");

        // Thick lines add a camera-facing quad pass on top of the hairline.
        module.lineThickness = 6.0f;
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.size() == 2, "a raised line thickness adds the quad pass");
        check(g_batches.size() == 2 && g_batches[1].emittedVertices == 24,
              "the crisp hairline pass still runs at every thickness");
        module.lineThickness = 1.0f;

        // A disabled module draws nothing even with actors in range.
        module.setMasterEnabled(false);
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.empty(), "disabling the module stops the overlay");
        check(g_localPlayerPtr == nullptr, "disabling the module drops the cached player pointer");
    }

    std::printf("hitbox config\n");
    {
        HitboxModule source;
        source.showEntities = false;
        source.showPlayers = false;
        source.showItems = false;
        source.showItemsColor = 0xFF00FF00u;
        source.show3rdPerson = true;
        source.showEyeLine = true;
        source.showLookLine = true;
        source.lookLineLength = 3.5f;
        source.lineThickness = 6.0f;
        source.hitboxIndicator = true;
        source.hitboxColor = 0xFF123456u;
        source.eyeLineColor = 0xFF223344u;
        source.lookLineColor = 0xFF556677u;
        source.indicatorDefaultColor = 0xFF8899AAu;
        source.indicatorActiveColor = 0xFFBBCCDDu;

        nlohmann::json saved;
        source.saveConfig(saved);
        check(saved.contains("showItemsColor"),
              "the item hitbox color is part of the saved config");
        check(saved["hitboxColor"].get<std::string>() == "#123456" &&
              saved["showItemsColor"].get<std::string>() == "#00FF00",
              "colors are saved as the #RRGGBB the launcher color picker reads");

        HitboxModule loaded;
        loaded.loadConfig(saved);
        check(!loaded.showEntities && !loaded.showPlayers && !loaded.showItems && loaded.show3rdPerson,
              "entity filters round-trip");
        check((loaded.showItemsColor & 0xFFFFFFu) == 0x00FF00u,
              "the item hitbox color round-trips");
        check(loaded.showEyeLine && loaded.showLookLine && near(loaded.lookLineLength, 3.5f),
              "eye/look line settings round-trip");
        check(near(loaded.lineThickness, 6.0f), "line thickness round-trips");
        check((loaded.hitboxColor & 0xFFFFFFu) == 0x123456u &&
              (loaded.eyeLineColor & 0xFFFFFFu) == 0x223344u &&
              (loaded.lookLineColor & 0xFFFFFFu) == 0x556677u &&
              (loaded.indicatorDefaultColor & 0xFFFFFFu) == 0x8899AAu &&
              (loaded.indicatorActiveColor & 0xFFFFFFu) == 0xBBCCDDu,
              "all five colors round-trip");
        check(loaded.hitboxIndicator, "the indicator toggle round-trips");
    }

    {
        // An older config stored "showSelf" instead of "show3rdPerson".
        nlohmann::json legacy;
        legacy["showSelf"] = true;
        legacy["hitboxColor"] = "#0A1B2C";
        HitboxModule migrated;
        migrated.loadConfig(legacy);
        check(migrated.show3rdPerson, "the legacy showSelf key migrates to show3rdPerson");
        check((migrated.hitboxColor & 0xFFFFFFu) == 0x0A1B2Cu, "a 6-digit color migrates");
    }

    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d hitbox integration check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all hitbox integration checks passed\n");
    return 0;
}

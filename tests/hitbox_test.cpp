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
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <pl/ModMenu.hpp>

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
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

std::vector<Batch> g_batches;
Batch g_currentBatch;

// Stand-in world: a map of "solid" block coordinates used by the occlusion
// ray cast.
std::vector<std::array<int, 3>> g_solidBlocks;
int g_solidBlockQueries = 0;

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

void* g_fetchedActors[512] = {nullptr};
int g_fetchedActorCount = 0;
// Laid out exactly like the game's DistanceSortedActor: the production code
// walks this with its own 12-byte stride, so a plain pointer array here would
// be read out of bounds.
std::vector<FakeSortedActor> g_fetchedStorage;
bool g_reportPlayer = false;
bool g_reportInvisible = false;
int g_perspective = 0;

void fakeRenderLevel(void*, void*, void*) {}

void fakeTessBegin(void*, void*, int mode, int vertexCount, int) {
    g_currentBatch = {mode, vertexCount, 0};
}
void fakeTessColor(void*, float r, float g, float b, float a) {
    g_currentBatch.r = r;
    g_currentBatch.g = g;
    g_currentBatch.b = b;
    g_currentBatch.a = a;
}
void fakeTessVertex(void*, float, float, float) { ++g_currentBatch.emittedVertices; }
void* g_lastMaterial = nullptr;
void fakeRenderMesh(void*, void*, void* material, char*) {
    g_lastMaterial = material;
    g_batches.push_back(g_currentBatch);
}

bool fakeIsPlayer(void*) { return g_reportPlayer; }
bool fakeIsInvisible(void*) { return g_reportInvisible; }

FakeActorVec fakeFetchNearby(void*, void*, int) {
    g_fetchedStorage.clear();
    for (int i = 0; i < g_fetchedActorCount; ++i) {
        g_fetchedStorage.push_back(FakeSortedActor{g_fetchedActors[i], 0.0f, 0.0f});
    }
    FakeActorVec out{};
    if (g_fetchedStorage.empty()) return out;
    // The production code only reads the actor pointer out of each element.
    out.begin = g_fetchedStorage.data();
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

// ActorManager::getRuntimeActorList(): every actor in the level.
std::vector<void*> g_managerActors;
int g_managerCalls = 0;
std::vector<void*> fakeRuntimeActorList(void*) {
    ++g_managerCalls;
    return g_managerActors;
}

// Level::getHitResult() + HitResult::getEntity(): what the crosshair is on.
std::array<std::byte, 64> g_hitResult{};
void* g_hitResultEntity = nullptr;
void* fakeLevelGetHitResult(void*) { return g_hitResult.data(); }
void* fakeHitResultGetEntity(void*) { return g_hitResultEntity; }

std::uintptr_t fakeResolve(bedrocktools::memory::SignatureId id) {
    using bedrocktools::memory::SignatureId;
    switch (id) {
        case SignatureId::ActorManagerList: return reinterpret_cast<std::uintptr_t>(&fakeRuntimeActorList);
        case SignatureId::LevelGetHitResult: return reinterpret_cast<std::uintptr_t>(&fakeLevelGetHitResult);
        case SignatureId::HitResultGetEntity: return reinterpret_cast<std::uintptr_t>(&fakeHitResultGetEntity);
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

// The client-instance route the fallback uses when no tick ever delivered a
// player pointer: GameHooks' ClientInstanceUpdate hook in production, a plain
// stand-in here.
namespace bedrocktools::core::gamehooks {
void* g_client = nullptr;
void* clientInstance() { return g_client; }
} // namespace bedrocktools::core::gamehooks

namespace {
void* g_fakeClientPlayer = nullptr;
void* fakeGetLocalPlayer(void*) { return g_fakeClientPlayer; }

// A stand-in ClientInstance: its first word is the vtable the production code
// indexes into for getLocalPlayer().
void** fakeClientObject() {
    static void** vtable = nullptr;
    static void* object[4] = {nullptr};
    if (!vtable) {
        vtable = new void*[64]();
        vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer] =
            reinterpret_cast<void*>(&fakeGetLocalPlayer);
        object[0] = vtable;
    }
    return reinterpret_cast<void**>(object);
}
} // namespace

// The launcher HUD layer. The fallback submits its boxes there instead of
// tessellating them into the world pass, so the test installs the shared
// fake's draw sink and reads the commands back out of it.
namespace {
std::vector<pl::modmenu::DrawCommand> g_submitted;
std::vector<pl::modmenu::DrawCommand>& hudSink() {
    pl::modmenu::drawCommandSink() = &g_submitted;
    return g_submitted;
}
} // namespace

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

    void attachLevel(void* level) {
        writeAt(actor, bedrocktools::sdk::offsets::Actor::mLevel,
                reinterpret_cast<std::uintptr_t>(level));
    }

    void* ptr() { return actor.data(); }
};

} // namespace

int main() {
    using namespace bedrocktools::sdk::offsets;

    // Everything the module submits to the HUD layer lands in g_submitted.
    hudSink();


    std::printf("hitbox render integration\n");

    // Captured up front: constructing a HitboxModule installs it as the global
    // g_hitboxMod, so this must not be a temporary created mid-test.
    const bool hideBehindWallsDefault = HitboxModule{}.hideBehindWalls;

    alignas(std::max_align_t) std::array<std::byte, 256> screenContext{};
    alignas(std::max_align_t) std::array<std::byte, 0x500> levelRenderer{};
    alignas(std::max_align_t) std::array<std::byte, 0x1100> playerRenderer{};
    alignas(std::max_align_t) std::array<float, 4> colorHolder{0.2f, 0.3f, 0.4f, 0.5f};
    std::uint64_t tessellator = 0;

    writeAt(screenContext, ScreenContext::mTessellator, static_cast<void*>(&tessellator));
    writeAt(screenContext, ScreenContext::mColorHolder, static_cast<void*>(colorHolder.data()));
    writeAt(levelRenderer, LevelRenderer::mLevelRendererPlayer, static_cast<void*>(playerRenderer.data()));

    // The embedded selection-overlay material the module falls back to when
    // the material group is unavailable. It is a MaterialPtr: data + control
    // block whose first word is a code pointer. A build gets an unusable slot
    // when its offset is wrong (the module probes every known offset), so the
    // stand-in has to look real for the fallback to be accepted.
    alignas(std::max_align_t) std::array<std::byte, 32> materialData{};
    alignas(std::max_align_t) std::array<std::byte, 32> materialControl{};
    writeAt(materialControl, 0, static_cast<void*>(materialData.data()));
    auto installEmbeddedMaterial = [&](std::size_t offset) {
        writeAt(playerRenderer, offset, static_cast<void*>(materialData.data()));
        writeAt(playerRenderer, offset + sizeof(void*), static_cast<void*>(materialControl.data()));
    };
    installEmbeddedMaterial(LevelRendererPlayer::mSelectionOverlayMaterial);

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

    // The level the local player belongs to, with its actor manager: the
    // second (and primary) actor source the module now uses.
    alignas(std::max_align_t) std::array<std::byte, 0x500> level{};
    alignas(std::max_align_t) std::array<std::byte, 64> actorManager{};
    writeAt(level, Level::mActorManager, static_cast<void*>(actorManager.data()));
    localPlayer.attachLevel(level.data());

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

        // Invisible actors are boxed by default (see the dedicated checks
        // below) and skipped once Hide Invisible is on.
        g_reportInvisible = true;
        module.hideInvisible = true;
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.empty(), "invisible actors are skipped while hide invisible is on");
        module.hideInvisible = false;
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

        // Wall culling is opt-in now. It used to be hardcoded on, which meant a
        // build where the isSolidBlockingBlock resolution is wrong silently
        // suppressed every hitbox with no way to recover.
        check(!hideBehindWallsDefault, "hide behind walls defaults to off");
        g_fetchedActors[0] = mob.ptr();
        g_fetchedActorCount = 1;
        g_solidBlocks.push_back({12, 65, 10});
        g_solidBlockQueries = 0;
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_solidBlockQueries == 0, "the block source is not queried while culling is off");
        check(g_batches.size() == 1, "a mob behind a wall is still drawn by default");

        // With the setting on, a solid block between the camera and the mob
        // culls it.
        module.hideBehindWalls = true;
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
        module.hideBehindWalls = false;

        // Thick lines add a camera-facing quad pass on top of the hairline.
        module.lineThickness = 6.0f;
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.size() == 2, "a raised line thickness adds the quad pass");
        check(g_batches.size() == 2 && g_batches[1].emittedVertices == 24,
              "the crisp hairline pass still runs at every thickness");
        module.lineThickness = 1.0f;

        // The actor manager is a second, independent source: with the
        // nearby-actor scan returning nothing at all, boxes still appear. This
        // is the regression the module kept being reported with - one broken
        // enumeration signature left the whole overlay empty.
        g_fetchedActorCount = 0;
        g_managerActors = {mob.ptr()};
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.size() == 1,
              "the level's actor manager alone is enough to draw a box");

        // ... and the two sources merge instead of drawing the same actor twice.
        g_fetchedActors[0] = mob.ptr();
        g_fetchedActorCount = 1;
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.size() == 1, "an actor found by both sources is boxed once");
        g_managerActors.clear();
        g_fetchedActorCount = 0;

        // The entity under the crosshair is boxed even when no list has it,
        // and it is the one the indicator highlights.
        module.hitboxIndicator = true;
        g_hitResultEntity = mob.ptr();
        writeAt(g_hitResult, HitResult::mType, HitResult::TypeEntity);
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.size() == 1, "the entity from the game's hit result is boxed");
        check(g_batches.size() == 1 && near(g_batches[0].r, 1.0f) && near(g_batches[0].g, 0.0f),
              "the aimed entity uses the active indicator color");
        module.hitboxIndicator = false;
        g_hitResultEntity = nullptr;
        writeAt(g_hitResult, HitResult::mType, HitResult::TypeNoHit);

        // A color with no RGB (what a launcher color picker leaves behind when
        // it cannot parse its value) would draw an invisible box, so it falls
        // back to white instead of leaving the module looking dead.
        module.hitboxColor = 0x00000000u;
        module.showItemsColor = 0x00000000u;
        module.showEntities = true;
        g_fetchedActors[0] = mob.ptr();
        g_fetchedActorCount = 1;
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.size() == 1 && near(g_batches[0].r, 1.0f) &&
              near(g_batches[0].g, 1.0f) && near(g_batches[0].b, 1.0f),
              "a black box color falls back to white instead of drawing nothing");
        module.hitboxColor = 0xFFFFFFFFu;
        module.showItemsColor = 0xFFFFFFFFu;

        // Invisible actors are boxed by default (this is a hitbox overlay), and
        // skipped once the option is turned on.
        g_reportInvisible = true;
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.size() == 1, "invisible actors keep their box by default");
        module.hideInvisible = true;
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.empty(), "hide invisible turns the box off");
        module.hideInvisible = false;
        g_reportInvisible = false;

        // A box that is not finite cannot be tessellated: it would poison the
        // whole mesh, so it is dropped before any vertex is emitted.
        FakeActor nanActor({0.0f / 0.0f, 64.0f, 10.0f}, {1.0f, 65.0f, 11.0f},
                           ActorCategories::IsMob);
        g_fetchedActors[0] = nanActor.ptr();
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.empty(), "an unusable (NaN) box is never tessellated");
        g_fetchedActors[0] = mob.ptr();

        // The embedded selection material is used when the material group
        // could not be resolved (the stand-in build has no material group), and
        // its address is the probed slot rather than a pointer into unrelated
        // memory. Both known offsets are checked because the offset differs
        // between game builds.
        g_fetchedActors[0] = mob.ptr();
        g_fetchedActorCount = 1;
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.size() == 1 && g_lastMaterial ==
                  reinterpret_cast<void*>(playerRenderer.data() + LevelRendererPlayer::mSelectionOverlayMaterial),
              "the embedded material slot is used when no material group resolved");

        // The upstream offset is probed too, so a build whose header disagrees
        // still draws instead of handing the renderer a foreign pointer.
        writeAt(playerRenderer, LevelRendererPlayer::mSelectionOverlayMaterial, static_cast<void*>(nullptr));
        writeAt(playerRenderer, LevelRendererPlayer::mSelectionOverlayMaterial + sizeof(void*),
                static_cast<void*>(nullptr));
        installEmbeddedMaterial(0x1030);
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.size() == 1 && g_lastMaterial ==
                  reinterpret_cast<void*>(playerRenderer.data() + 0x1030),
              "the upstream material offset is used when the header offset is empty");

        // With no usable material anywhere the module stops instead of
        // tessellating with a pointer that is not a material at all.
        writeAt(playerRenderer, 0x1030, static_cast<void*>(nullptr));
        writeAt(playerRenderer, 0x1030 + sizeof(void*), static_cast<void*>(nullptr));
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.empty(), "an unusable material stops the draw instead of rendering garbage");
        installEmbeddedMaterial(LevelRendererPlayer::mSelectionOverlayMaterial);

        // A camera that does not sit near the player means the player-renderer
        // layout is not the one this build was written for. Nothing is drawn in
        // world space then (the boxes would land in empty space), which is what
        // lets the HUD fallback take over.
        const Vec3 savedCamera = thirdPersonCamera;
        s_lastWorldDrawUs = 0;
        writeAt(playerRenderer, LevelRendererPlayer::mCamPos, Vec3{5000.0f, 5000.0f, 5000.0f});
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.empty(), "a camera that is not near the player stops the world-space draw");
        check(s_lastWorldDrawUs == 0, "an unusable camera does not count as a live world pass");
        writeAt(playerRenderer, LevelRendererPlayer::mCamPos, savedCamera);

        // With the world pass silent the boxes are projected onto the HUD layer
        // instead, which is how the overlay stays visible on a build where the
        // level-renderer signatures or offsets do not match. The fallback reads
        // what the client tick cached, so tick first.
        module.hudFallback = true;
        s_lastWorldDrawUs = 0;
        auto tick = [&] {
            bedrocktools::events::LocalPlayerTickEvent tickEvent{
                reinterpret_cast<bedrocktools::sdk::Player*>(localPlayer.ptr())};
            bedrocktools::events::bus().publish(tickEvent);
        };
        g_fetchedActors[0] = mob.ptr();
        g_fetchedActorCount = 1;
        // Yaw -90 looks towards +X, which is where the stand-in mob stands, so
        // the box is inside the fallback's view frustum.
        writeAt(localPlayer.rotation, 0, Vec2{0.0f, -90.0f});
        tick();
        g_submitted.clear();
        module.onFrame();
        check(!g_submitted.empty(), "the HUD fallback submits box edges");
        check(g_submitted.size() == 12 &&
                  g_submitted[0].type == pl::modmenu::DrawCommandType::Line,
              "one visible actor is twelve projected line edges");
        check(g_submitted.empty() ||
                  (g_submitted[0].w == g_submitted[0].w &&
                   std::isfinite(g_submitted[0].x) &&
                   std::isfinite(g_submitted[0].w)),
              "projected edges are finite surface coordinates");

        // While the world pass is alive the HUD layer is cleared again, so the
        // overlay is never drawn twice.
        s_lastWorldDrawUs = nowUs();
        module.onFrame();
        check(g_submitted.empty(), "a live world pass keeps the HUD layer clear");
        s_lastWorldDrawUs = 0;

        // Forcing it on draws the HUD boxes even while the world pass says it
        // is alive - the escape hatch for a pass that runs but stays invisible.
        module.hudFallbackAlways = true;
        s_lastWorldDrawUs = nowUs();
        g_submitted.clear();
        module.onFrame();
        check(!g_submitted.empty(),
              "the always-on fallback draws while the world pass reports live");
        module.hudFallbackAlways = false;
        s_lastWorldDrawUs = nowUs();
        g_submitted.clear();
        module.onFrame();
        check(g_submitted.empty(), "the always-on fallback is off by default");
        s_lastWorldDrawUs = 0;
        g_submitted.clear();
        module.onFrame();
        check(!g_submitted.empty(), "the automatic fallback takes over again");

        // Turning the fallback off draws nothing at all on such a build.
        module.hudFallback = false;
        module.onFrame();
        check(g_submitted.empty(), "hide the fallback and nothing is submitted");
        module.hudFallback = true;

        // The fallback honors the same color rule as the world pass.
        module.hitboxColor = 0x00000000u;
        tick();
        module.onFrame();
        check(!g_submitted.empty() &&
                  (g_submitted[0].color & 0x00FFFFFFu) == 0x00FFFFFFu,
              "the fallback also falls back to white for a black box color");
        module.hitboxColor = 0xFFFFFFFFu;

        // Disabling the module clears the HUD layer.
        g_submitted.clear();
        module.setMasterEnabled(true);
        tick();
        module.onFrame();
        check(!g_submitted.empty(), "the fallback is drawing before the module is disabled");

        // The on-screen status readout is off by default (it is a debug line),
        // and when it is on it rides along in the same submission.
        auto countText = [] {
            size_t n = 0;
            for (const auto& command : g_submitted) {
                if (command.type == pl::modmenu::DrawCommandType::Text) ++n;
            }
            return n;
        };
        check(countText() == 0, "the status readout is off by default");

        module.hudDiagnostics = true;
        module.onFrame();
        check(countText() == 2, "the status readout adds its two lines");
        bool describes = false;
        for (const auto& command : g_submitted) {
            if (command.type != pl::modmenu::DrawCommandType::Text) continue;
            if (command.text.find("fallback") != std::string::npos &&
                command.text.find("box") != std::string::npos) {
                describes = true;
            }
        }
        check(describes, "the readout names the pass that is drawing");
        module.hudDiagnostics = false;

        // onFrame works off its own copy of what the tick cached: a frame that
        // runs without a new tick still draws the same boxes (and the tick
        // thread can never be walking the list the render thread is reading).
        module.onFrame(); // back to box edges only
        const size_t frameOne = g_submitted.size();
        module.onFrame(); // no tick in between
        check(!g_submitted.empty() && g_submitted.size() == frameOne,
              "a frame without a new tick redraws the same boxes");

        // The tick hook is not the only way to the player pointer: when it never
        // delivered one, the fallback asks the client instance instead (a
        // vtable slot, the version-independent route the HUD modules use).
        module.onDisable(); // drop the tick cache
        g_localPlayerPtr = nullptr;
        g_fetchedActors[0] = mob.ptr();
        g_fetchedActorCount = 1;
        writeAt(localPlayer.rotation, 0, Vec2{0.0f, -90.0f});
        g_submitted.clear();
        module.onFrame();
        check(g_submitted.empty(),
              "with no player from the tick the fallback draws nothing (the client is silent)");

        bedrocktools::core::gamehooks::g_client = fakeClientObject();
        g_fakeClientPlayer = localPlayer.ptr();
        g_submitted.clear();
        // The client is only asked every so often (asking means walking the
        // level), so a few frames may pass before it is picked up.
        for (int frame = 0; frame < 32 && g_submitted.empty(); ++frame) module.onFrame();
        check(!g_submitted.empty(),
              "the fallback reaches the player through the client instance when the tick does not");
        check(g_submitted.size() >= 12, "the client-instance route boxes the same actors");
        bool allLines = true;
        for (const auto& command : g_submitted) {
            if (command.type != pl::modmenu::DrawCommandType::Line) allLines = false;
        }
        check(allLines, "the client-instance route submits box edges");
        bedrocktools::core::gamehooks::g_client = nullptr;
        g_fakeClientPlayer = nullptr;

        // A crowded world: the launcher rejects a draw batch that is too long,
        // so the fallback has to stay under its own budget instead of losing
        // every box at once.
        {
            std::vector<std::unique_ptr<FakeActor>> crowd;
            g_fetchedActorCount = 0;
            // 300 mobs packed inside the module's 30-block radius, so the edge
            // count (12 each) runs past the batch budget.
            for (int i = 0; i < 300; ++i) {
                const float x = 12.0f + static_cast<float>(i % 20) * 0.5f;
                const float z = 8.0f + static_cast<float>(i / 20) * 0.5f;
                crowd.push_back(std::make_unique<FakeActor>(Vec3{x, 64.0f, z},
                                                            Vec3{x + 0.6f, 65.8f, z + 0.6f},
                                                            ActorCategories::IsMob));
                g_fetchedActors[g_fetchedActorCount++] = crowd.back()->ptr();
            }
            tick();
            g_submitted.clear();
            module.onFrame();
            check(!g_submitted.empty(), "a crowded world still draws");
            check(g_submitted.size() <= 3072,
                  "the fallback stays under the launcher's batch budget");
            bool allFinite = true;
            for (const auto& command : g_submitted) {
                allFinite = allFinite && std::isfinite(command.x) && std::isfinite(command.y) &&
                            std::isfinite(command.w) && std::isfinite(command.h) &&
                            std::isfinite(command.size);
            }
            check(allFinite, "every submitted command is finite (one bad one drops the batch)");

            // The nearest actors are the ones that survive the cap.
            tick();
            module.hudDiagnostics = true;
            module.onFrame();
            bool cappedLine = false;
            for (const auto& command : g_submitted) {
                if (command.type == pl::modmenu::DrawCommandType::Text &&
                    command.text.find("capped") != std::string::npos) {
                    cappedLine = true;
                }
            }
            check(g_fetchedActorCount == 300 && g_submitted.size() <= 3076,
                  "the readout reports the capped frame");
            check(cappedLine, "the readout says the frame was capped");
            module.hudDiagnostics = false;

            g_fetchedActorCount = 1;
            g_fetchedActors[0] = mob.ptr();
            tick();
            module.onFrame();
        }
        writeAt(localPlayer.rotation, 0, Vec2{0.0f, 0.0f});

        // A disabled module draws nothing even with actors in range.
        module.setMasterEnabled(false);
        g_batches.clear();
        _renderLevel_hook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_batches.empty(), "disabling the module stops the overlay");
        check(g_localPlayerPtr == nullptr, "disabling the module drops the cached player pointer");
        check(g_submitted.empty(), "disabling the module clears the HUD layer");
    }

    std::printf("hitbox config\n");
    {
        HitboxModule source;
        source.showEntities = false;
        source.showPlayers = false;
        source.showItems = false;
        source.showItemsColor = 0xFF00FF00u;
        source.hideBehindWalls = false;
        source.show3rdPerson = true;
        source.showEyeLine = true;
        source.showLookLine = true;
        source.lookLineLength = 3.5f;
        source.lineThickness = 6.0f;
        source.hitboxIndicator = true;
        source.hudFallback = false;
        source.hudFallbackAlways = true;
        source.hudDiagnostics = true;
        source.hudFov = 95.0f;
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
        check(!loaded.hideBehindWalls, "hide behind walls stays off across a config round-trip");
        check(!loaded.hideInvisible, "invisible actors stay boxed across a config round-trip");
        check(!loaded.hudFallback, "the screen-space fallback round-trips");
        check(loaded.hudDiagnostics, "the status readout setting round-trips");
        check(near(loaded.hudFov, 95.0f), "the fallback field of view round-trips");
        check(loaded.hudFallbackAlways, "the always-on fallback round-trips");
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

// Integration-style host test for the Storage ESP module. It drives the real
// tick scan and the real render pass against small in-memory stand-ins for the
// dimension, the block store and the tessellator, then checks the config keys.
//
// Build: g++ -std=c++20 -I include -I src -I tests/fakepl -I tests/fakejson
//        tests/storageesp_test.cpp -o /tmp/storageesp_test
// Run:   /tmp/storageesp_test

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "bedrocktools/events/EventBus.hpp"
#include "bedrocktools/memory/Signatures.hpp"
#include "bedrocktools/sdk/Offsets.hpp"
#include "bedrocktools/sdk/Types.hpp"

namespace {

using bedrocktools::sdk::BlockPos;
using bedrocktools::sdk::Vec2;
using bedrocktools::sdk::Vec3;

struct Batch {
    int mode = -1;
    int reservedVertices = 0;
    int emittedVertices = 0;
    std::vector<Vec3> vertices; // camera-relative, in emission order
};

std::vector<Batch> g_batches;
Batch g_currentBatch;

void fakeRenderLevel(void*, void*, void*) {}
void fakeTessBegin(void*, void*, int mode, int vertexCount, int) {
    g_currentBatch = Batch{};
    g_currentBatch.mode = mode;
    g_currentBatch.reservedVertices = vertexCount;
}
void fakeTessColor(void*, float, float, float, float) {}
void fakeTessVertex(void*, float x, float y, float z) {
    ++g_currentBatch.emittedVertices;
    g_currentBatch.vertices.push_back({x, y, z});
}
void fakeRenderMesh(void*, void*, void*, char*) { g_batches.push_back(g_currentBatch); }

template <typename T, std::size_t N>
void writeAt(std::array<std::byte, N>& storage, std::size_t offset, const T& value) {
    std::memcpy(storage.data() + offset, &value, sizeof(T));
}

// One fake block type: the game's BlockLegacy, whose full name lives at
// BlockType::mNameInfo + NameInfo::mFullName + HashedString::mString, plus the
// Block object that points at it. All block instances of one type share the
// same pointers, exactly like the game's per-type singletons.
struct FakeBlockType {
    static constexpr std::size_t kNameOffset = bedrocktools::sdk::offsets::BlockType::mNameInfo +
                                               bedrocktools::sdk::offsets::NameInfo::mFullName +
                                               bedrocktools::sdk::offsets::HashedString::mString;

    alignas(std::max_align_t) std::byte legacy[0x120]{};
    alignas(std::max_align_t) std::byte block[0x80]{};
    std::string name; // owns the buffer the copy inside `legacy` points at

    explicit FakeBlockType(std::string text) : name(std::move(text)) {
        std::memcpy(legacy + kNameOffset, &name, sizeof(std::string));
        void* legacyPointer = legacy;
        std::memcpy(block + bedrocktools::sdk::offsets::Block::mBlockType, &legacyPointer,
                    sizeof(void*));
    }
};

std::map<std::string, std::unique_ptr<FakeBlockType>> g_types;
std::map<std::tuple<int, int, int>, FakeBlockType*> g_world;

FakeBlockType* typeFor(const std::string& name) {
    const auto existing = g_types.find(name);
    if (existing != g_types.end()) return existing->second.get();
    auto created = std::make_unique<FakeBlockType>(name);
    FakeBlockType* type = created.get();
    g_types.emplace(name, std::move(created));
    return type;
}

void setBlock(const BlockPos& position, const char* name) {
    g_world[{position.x, position.y, position.z}] = name ? typeFor(name) : nullptr;
}

const void* fakeGetBlock(void*, const BlockPos* position, int) {
    static FakeBlockType* air = typeFor("minecraft:air");
    const auto found = g_world.find({position->x, position->y, position->z});
    // Unknown positions behave like the game's shared air block.
    return (found == g_world.end() || !found->second) ? static_cast<const void*>(air->block)
                                                     : static_cast<const void*>(found->second->block);
}

void clearWorld() { g_world.clear(); }
} // namespace

namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId id) {
    switch (id) {
        case SignatureId::RenderLevel:
            return reinterpret_cast<std::uintptr_t>(&fakeRenderLevel);
        case SignatureId::TessellatorBegin:
            return reinterpret_cast<std::uintptr_t>(&fakeTessBegin);
        case SignatureId::TessellatorColor:
            return reinterpret_cast<std::uintptr_t>(&fakeTessColor);
        case SignatureId::TessellatorVertex:
            return reinterpret_cast<std::uintptr_t>(&fakeTessVertex);
        case SignatureId::MeshHelpersRenderMeshImmediately2:
            return reinterpret_cast<std::uintptr_t>(&fakeRenderMesh);
        case SignatureId::BlockSourceGetBlock:
            return reinterpret_cast<std::uintptr_t>(&fakeGetBlock);
        default:
            return 0;
    }
}
} // namespace bedrocktools::memory

namespace bedrocktools::events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace bedrocktools::events

// Include the production implementation so this test can drive the module's
// internal scan and render helpers without adding a test-only API to it.
#include "modules/visual/storageesp.cpp"

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

using Kind = storageesp::StorageKind;

struct FakeWorld {
    alignas(std::max_align_t) std::array<std::byte, 0x400> player{};
    alignas(std::max_align_t) std::array<std::byte, 0x100> dimension{};
    alignas(std::max_align_t) std::array<std::byte, 16> region{};
    alignas(std::max_align_t) std::array<std::byte, 32> position{};
    alignas(std::max_align_t) std::array<std::byte, 16> rotationComponent{};
    alignas(std::max_align_t) std::array<std::byte, 256> screenContext{};
    alignas(std::max_align_t) std::array<std::byte, 0x500> levelRenderer{};
    alignas(std::max_align_t) std::array<std::byte, 0x1100> playerRenderer{};
    std::array<float, 4> colorHolder{0.2f, 0.3f, 0.4f, 0.5f};
    std::uint64_t tessellator = 0;

    void place(const Vec3& at) {
        std::memcpy(position.data(), &at, sizeof(Vec3));
    }

    // The tick publishes the player's rotation with its position, because the
    // render pass has a camera position but no view direction of its own.
    void setRotation(const Vec2& rotation) {
        std::memcpy(rotationComponent.data(), &rotation, sizeof(Vec2));
    }

    void bind() {
        void* regionPointer = region.data();
        void* positionPointer = position.data();
        void* rotationPointer = rotationComponent.data();
        void* dimensionPointer = dimension.data();
        void* playerRendererPointer = playerRenderer.data();
        void* colorPointer = colorHolder.data();
        void* tessellatorPointer = &tessellator;

        writeAt(dimension, bedrocktools::sdk::offsets::Dimension::mBlockSource, regionPointer);
        writeAt(player, bedrocktools::sdk::offsets::Actor::mDimension, dimensionPointer);
        writeAt(player, bedrocktools::sdk::offsets::Actor::mStateVectorComponent, positionPointer);
        writeAt(player, bedrocktools::sdk::offsets::Actor::mActorRotationComponent, rotationPointer);
        writeAt(screenContext, bedrocktools::sdk::offsets::ScreenContext::mTessellator,
                tessellatorPointer);
        writeAt(screenContext, bedrocktools::sdk::offsets::ScreenContext::mColorHolder, colorPointer);
        writeAt(levelRenderer, bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer,
                playerRendererPointer);
    }

    void setCamera(const Vec3& camera) {
        std::memcpy(playerRenderer.data() + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos,
                    &camera, sizeof(Vec3));
    }
};

void tick(FakeWorld& world) { scanStep(reinterpret_cast<bedrocktools::sdk::Player*>(world.player.data())); }

int outlineBatches() {
    int count = 0;
    for (const auto& batch : g_batches) {
        if (batch.mode == 4) ++count;
    }
    return count;
}

int fillBatches() {
    int count = 0;
    for (const auto& batch : g_batches) {
        if (batch.mode == 1) ++count;
    }
    return count;
}

int lineVertices() {
    int count = 0;
    for (const auto& batch : g_batches) {
        if (batch.mode == 4) count += batch.emittedVertices;
    }
    return count;
}

float length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// True when at least one native-line batch was submitted and every segment in
// those batches starts `depth` blocks in front of the camera along `forward`.
// Submitted vertices are camera-relative, so the dot product is the view depth.
// A tracer start at depth zero is the bug this guards: it projects through a
// divide by zero and mobile drivers drop the primitive instead of clipping it.
bool lineBatchesStartAtDepth(float depth, const Vec3& forward) {
    bool sawBatch = false;
    for (const auto& batch : g_batches) {
        if (batch.mode != 4 || batch.vertices.size() < 2) continue;
        sawBatch = true;
        for (std::size_t i = 0; i + 1 < batch.vertices.size(); i += 2) {
            const Vec3& from = batch.vertices[i];
            const float fromDepth = from.x * forward.x + from.y * forward.y + from.z * forward.z;
            if (!near(fromDepth, depth, 0.001f)) return false;
        }
    }
    return sawBatch;
}

// True when every submitted line segment lies on one ray out of the camera, so
// its two camera-relative vertices are parallel. That is what a camera-anchored
// tracer has to look like: all of them converge on the crosshair.
bool lineBatchesRunThroughTheCamera() {
    bool sawBatch = false;
    for (const auto& batch : g_batches) {
        if (batch.mode != 4 || batch.vertices.size() < 2) continue;
        sawBatch = true;
        for (std::size_t i = 0; i + 1 < batch.vertices.size(); i += 2) {
            const Vec3& from = batch.vertices[i];
            const Vec3& to = batch.vertices[i + 1];
            const Vec3 cross{from.y * to.z - from.z * to.y,
                             from.z * to.x - from.x * to.z,
                             from.x * to.y - from.y * to.x};
            if (length(cross) > 0.001f * std::max(1.0f, length(to))) return false;
        }
    }
    return sawBatch;
}

// True when every submitted line segment spans a real screen angle: its two
// camera-relative ends point in different directions, so the segment is a line
// on screen instead of a single pixel. A tracer anchored to the eye fails this
// by construction, which is the bug this guards.
bool lineBatchesSpanScreenAngle(float minimumRadians) {
    bool sawBatch = false;
    for (const auto& batch : g_batches) {
        if (batch.mode != 4 || batch.vertices.size() < 2) continue;
        sawBatch = true;
        for (std::size_t i = 0; i + 1 < batch.vertices.size(); i += 2) {
            const Vec3& from = batch.vertices[i];
            const Vec3& to = batch.vertices[i + 1];
            const float fromLength = length(from);
            const float toLength = length(to);
            if (fromLength < 0.00001f || toLength < 0.00001f) return false;
            float cosine = (from.x * to.x + from.y * to.y + from.z * to.z) /
                           (fromLength * toLength);
            cosine = std::clamp(cosine, -1.0f, 1.0f);
            if (std::acos(cosine) < minimumRadians) return false;
        }
    }
    return sawBatch;
}

// True when no submitted camera-facing strip is edge-on. A strip is seen
// edge-on when its plane holds the eye, i.e. when the plane's normal is
// perpendicular to the vector reaching one of its corners; such a strip covers
// no pixels at all, which is how a camera-anchored tracer used to disappear.
// Corners are emitted near-far-far-near, so the far end is |v2 - v1| and the
// near |v3 - v0|.
bool stripsFaceTheCamera() {
    bool sawBatch = false;
    for (const auto& batch : g_batches) {
        if (batch.mode != 1 || batch.vertices.size() < 4) continue;
        sawBatch = true;
        for (std::size_t i = 0; i + 4 <= batch.vertices.size(); i += 4) {
            const Vec3& corner = batch.vertices[i];
            const Vec3 axis{batch.vertices[i + 1].x + batch.vertices[i + 2].x -
                                batch.vertices[i].x - batch.vertices[i + 3].x,
                            batch.vertices[i + 1].y + batch.vertices[i + 2].y -
                                batch.vertices[i].y - batch.vertices[i + 3].y,
                            batch.vertices[i + 1].z + batch.vertices[i + 2].z -
                                batch.vertices[i].z - batch.vertices[i + 3].z};
            const Vec3 across{batch.vertices[i + 3].x - batch.vertices[i].x,
                              batch.vertices[i + 3].y - batch.vertices[i].y,
                              batch.vertices[i + 3].z - batch.vertices[i].z};
            const Vec3 normal{axis.y * across.z - axis.z * across.y,
                              axis.z * across.x - axis.x * across.z,
                              axis.x * across.y - axis.y * across.x};
            const float normalLength = length(normal);
            if (normalLength < 0.00001f) return false;
            const float distanceFromEye =
                (normal.x * corner.x + normal.y * corner.y + normal.z * corner.z) / normalLength;
            if (std::fabs(distanceFromEye) <= 0.001f * std::max(1.0f, length(corner))) {
                return false;
            }
        }
    }
    return sawBatch;
}

// True when every camera-facing strip is wider at its far end than at its near
// end, by more than the distance ratio would suggest is accidental. Corners are
// emitted near-far-far-near, so the far end is |v2 - v1| and the near |v3 - v0|.
bool stripsWidenWithDistance() {
    bool sawBatch = false;
    for (const auto& batch : g_batches) {
        if (batch.mode != 1 || batch.vertices.size() < 4) continue;
        sawBatch = true;
        for (std::size_t i = 0; i + 4 <= batch.vertices.size(); i += 4) {
            const Vec3 nearSide{batch.vertices[i + 3].x - batch.vertices[i].x,
                                batch.vertices[i + 3].y - batch.vertices[i].y,
                                batch.vertices[i + 3].z - batch.vertices[i].z};
            const Vec3 farSide{batch.vertices[i + 2].x - batch.vertices[i + 1].x,
                               batch.vertices[i + 2].y - batch.vertices[i + 1].y,
                               batch.vertices[i + 2].z - batch.vertices[i + 1].z};
            if (length(farSide) < length(nearSide) * 2.0f) return false;
        }
    }
    return sawBatch;
}

} // namespace

int main() {
    std::printf("storage esp world scan\n");
    FakeWorld world;
    world.bind();
    // Standing in the middle of a chunk keeps the fake chunk math predictable.
    const Vec3 playerPosition{8.5f, 64.0f, 8.5f};
    world.place(playerPosition);
    world.setCamera({8.5f, 64.5f, 8.5f});

    clearWorld();
    setBlock({10, 64, 9}, "minecraft:chest");
    setBlock({3, 66, 3}, "minecraft:ender_chest");
    setBlock({-8, 64, 8}, "minecraft:barrel");
    setBlock({64, 64, 64}, "minecraft:chest"); // far outside the scan area
    setBlock({17, 64, 8}, "minecraft:chest");  // scanned, but past the radius

    {
        StorageEspModule module;
        module.scanSpeed = 0; // Relaxed: 3000 blocks per tick, so a sweep takes a few ticks
        module.scanRadius = 16;
        module.scanHeight = 2;
        module.onInit();
        module.setMasterEnabled(true);

        tick(world);
        check(s_cache.contains({10, 64, 9}) && s_cache.contains({3, 66, 3}) && s_cache.size() == 3,
              "one tick of the limited budget covers the player's own chunk and the next one");
        check(!s_cache.contains({-8, 64, 8}), "a sweep that ran out of budget keeps going next tick");

        tick(world);
        tick(world);
        tick(world);
        check(s_cache.contains({-8, 64, 8}) && s_cache.contains({17, 64, 8}),
              "the next ticks finish the sweep and remember the rest of the area");
        check(s_cache.size() == 4, "every container inside the scan area is remembered, once per block");
        check(!s_cache.contains({64, 64, 64}), "blocks outside the scan radius are never looked at");

        const auto* chest = s_cache.find({10, 64, 9});
        const auto* ender = s_cache.find({3, 66, 3});
        const auto* barrel = s_cache.find({-8, 64, 8});
        check(chest && chest->kind == Kind::Chest, "a chest block is remembered as a chest");
        check(ender && ender->kind == Kind::EnderChest, "an ender chest is remembered as an ender chest");
        check(barrel && barrel->kind == Kind::Barrel, "a barrel is remembered as a barrel");

        std::printf("storage esp overlay\n");
        g_batches.clear();
        renderStorageEsp(world.levelRenderer.data(), world.screenContext.data());
        check(outlineBatches() == 3, "one crisp-line batch per highlight group");
        check(fillBatches() == 3, "one camera-facing quad batch per highlight group");
        bool countsMatch = true;
        for (const auto& batch : g_batches) {
            if (batch.emittedVertices != batch.reservedVertices) countsMatch = false;
        }
        check(countsMatch, "every tessellator batch emits exactly what it reserved");
        check(near(world.colorHolder[0], 0.2f) && near(world.colorHolder[1], 0.3f) &&
                  near(world.colorHolder[2], 0.4f) && near(world.colorHolder[3], 0.5f),
              "the overlay restores ScreenContext color state");

        check(lineVertices() == 4 * 24, "all four in-range containers draw twelve edges each");

        module.fill = true;
        module.fillOpacity = 0.5f;
        g_batches.clear();
        renderStorageEsp(world.levelRenderer.data(), world.screenContext.data());
        check(fillBatches() == 6, "enabling fill adds a translucent pass for every group");

        module.fill = false;
        module.showEnderChests = false;
        g_batches.clear();
        renderStorageEsp(world.levelRenderer.data(), world.screenContext.data());
        check(outlineBatches() == 2, "turning a highlight group off hides it immediately");
        module.showEnderChests = true;

        module.outline = false;
        g_batches.clear();
        renderStorageEsp(world.levelRenderer.data(), world.screenContext.data());
        check(g_batches.empty(), "with both passes off nothing is submitted");
        module.outline = true;

        module.maxBoxes = 1;
        g_batches.clear();
        renderStorageEsp(world.levelRenderer.data(), world.screenContext.data());
        int cappedBoxes = 0;
        for (const auto& batch : g_batches) {
            if (batch.mode == 4) cappedBoxes += batch.emittedVertices / 24;
        }
        check(cappedBoxes == 1, "the box cap keeps only the nearest container");
        module.maxBoxes = 64;

        std::printf("storage esp tracers\n");
        // Looking towards +X: the two chests are in front of the camera, the
        // ender chest and the barrel are behind it.
        world.setRotation({0.0f, -90.0f});
        const Vec3 forward{1.0f, 0.0f, 0.0f};
        tick(world); // the tick publishes the rotation to the render pass

        module.outline = false;
        module.tracer = true;
        g_batches.clear();
        renderStorageEsp(world.levelRenderer.data(), world.screenContext.data());
        check(outlineBatches() == 1, "the visible tracers of a group are one line batch");
        check(lineVertices() == 2 * 2,
              "containers behind the eye plane get no tracer instead of a degenerate one");
        check(fillBatches() == 1, "a tracer thicker than a hairline also builds a camera-facing strip");
        bool tracerCountsMatch = true;
        for (const auto& batch : g_batches) {
            if (batch.emittedVertices != batch.reservedVertices) tracerCountsMatch = false;
        }
        check(tracerCountsMatch, "tracer batches emit exactly what they reserved");
        check(lineBatchesStartAtDepth(storageesp::kTracerNearPlane, forward),
              "the default anchor starts just in front of the eye plane, never on it");
        check(!lineBatchesRunThroughTheCamera(),
              "the default anchor is off the eye ray, so the line spans the screen");
        check(lineBatchesSpanScreenAngle(0.05f),
              "every submitted tracer spans a real screen angle instead of one pixel");
        check(stripsFaceTheCamera(),
              "no submitted tracer strip is edge-on, so it covers pixels");
        check(stripsWidenWithDistance(),
              "the strip widens with distance so a long tracer keeps one width on screen");

        module.tracerOrigin = 1; // Feet: published by the tick from the player position
        g_batches.clear();
        renderStorageEsp(world.levelRenderer.data(), world.screenContext.data());
        check(lineVertices() == 2 * 2, "feet-anchored tracers reach the same containers");
        check(lineBatchesStartAtDepth(storageesp::kTracerNearPlane, forward),
              "a feet anchor sitting on the eye plane is clipped forward instead of dropped");
        check(!lineBatchesRunThroughTheCamera(),
              "feet-anchored tracers start at the player, not on the eye ray");
        check(lineBatchesSpanScreenAngle(0.05f) && stripsFaceTheCamera(),
              "the feet anchor draws visible lines on screen too");
        module.tracerOrigin = 0; // back to the default anchor

        module.tracerThickness = 1.0f;
        g_batches.clear();
        renderStorageEsp(world.levelRenderer.data(), world.screenContext.data());
        check(fillBatches() == 0 && outlineBatches() == 1,
              "a hairline tracer skips the strip pass and stays a native line");
        module.tracerThickness = 2.0f;

        world.setRotation({90.0f, -90.0f}); // look straight down: nothing is in front of the camera
        tick(world);
        g_batches.clear();
        renderStorageEsp(world.levelRenderer.data(), world.screenContext.data());
        check(g_batches.empty(), "nothing is submitted when no container is in front of the camera");
        world.setRotation({0.0f, -90.0f});
        tick(world);

        module.tracer = false;
        g_batches.clear();
        renderStorageEsp(world.levelRenderer.data(), world.screenContext.data());
        check(g_batches.empty(), "with the boxes and the tracers both off nothing is submitted");
        module.outline = true;

        std::printf("storage esp cache upkeep\n");
        setBlock({10, 64, 9}, nullptr); // the chest got broken or moved
        module.scanSpeed = 3;           // Instant: one tick covers the whole area
        tick(world);
        check(!s_cache.contains({10, 64, 9}), "a broken container stops being highlighted on the next visit");
        check(s_cache.size() == 3, "the other findings stay");

        module.setMasterEnabled(false);
        g_batches.clear();
        renderStorageEsp(world.levelRenderer.data(), world.screenContext.data());
        check(g_batches.empty(), "a disabled module stops drawing at once");
        tick(world);
        check(s_cache.empty() && s_scan.cells.empty(),
              "disabling the module forgets the whole world on the next tick, off the render path");

        module.setMasterEnabled(true);
        tick(world);
        check(s_cache.size() == 3, "re-enabling rescans the area around the player");

        world.place({300.5f, 64.0f, 300.5f}); // a teleport to an empty part of the world
        world.setCamera({300.5f, 64.5f, 300.5f});
        g_batches.clear();
        renderStorageEsp(world.levelRenderer.data(), world.screenContext.data());
        check(g_batches.empty(), "containers left behind by a teleport are not drawn");
        tick(world);
        check(s_cache.empty(), "containers the moved scan area cannot reach any more are forgotten");
        check(s_scan.region.anchor.x == 300 && s_scan.region.anchor.z == 300,
              "the sweep re-centers on the player after a teleport");

        std::printf("storage esp copper chests\n");
        setBlock({302, 64, 301}, "minecraft:oxidized_copper_chest");
        setBlock({303, 64, 301}, "minecraft:waxed_copper_chest");
        setBlock({304, 64, 301}, "minecraft:chest");
        tick(world);
        const auto* copper = s_cache.find({302, 64, 301});
        const auto* waxed = s_cache.find({303, 64, 301});
        const auto* plain = s_cache.find({304, 64, 301});
        check(copper && copper->kind == Kind::CopperChest,
              "an oxidized copper chest is remembered in its own group");
        check(waxed && waxed->kind == Kind::CopperChest,
              "waxed copper chests share the copper group");
        check(plain && plain->kind == Kind::Chest,
              "a wooden chest next to them stays in the chest group");

        g_batches.clear();
        renderStorageEsp(world.levelRenderer.data(), world.screenContext.data());
        check(outlineBatches() == 2,
              "copper chests are drawn as their own batch, apart from wooden chests");

        module.showCopperChests = false;
        g_batches.clear();
        renderStorageEsp(world.levelRenderer.data(), world.screenContext.data());
        check(outlineBatches() == 1, "turning the copper group off hides it immediately");
        module.showCopperChests = true;

        setBlock({302, 64, 301}, nullptr);
        setBlock({303, 64, 301}, nullptr);
        setBlock({304, 64, 301}, nullptr);
        tick(world);
        check(s_cache.empty(), "copper chests that were broken stop being highlighted");

        std::printf("storage esp config\n");
        module.showBarrels = false;
        module.showChestsColor = 0xFF112233u;
        module.showCopperChests = false;
        module.showCopperChestsColor = 0xFF123456u;
        module.tracer = true;
        module.tracerThickness = 5.0f;
        module.tracerOpacity = 0.5f;
        module.tracerOrigin = 1;
        module.lineThickness = 4.0f;
        module.throughWalls = false;
        module.modelSizedBoxes = false;
        module.pulse = true;
        module.scanRadius = 40;
        module.scanHeight = 5;
        module.scanSpeed = 3;
        module.maxBoxes = 12;
        nlohmann::json saved;
        module.saveConfig(saved);

        StorageEspModule restored;
        restored.loadConfig(saved);
        check(!restored.showBarrels && restored.showChests, "highlight groups round-trip");
        check(!restored.showCopperChests, "the copper chest group round-trips");
        check(restored.showChestsColor == 0xFF112233u, "colors round-trip");
        check(restored.showCopperChestsColor == 0xFF123456u, "the copper chest color round-trips");
        check(restored.tracer, "the tracer toggle round-trips");
        check(near(restored.tracerThickness, 5.0f) && near(restored.tracerOpacity, 0.5f),
              "the tracer style round-trips");
        check(restored.tracerOrigin == 1,
              "the tracer origin round-trips through the launcher radio format");
        check(near(restored.lineThickness, 4.0f), "thickness round-trips");
        check(!restored.throughWalls && !restored.modelSizedBoxes, "render options round-trip");
        check(restored.pulse, "animation toggles round-trip");
        check(restored.scanRadius == 40 && restored.scanHeight == 5, "scan area round-trips");
        check(restored.scanSpeed == 3, "the radio value round-trips through the launcher format");
        check(restored.maxBoxes == 12, "the box cap round-trips");
        check(saved["showChestsColor"].is_string(), "colors are saved as #rrggbb strings for the picker");
        check(saved["tracerOrigin"].is_string(), "the tracer origin is saved as a radio value string");

        nlohmann::json legacy;
        legacy["showOutline"] = false;
        legacy["thickness"] = 9.0f;
        legacy["radius"] = 500;   // out of range: clamped instead of rejected
        legacy["verticalRadius"] = 0;
        legacy["maxHighlights"] = 0;
        legacy["xray"] = true;
        legacy["tightBoxes"] = false;
        legacy["opacity"] = 2.0f;
        legacy["scanSpeed"] = "Fast";
        legacy["showTracers"] = true;
        legacy["tracerWidth"] = 20.0f;   // out of range: clamped instead of rejected
        legacy["tracerOpacity"] = 3.0f;
        legacy["tracerOrigin"] = "Feet";
        StorageEspModule fromLegacy;
        fromLegacy.loadConfig(legacy);
        check(!fromLegacy.outline, "legacy showOutline is imported");
        check(near(fromLegacy.lineThickness, 9.0f), "legacy thickness is imported");
        check(fromLegacy.scanRadius == 64 && fromLegacy.scanHeight == 2, "the scan area is clamped to a safe size");
        check(fromLegacy.maxBoxes == 1, "the box cap never goes below one box");
        check(fromLegacy.throughWalls && !fromLegacy.modelSizedBoxes, "legacy xray/tightBoxes are imported");
        check(near(fromLegacy.fillOpacity, 1.0f), "opacity clamps to fully opaque");
        check(fromLegacy.scanSpeed == 2, "a bare option name from an older config resolves");
        check(fromLegacy.tracer, "the showTracers alias is imported");
        check(near(fromLegacy.tracerThickness, 10.0f), "the tracer width clamps to the widest line");
        check(near(fromLegacy.tracerOpacity, 1.0f), "the tracer opacity clamps to fully opaque");
        check(fromLegacy.tracerOrigin == 1, "a bare tracer origin name resolves");

        nlohmann::json speedAsIndex;
        speedAsIndex["scanSpeed"] = 0;
        StorageEspModule numericSpeed;
        numericSpeed.loadConfig(speedAsIndex);
        check(numericSpeed.scanSpeed == 0, "a numeric scan speed (the legacy key type) still loads");

        nlohmann::json originAsIndex;
        originAsIndex["tracerOrigin"] = 1;
        originAsIndex["tracer"] = false;
        StorageEspModule numericOrigin;
        numericOrigin.loadConfig(originAsIndex);
        check(numericOrigin.tracerOrigin == 1 && !numericOrigin.tracer,
              "a numeric tracer origin still loads and tracers stay opt-in");
    }

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("all storage esp integration checks passed\n");
        return 0;
    }
    std::printf("%d storage esp integration check(s) failed\n", g_failures);
    return 1;
}

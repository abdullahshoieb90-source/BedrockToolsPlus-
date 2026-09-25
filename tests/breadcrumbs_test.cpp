// Integration-style host test for the Breadcrumbs module.
// It drives the real trail recorder and render path with small in-memory
// Minecraft stand-ins, then verifies config persistence.
//
// Build: g++ -std=c++20 -I include -I src -I tests/fakepl -I tests/fakejson
//        tests/breadcrumbs_test.cpp -o /tmp/breadcrumbs_test
// Run:   /tmp/breadcrumbs_test

#include <array>
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

namespace {

struct Batch {
    int mode = -1;
    int reservedVertices = 0;
    int emittedVertices = 0;
    std::vector<std::uint32_t> colors;
    std::vector<bedrocktools::sdk::Vec3> vertices;
};

std::vector<Batch> g_batches;
Batch g_current;

std::uint32_t pack(float r, float g, float b, float a) {
    const auto byte = [](float value) {
        return static_cast<std::uint32_t>(value * 255.0f + 0.5f) & 0xFFu;
    };
    return (byte(a) << 24) | (byte(r) << 16) | (byte(g) << 8) | byte(b);
}

void fakeRenderLevel(void*, void*, void*) {}

// Stands in for the game's own renderLevel so the hook's ordering is checkable:
// the world has to be rendered before the trail is submitted.
int g_originalCalls = 0;
int g_batchesWhenOriginalRan = -1;
void countingOriginal(void*, void*, void*) {
    ++g_originalCalls;
    g_batchesWhenOriginalRan = static_cast<int>(g_batches.size());
}

void fakeTessBegin(void*, void*, int mode, int vertexCount, int) {
    g_current = Batch{};
    g_current.mode = mode;
    g_current.reservedVertices = vertexCount;
}
void fakeTessColor(void*, float r, float g, float b, float a) {
    g_current.colors.push_back(pack(r, g, b, a));
}
void fakeTessVertex(void*, float x, float y, float z) {
    g_current.vertices.push_back({x, y, z});
    ++g_current.emittedVertices;
}
void fakeRenderMesh(void*, void*, void*, char*) {
    g_batches.push_back(std::move(g_current));
}

template <typename T, std::size_t N>
void writeAt(std::array<std::byte, N>& storage, std::size_t offset, const T& value) {
    std::memcpy(storage.data() + offset, &value, sizeof(T));
}

}  // namespace

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
        default:
            return 0;
    }
}
}  // namespace bedrocktools::memory

namespace bedrocktools::events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
}  // namespace bedrocktools::events

// Include the production implementation so this test can drive its render hook
// without adding a test-only API to the module.
#include "modules/visual/breadcrumbs.cpp"

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

float alphaOf(std::uint32_t color) {
    return static_cast<float>((color >> 24) & 0xFFu) / 255.0f;
}

}  // namespace

int main() {
    using namespace bedrocktools::sdk;
    using namespace bedrocktools::sdk::offsets;

    std::printf("breadcrumbs trail\n");
    {
        breadcrumbs::Trail trail;
        trail.setInterval(3);
        trail.setMaxPoints(4);

        check(!trail.onTick({0.0f, 64.0f, 0.0f}) && !trail.onTick({0.0f, 64.0f, 0.0f}),
              "no sample is stored before the interval elapses");
        check(trail.onTick({0.0f, 64.0f, 0.0f}) && trail.size() == 1,
              "the tick that completes the interval stores a sample");
        check(!trail.onTick({5.0f, 64.0f, 0.0f}) && !trail.onTick({6.0f, 64.0f, 0.0f}) &&
                  trail.onTick({7.0f, 64.0f, 0.0f}) && trail.size() == 2,
              "the interval restarts after every stored sample");

        breadcrumbs::Trail dedupe;
        dedupe.setInterval(1);
        check(dedupe.onTick({10.4f, 64.0f, 10.2f}), "the first sample is always stored");
        check(!dedupe.onTick({10.9f, 64.1f, 10.8f}),
              "moving inside the same block column adds nothing");
        check(dedupe.onTick({11.0f, 64.0f, 10.8f}), "crossing into the next block stores a sample");
        check(dedupe.onTick({11.2f, 66.5f, 10.9f}),
              "a height change of a block or more stores a sample inside the same column");

        breadcrumbs::Trail bounded;
        bounded.setInterval(1);
        bounded.setMaxPoints(3);
        for (int i = 0; i < 10; ++i) {
            bounded.onTick({static_cast<float>(i), 64.0f, 0.0f});
        }
        check(bounded.size() == 3, "the trail never grows past its limit");
        check(near(bounded.points().front().x, 7.0f) && near(bounded.points().back().x, 9.0f),
              "the oldest samples are dropped first");

        bounded.clear();
        check(bounded.empty(), "clear empties the trail");

        check(near(breadcrumbs::Trail::fade(0, 3), 0.4f) &&
                  near(breadcrumbs::Trail::fade(2, 3), 1.0f),
              "the trail fades from its head towards full opacity at the tail");

        const auto box = breadcrumbs::footprint({10.7f, 64.0f, 10.2f});
        check(near(box[0].x, 10.0f) && near(box[0].z, 10.0f) &&
                  near(box[0].y, 64.0f + breadcrumbs::kFootprintLift),
              "the footprint sits on the block below the sample, lifted off the ground");
        check(near(box[2].x, 11.0f) && near(box[2].z, 11.0f), "the footprint covers a whole block");

        const auto head = breadcrumbs::arrowhead({0.5f, 64.0f, 0.5f}, {1.5f, 64.0f, 0.5f});
        check(head.valid && near(head.tip.x, 1.5f - breadcrumbs::kArrowBackOff) &&
                  near(head.tip.z, 0.5f),
              "the arrow stops just short of the step it points at");
        // dir = +X, so the perpendicular is -Z: one wing sits behind the tip on
        // one side, the other mirrored on the far side.
        check(near(head.left.x, head.right.x) &&
                  near((head.left.z + head.right.z) * 0.5f, head.tip.z) &&
                  near(head.left.z, head.tip.z - breadcrumbs::kArrowLength * 0.8f),
              "the arrow wings are symmetric around the direction of travel");
        check(!breadcrumbs::arrowhead({0.5f, 64.0f, 0.5f}, {0.5f, 64.0f, 0.5f}).valid,
              "a step with no direction draws no arrow");
    }

    std::printf("breadcrumbs render integration\n");

    alignas(std::max_align_t) std::array<std::byte, 640> player{};
    alignas(std::max_align_t) std::array<std::byte, 64> shape{};
    alignas(std::max_align_t) std::array<std::byte, 256> screenContext{};
    alignas(std::max_align_t) std::array<std::byte, 0x500> levelRenderer{};
    alignas(std::max_align_t) std::array<std::byte, 0x1100> playerRenderer{};
    alignas(std::max_align_t) std::array<float, 4> colorHolder{0.2f, 0.3f, 0.4f, 0.5f};
    alignas(std::max_align_t) Vec3 playerPosition{10.5f, 65.5f, 10.5f};
    std::uint64_t tessellator = 0;

    const AABB aabb{{10.2f, 64.0f, 10.2f}, {10.8f, 65.8f, 10.8f}};
    writeAt(shape, AABBShapeComponent::mAABB, aabb);

    void* componentPointer = &playerPosition;
    void* shapePointer = shape.data();
    writeAt(player, Actor::mStateVectorComponent, componentPointer);
    writeAt(player, Actor::mStateVectorComponent + BuiltInActorComponents::mAABBShapeComponent,
            shapePointer);

    void* tessellatorPointer = &tessellator;
    void* colorHolderPointer = colorHolder.data();
    writeAt(screenContext, ScreenContext::mTessellator, tessellatorPointer);
    writeAt(screenContext, ScreenContext::mColorHolder, colorHolderPointer);

    void* playerRendererPointer = playerRenderer.data();
    writeAt(levelRenderer, LevelRenderer::mLevelRendererPlayer, playerRendererPointer);
    const Vec3 camera{10.5f, 65.5f, 2.0f};
    writeAt(playerRenderer, LevelRendererPlayer::mCamPos, camera);

    {
        BreadcrumbsModule module;
        module.onInit();
        module.setMasterEnabled(true);
        module.tickInterval = 1;
        module.trail().setInterval(1);

        bedrocktools::events::LocalPlayerTickEvent event{
            reinterpret_cast<bedrocktools::sdk::Player*>(player.data())};
        bedrocktools::events::bus().publish(event);
        check(module.trail().size() == 1, "the client tick records a sample at the player's feet");
        check(module.trail().size() == 1 && near(module.trail().points()[0].y, 64.0f),
              "the sample uses the bottom of the player's AABB, not the eye height");

        bedrocktools::events::bus().publish(event);
        check(module.trail().size() == 1, "standing still adds no sample");

        playerPosition = {11.5f, 65.5f, 10.5f};
        bedrocktools::events::bus().publish(event);
        check(module.trail().size() == 2, "walking into the next block adds a sample");

        module.setMasterEnabled(false);
        check(module.trail().empty(), "disabling the module drops the trail");
    }

    {
        BreadcrumbsModule module;
        module.onInit();
        module.setMasterEnabled(true);
        {
            std::lock_guard lock(module.trailMutex());
            module.trail().setInterval(1);
            module.trail().onTick({10.0f, 64.0f, 10.0f});
            module.trail().onTick({11.0f, 64.0f, 10.0f});
            module.trail().onTick({12.0f, 64.0f, 10.0f});
        }

        g_batches.clear();
        renderBreadcrumbs(levelRenderer.data(), screenContext.data());

        check(g_batches.size() == 2, "the trail and the arrowheads are submitted separately");
        check(g_batches.size() == 2 && g_batches[0].mode == 4 &&
                  g_batches[0].reservedVertices == 28 && g_batches[0].emittedVertices == 28,
              "three samples draw three block outlines plus two connector lines");
        check(g_batches.size() == 2 && g_batches[1].emittedVertices == 8,
              "two steps draw two arrowheads");
        check(g_batches.size() == 2 && g_batches[0].colors.size() == 3 &&
                  alphaOf(g_batches[0].colors[0]) < alphaOf(g_batches[0].colors[1]) &&
                  alphaOf(g_batches[0].colors[1]) < alphaOf(g_batches[0].colors[2]),
              "the trail fades towards its head");
        check(g_batches.size() == 2 &&
                  (g_batches[0].colors[2] & 0x00FFFFFFu) == (module.trailColor & 0x00FFFFFFu),
              "the newest sample keeps the picked colour");
        check(g_batches.size() == 2 && g_batches[1].colors.size() == 2 &&
                  (g_batches[1].colors[1] & 0x00FFFFFFu) == 0x00FFFFFFu,
              "arrowheads are drawn in white whatever colour the trail uses");
        check(g_batches.size() == 2 && !g_batches[0].vertices.empty() &&
                  near(g_batches[0].vertices[0].x, 10.0f - camera.x) &&
                  near(g_batches[0].vertices[0].y,
                       64.0f + breadcrumbs::kFootprintLift - camera.y),
              "vertices are emitted camera relative");
        check(near(colorHolder[0], 0.2f) && near(colorHolder[3], 0.5f),
              "renderer restores ScreenContext color state");

        g_renderLevelOriginal = &countingOriginal;
        g_originalCalls = 0;
        g_batchesWhenOriginalRan = -1;
        g_batches.clear();
        renderLevelHook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_originalCalls == 1, "the hook still calls the game's own renderLevel");
        check(g_batchesWhenOriginalRan == 0 && !g_batches.empty(),
              "the world renders before the trail is submitted");
        g_renderLevelOriginal = nullptr;

        module.setMasterEnabled(false);
        g_batches.clear();
        renderBreadcrumbs(levelRenderer.data(), screenContext.data());
        check(g_batches.empty(), "a disabled module draws nothing");
    }

    std::printf("breadcrumbs config\n");
    {
        BreadcrumbsModule source;
        source.tickInterval = 12;
        source.maxPoints = 250;
        source.trailColor = 0xFF123456u;

        nlohmann::json saved;
        source.saveConfig(saved);
        check(saved["trailColor"].get<std::string>() == "#FF123456",
              "trail colour keeps its alpha byte in the config");

        BreadcrumbsModule loaded;
        loaded.loadConfig(saved);
        check(loaded.tickInterval == 12 && loaded.maxPoints == 250 &&
                  loaded.trailColor == 0xFF123456u,
              "interval, length and colour round-trip");
    }

    {
        nlohmann::json picker;
        picker["trailColor"] = "#0A1B2C";
        picker["tickInterval"] = 0;
        picker["maxPoints"] = 99999;

        BreadcrumbsModule migrated;
        migrated.loadConfig(picker);
        check(migrated.trailColor == 0xFF0A1B2Cu,
              "picker colours without an alpha byte become opaque");
        check(migrated.tickInterval == 1 && migrated.maxPoints == 2000,
              "interval and trail length clamp to the menu range");
    }

    {
        BreadcrumbsModule module;
        {
            std::lock_guard lock(module.trailMutex());
            module.trail().setInterval(1);
            module.trail().onTick({1.0f, 64.0f, 1.0f});
            module.trail().onTick({2.0f, 64.0f, 1.0f});
        }
        nlohmann::json press;
        press["clearTrailButton"] = true;
        module.loadConfig(press);
        check(module.trail().empty(), "the Clear Trail button empties the trail");

        nlohmann::json saved;
        module.saveConfig(saved);
        check(saved["clearTrailButton"].get<bool>() == false,
              "the button never stays pressed in the saved config");
    }

    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d breadcrumbs check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all breadcrumbs checks passed\n");
    return 0;
}

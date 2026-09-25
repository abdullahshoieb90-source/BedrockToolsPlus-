// Integration-style host test for the Chunk Border module.
// It drives the real line generator and render path with small in-memory
// Minecraft stand-ins, then verifies config persistence.
//
// Build: g++ -std=c++20 -I include -I src -I tests/fakepl -I tests/fakejson
//        tests/chunkborder_test.cpp -o /tmp/chunkborder_test
// Run:   /tmp/chunkborder_test

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
    std::uint32_t color = 0;
    std::vector<bedrocktools::sdk::Vec3> vertices;
};

std::vector<Batch> g_batches;
Batch g_current;
float g_lastColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};

std::uint32_t pack(float r, float g, float b, float a) {
    const auto byte = [](float value) {
        return static_cast<std::uint32_t>(value * 255.0f + 0.5f) & 0xFFu;
    };
    return (byte(a) << 24) | (byte(r) << 16) | (byte(g) << 8) | byte(b);
}

void fakeRenderLevel(void*, void*, void*) {}

// Stands in for the game's own renderLevel so the hook's ordering is checkable:
// the world has to be rendered before the border is submitted.
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
    g_lastColor[0] = r;
    g_lastColor[1] = g;
    g_lastColor[2] = b;
    g_lastColor[3] = a;
}
void fakeTessVertex(void*, float x, float y, float z) {
    g_current.vertices.push_back({x, y, z});
    ++g_current.emittedVertices;
}
void fakeRenderMesh(void*, void*, void*, char*) {
    g_current.color = pack(g_lastColor[0], g_lastColor[1], g_lastColor[2], g_lastColor[3]);
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
#include "modules/visual/chunkborder.cpp"

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

}  // namespace

int main() {
    using namespace bedrocktools::sdk;
    using namespace bedrocktools::sdk::offsets;

    std::printf("chunk border geometry\n");

    const auto anchor = chunkborder::chunkAnchor({10.5f, 65.0f, -3.0f});
    check(near(anchor.x, 0.0f) && near(anchor.z, -16.0f), "anchor snaps down to the chunk origin");
    const auto negative = chunkborder::chunkAnchor({-1.0f, 0.0f, -1.0f});
    check(near(negative.x, -16.0f) && near(negative.z, -16.0f),
          "anchor rounds towards negative infinity, not towards zero");

    const auto border = chunkborder::buildBorder({10.5f, 65.0f, -3.0f}, 2.0f, 2);
    check(border.corners.size() == 4, "the four chunk corner posts are drawn once each");
    // 7 inner offsets x 4 walls, plus 193 rings x 4 sides over -64..320.
    check(border.grid.size() == 7 * 4 + 193 * 4, "grid holds the inner posts and the rings");
    check(border.adjacent.size() == 12, "the twelve surrounding corner posts are drawn");
    check(border.lineCount() == 816, "default spacing draws 816 lines in total");

    bool spansWorld = true;
    for (const auto& line : border.corners) {
        if (!near(line.from.y, chunkborder::kWorldBottom) || !near(line.to.y, chunkborder::kWorldTop)) {
            spansWorld = false;
        }
    }
    check(spansWorld, "corner posts span the whole build height");

    const auto sparse = chunkborder::buildBorder({0.0f, 0.0f, 0.0f}, 0.0f, 0);
    check(sparse.corners.empty() && sparse.grid.empty() && sparse.adjacent.size() == 12,
          "zero spacings switch both grid families off but keep the neighbouring posts");

    std::printf("chunk border render integration\n");

    alignas(std::max_align_t) std::array<std::byte, 640> player{};
    alignas(std::max_align_t) std::array<std::byte, 256> screenContext{};
    alignas(std::max_align_t) std::array<std::byte, 0x500> levelRenderer{};
    alignas(std::max_align_t) std::array<std::byte, 0x1100> playerRenderer{};
    alignas(std::max_align_t) std::array<float, 4> colorHolder{0.2f, 0.3f, 0.4f, 0.5f};
    alignas(std::max_align_t) Vec3 playerPosition{10.5f, 65.0f, -3.0f};
    std::uint64_t tessellator = 0;

    void* componentPointer = &playerPosition;
    writeAt(player, Actor::mStateVectorComponent, componentPointer);

    void* tessellatorPointer = &tessellator;
    void* colorHolderPointer = colorHolder.data();
    writeAt(screenContext, ScreenContext::mTessellator, tessellatorPointer);
    writeAt(screenContext, ScreenContext::mColorHolder, colorHolderPointer);

    void* playerRendererPointer = playerRenderer.data();
    writeAt(levelRenderer, LevelRenderer::mLevelRendererPlayer, playerRendererPointer);
    const Vec3 camera{10.5f, 65.5f, 2.0f};
    writeAt(playerRenderer, LevelRendererPlayer::mCamPos, camera);

    {
        ChunkBorderModule module;
        module.onInit();
        module.setMasterEnabled(true);
        check(module.enabled, "enabling the module turns it on");

        bedrocktools::events::LocalPlayerTickEvent event{
            reinterpret_cast<bedrocktools::sdk::Player*>(player.data())};
        bedrocktools::events::bus().publish(event);

        g_batches.clear();
        renderChunkBorder(levelRenderer.data(), screenContext.data());

        check(g_batches.size() == 3, "corners, grid and neighbours are submitted as three batches");
        check(g_batches.size() == 3 && g_batches[0].mode == 4 &&
                  g_batches[0].reservedVertices == 8 && g_batches[0].emittedVertices == 8,
              "corner batch reserves and emits one vertex per post end");
        check(g_batches.size() == 3 && g_batches[1].emittedVertices == 1600,
              "grid batch emits every grid line");
        check(g_batches.size() == 3 && g_batches[2].emittedVertices == 24,
              "neighbour batch emits the twelve surrounding posts");
        check(g_batches.size() == 3 && g_batches[0].color == module.cornerColor &&
                  g_batches[1].color == module.midColor && g_batches[2].color == module.adjColor,
              "each batch is drawn in its own colour");

        // The first corner post starts at the chunk origin, relative to the
        // camera the way renderLevel expects.
        check(g_batches.size() == 3 && !g_batches[0].vertices.empty() &&
                  near(g_batches[0].vertices[0].x, 0.0f - camera.x) &&
                  near(g_batches[0].vertices[0].y, chunkborder::kWorldBottom - camera.y) &&
                  near(g_batches[0].vertices[0].z, -16.0f - camera.z),
              "vertices are emitted camera relative");
        check(near(colorHolder[0], 0.2f) && near(colorHolder[1], 0.3f) &&
                  near(colorHolder[2], 0.4f) && near(colorHolder[3], 0.5f),
              "renderer restores ScreenContext color state");

        g_renderLevelOriginal = &countingOriginal;
        g_originalCalls = 0;
        g_batchesWhenOriginalRan = -1;
        g_batches.clear();
        renderLevelHook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_originalCalls == 1, "the hook still calls the game's own renderLevel");
        check(g_batchesWhenOriginalRan == 0 && !g_batches.empty(),
              "the world renders before the border is submitted");
        g_renderLevelOriginal = nullptr;

        module.horizLineSpacing = 0;
        module.vertLineSpacing = 0.0f;
        g_batches.clear();
        renderChunkBorder(levelRenderer.data(), screenContext.data());
        check(g_batches.size() == 1 && g_batches[0].emittedVertices == 24,
              "zero spacings leave only the neighbouring posts");

        module.setMasterEnabled(false);
        g_batches.clear();
        renderChunkBorder(levelRenderer.data(), screenContext.data());
        check(g_batches.empty(), "a disabled module draws nothing");
    }

    std::printf("chunk border config\n");
    {
        ChunkBorderModule source;
        source.vertLineSpacing = 4.0f;
        source.horizLineSpacing = 8;
        source.cornerColor = 0xFF123456u;
        source.midColor = 0xFFABCDEFu;
        source.adjColor = 0x8000FF00u;

        nlohmann::json saved;
        source.saveConfig(saved);
        check(saved["cornerColor"].get<std::string>() == "#FF123456",
              "corner colour keeps its alpha byte in the config");
        check(saved["adjColor"].get<std::string>() == "#8000FF00",
              "a translucent neighbour colour survives the round trip");

        ChunkBorderModule loaded;
        loaded.loadConfig(saved);
        check(near(loaded.vertLineSpacing, 4.0f) && loaded.horizLineSpacing == 8,
              "both spacings round-trip");
        check(loaded.cornerColor == 0xFF123456u && loaded.midColor == 0xFFABCDEFu &&
                  loaded.adjColor == 0x8000FF00u,
              "all three colours round-trip");
    }

    {
        // The launcher's colour picker writes "#RRGGBB" with no alpha byte;
        // reading it as 0x00RRGGBB would make the border invisible.
        nlohmann::json picker;
        picker["cornerColor"] = "#0A1B2C";
        picker["vertLineSpacing"] = 50.0f;
        picker["horizLineSpacing"] = 99;

        ChunkBorderModule migrated;
        migrated.loadConfig(picker);
        check(migrated.cornerColor == 0xFF0A1B2Cu,
              "picker colours without an alpha byte become opaque");
        check(near(migrated.vertLineSpacing, 16.0f) && migrated.horizLineSpacing == 16,
              "spacings clamp to the size of a chunk");
    }

    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d chunk border check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all chunk border checks passed\n");
    return 0;
}

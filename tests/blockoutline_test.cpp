// Integration-style host test for the Block Outline module.
// It drives the real tick target sampler and render path with small in-memory
// Minecraft stand-ins, then verifies config persistence and legacy aliases.
//
// Build: g++ -std=c++20 -I include -I src -I tests/fakepl -I tests/fakejson
//        tests/blockoutline_test.cpp -o /tmp/blockoutline_test
// Run:   /tmp/blockoutline_test

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
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
};

std::vector<Batch> g_batches;
Batch g_currentBatch;
void* g_hitResult = nullptr;

void fakeRenderLevel(void*, void*, void*) {}
void fakeTessBegin(void*, void*, int mode, int vertexCount, int) {
    g_currentBatch = {mode, vertexCount, 0};
}
void fakeTessColor(void*, float, float, float, float) {}
void fakeTessVertex(void*, float, float, float) {
    ++g_currentBatch.emittedVertices;
}
void fakeRenderMesh(void*, void*, void*, char*) {
    g_batches.push_back(g_currentBatch);
}
void* fakeGetHitResult(void*) {
    return g_hitResult;
}

template <typename T, std::size_t N>
void writeAt(std::array<std::byte, N>& storage, std::size_t offset, const T& value) {
    std::memcpy(storage.data() + offset, &value, sizeof(T));
}

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
        case SignatureId::LevelGetHitResult:
            return reinterpret_cast<std::uintptr_t>(&fakeGetHitResult);
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

// Include the production implementation so this test can invoke its internal
// render hook helpers without adding a test-only API to the module.
#include "modules/visual/blockoutline.cpp"

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

} // namespace

int main() {
    using namespace bedrocktools::sdk;
    using namespace bedrocktools::sdk::offsets;

    std::printf("block outline render integration\n");

    alignas(std::max_align_t) std::array<std::byte, 640> player{};
    alignas(std::max_align_t) std::array<std::byte, 16> level{};
    alignas(std::max_align_t) std::array<std::byte, 96> hit{};
    alignas(std::max_align_t) std::array<std::byte, 256> screenContext{};
    alignas(std::max_align_t) std::array<std::byte, 0x500> levelRenderer{};
    alignas(std::max_align_t) std::array<std::byte, 0x1100> playerRenderer{};
    alignas(std::max_align_t) std::array<float, 4> colorHolder{0.2f, 0.3f, 0.4f, 0.5f};
    std::uint64_t tessellator = 0;

    void* levelPointer = level.data();
    writeAt(player, Actor::mLevel, levelPointer);

    const int blockType = HitResult::TypeBlock;
    const int upFace = blockoutline::Up;
    const BlockPos blockPosition{10, 64, -3};
    writeAt(hit, HitResult::mType, blockType);
    writeAt(hit, HitResult::mFacing, upFace);
    writeAt(hit, HitResult::mBlockPos, blockPosition);
    g_hitResult = hit.data();

    void* tessellatorPointer = &tessellator;
    void* colorHolderPointer = colorHolder.data();
    writeAt(screenContext, ScreenContext::mTessellator, tessellatorPointer);
    writeAt(screenContext, ScreenContext::mColorHolder, colorHolderPointer);

    void* playerRendererPointer = playerRenderer.data();
    writeAt(levelRenderer, LevelRenderer::mLevelRendererPlayer, playerRendererPointer);
    const Vec3 camera{10.5f, 65.5f, 2.0f};
    writeAt(playerRenderer, LevelRendererPlayer::mCamPos, camera);

    {
        BlockOutlineModule module;
        module.onInit();
        module.setMasterEnabled(true);
        updateTarget(player.data());

        g_batches.clear();
        renderBlockOutline(levelRenderer.data(), screenContext.data());
        check(g_batches.size() == 2, "default thick outline submits quad and crisp-line batches");
        check(g_batches.size() >= 1 && g_batches[0].mode == 1 &&
              g_batches[0].reservedVertices == 96 && g_batches[0].emittedVertices == 96,
              "12 thick edges emit 96 camera-facing quad vertices");
        check(g_batches.size() >= 2 && g_batches[1].mode == 4 &&
              g_batches[1].reservedVertices == 24 && g_batches[1].emittedVertices == 24,
              "hairline pass emits all 12 block edges");
        check(near(colorHolder[0], 0.2f) && near(colorHolder[1], 0.3f) &&
              near(colorHolder[2], 0.4f) && near(colorHolder[3], 0.5f),
              "renderer restores ScreenContext color state");

        module.outline = false;
        module.fill = true;
        module.fillFaceOnly = true;
        g_batches.clear();
        renderBlockOutline(levelRenderer.data(), screenContext.data());
        check(g_batches.size() == 1 && g_batches[0].mode == 1 &&
              g_batches[0].emittedVertices == 4,
              "face-only fill emits exactly one selected face");

        module.fillFaceOnly = false;
        g_batches.clear();
        renderBlockOutline(levelRenderer.data(), screenContext.data());
        check(g_batches.size() == 1 && g_batches[0].emittedVertices == 24,
              "full fill emits all six block faces");

        const int entityType = HitResult::TypeEntity;
        writeAt(hit, HitResult::mType, entityType);
        updateTarget(player.data());
        g_batches.clear();
        renderBlockOutline(levelRenderer.data(), screenContext.data());
        check(g_batches.empty(), "entity/no-block hit clears the block overlay immediately");

        writeAt(hit, HitResult::mType, blockType);
        updateTarget(player.data());
        module.setMasterEnabled(false);
        g_batches.clear();
        renderBlockOutline(levelRenderer.data(), screenContext.data());
        check(g_batches.empty(), "disabling the module clears its cached target");
    }

    std::printf("block outline config\n");
    {
        BlockOutlineModule source;
        source.outline = false;
        source.outlineColor = 0xFF123456u;
        source.outlineOpacity = 0.65f;
        source.lineThickness = 7.0f;
        source.fill = true;
        source.fillColor = 0xFFABCDEFu;
        source.fillOpacity = 0.35f;
        source.fillFaceOnly = true;
        source.rainbow = true;
        source.rainbowSpeed = 0.8f;
        source.pulse = true;
        source.pulseSpeed = 0.6f;
        source.throughWalls = true;

        nlohmann::json saved;
        source.saveConfig(saved);
        check(saved["outlineColor"].get<std::string>() == "#123456",
              "outline color saves as menu-compatible RGB");
        check(saved["fillColor"].get<std::string>() == "#ABCDEF",
              "fill color saves independently");

        BlockOutlineModule loaded;
        loaded.loadConfig(saved);
        check(!loaded.outline && loaded.fill && loaded.fillFaceOnly,
              "outline/fill mode round-trips");
        check((loaded.outlineColor & 0xFFFFFFu) == 0x123456u &&
              (loaded.fillColor & 0xFFFFFFu) == 0xABCDEFu,
              "both colors round-trip");
        check(near(loaded.outlineOpacity, 0.65f) && near(loaded.fillOpacity, 0.35f) &&
              near(loaded.lineThickness, 7.0f),
              "opacity and thickness round-trip");
        check(loaded.rainbow && near(loaded.rainbowSpeed, 0.8f) &&
              loaded.pulse && near(loaded.pulseSpeed, 0.6f) && loaded.throughWalls,
              "animation and depth settings round-trip");
    }

    {
        nlohmann::json legacy;
        legacy["Color"] = "#0A1B2C";
        legacy["Opacity"] = 2.0f;
        legacy["Thickness"] = 0.2f;
        legacy["overlay"] = true;
        legacy["overlayColor"] = "#FFEEDD";
        legacy["overlayOpacity"] = -1.0f;
        legacy["fullOverlay"] = false;
        legacy["renderThrough"] = true;

        BlockOutlineModule migrated;
        migrated.loadConfig(legacy);
        check((migrated.outlineColor & 0xFFFFFFu) == 0x0A1B2Cu,
              "legacy Color key migrates");
        check(near(migrated.outlineOpacity, 1.0f) && near(migrated.fillOpacity, 0.0f) &&
              near(migrated.lineThickness, 1.0f),
              "legacy numeric settings clamp to safe renderer ranges");
        check(migrated.fill && migrated.fillFaceOnly && migrated.throughWalls,
              "legacy overlay/depth flags migrate");
    }

    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d block outline integration check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all block outline integration checks passed\n");
    return 0;
}

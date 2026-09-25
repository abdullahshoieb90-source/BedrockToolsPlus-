// Integration-style host test for the Light Overlay module.
// It drives the real glyph font and render path against an in-memory
// BlockSource stand-in, then verifies config persistence.
//
// Build: g++ -std=c++20 -I include -I src -I tests/fakepl -I tests/fakejson
//        tests/lightoverlay_test.cpp -o /tmp/lightoverlay_test
// Run:   /tmp/lightoverlay_test

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

using bedrocktools::sdk::BlockPos;

struct Batch {
    int mode = -1;
    int emittedVertices = 0;
    std::vector<std::uint32_t> colors;
};

std::vector<Batch> g_batches;
Batch g_current;
std::uint32_t g_lastColor = 0;

// A tiny world: everything at or below y 63 on the negative-X half is solid,
// the air above and on the positive-X half is lit.
int g_solidBlock = 0;
int g_airBlock = 0;

std::uint32_t pack(float r, float g, float b, float a) {
    const auto byte = [](float value) {
        return static_cast<std::uint32_t>(value * 255.0f + 0.5f) & 0xFFu;
    };
    return (byte(a) << 24) | (byte(r) << 16) | (byte(g) << 8) | byte(b);
}

bool worldIsSolid(const BlockPos& position) {
    return position.y <= 63 && position.x <= 0;
}

void fakeRenderLevel(void*, void*, void*) {}

// Stands in for the game's own renderLevel so the hook's ordering is checkable:
// the world has to be rendered before the labels are submitted.
int g_originalCalls = 0;
int g_batchesWhenOriginalRan = -1;
void countingOriginal(void*, void*, void*) {
    ++g_originalCalls;
    g_batchesWhenOriginalRan = static_cast<int>(g_batches.size());
}

void fakeTessBegin(void*, void*, int mode, int, int) {
    g_current = Batch{};
    g_current.mode = mode;
}
void fakeTessColor(void*, float r, float g, float b, float a) {
    g_lastColor = pack(r, g, b, a);
    g_current.colors.push_back(g_lastColor);
}
void fakeTessVertex(void*, float, float, float) { ++g_current.emittedVertices; }
void fakeRenderMesh(void*, void*, void*, char*) { g_batches.push_back(std::move(g_current)); }

void* fakeGetBlock(void*, const BlockPos& position) {
    return worldIsSolid(position) ? static_cast<void*>(&g_solidBlock)
                                  : static_cast<void*>(&g_airBlock);
}
float fakeGetBrightness(void*, const BlockPos& position) {
    return position.x >= 0 ? 1.0f : 0.0f;
}
bool fakeIsSolidBlockingBlock(void*, const BlockPos& position) { return worldIsSolid(position); }

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
        case SignatureId::BlockSourceGetBlock:
            return reinterpret_cast<std::uintptr_t>(&fakeGetBlock);
        case SignatureId::BlockSourceGetBrightness:
            return reinterpret_cast<std::uintptr_t>(&fakeGetBrightness);
        case SignatureId::BlockSourceIsSolidBlockingBlock:
            return reinterpret_cast<std::uintptr_t>(&fakeIsSolidBlockingBlock);
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
#include "modules/visual/lightoverlay.cpp"

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

    std::printf("light overlay glyphs\n");
    {
        check(lightoverlay::digitGlyph(0).count == 5 && lightoverlay::digitGlyph(7).count == 2,
              "digits are drawn with their own stroke count");
        check(lightoverlay::digitGlyph(-1).count == 0 && lightoverlay::digitGlyph(10).count == 0,
              "anything outside 0-9 draws nothing");

        bool insideCell = true;
        for (int digit = 0; digit <= 9; ++digit) {
            const auto glyph = lightoverlay::digitGlyph(digit);
            if (glyph.count == 0) insideCell = false;
            for (std::size_t i = 0; i < glyph.count; ++i) {
                const auto& stroke = glyph.strokes[i];
                if (std::fabs(stroke.from.x) > lightoverlay::kGlyphHalfWidth + 0.0001f ||
                    std::fabs(stroke.to.x) > lightoverlay::kGlyphHalfWidth + 0.0001f ||
                    std::fabs(stroke.from.y) > lightoverlay::kGlyphHalfHeight + 0.0001f ||
                    std::fabs(stroke.to.y) > lightoverlay::kGlyphHalfHeight + 0.0001f) {
                    insideCell = false;
                }
            }
        }
        check(insideCell, "every stroke stays inside its glyph cell");

        check(lightoverlay::numberStrokes(5).size() == 5, "a single digit keeps its own strokes");
        check(lightoverlay::numberStrokes(15).size() ==
                  lightoverlay::digitGlyph(1).count + lightoverlay::digitGlyph(5).count,
              "a two-digit level draws both digits");
        check(lightoverlay::numberStrokes(-3).size() == lightoverlay::numberStrokes(0).size() &&
                  lightoverlay::numberStrokes(200).size() == lightoverlay::numberStrokes(99).size(),
              "out-of-range levels clamp instead of drawing garbage");

        const auto tens = lightoverlay::numberStrokes(15);
        bool leftIsTens = true;
        for (std::size_t i = 0; i < lightoverlay::digitGlyph(1).count; ++i) {
            if (tens[i].from.x > 0.0f) leftIsTens = false;
        }
        check(leftIsTens, "the tens digit is drawn left of the ones digit");

        check(lightoverlay::lightLevelFromBrightness(1.0f) == 15 &&
                  lightoverlay::lightLevelFromBrightness(0.0f) == 0 &&
                  lightoverlay::lightLevelFromBrightness(0.5f) == 8,
              "brightness maps onto the 0-15 light scale");
        check(lightoverlay::isDangerous(7, 7) && !lightoverlay::isDangerous(8, 7),
              "the danger threshold is inclusive");
        check(lightoverlay::clampRadius(-5) == 0 && lightoverlay::clampRadius(1000) == 32,
              "the scan radius clamps to a range the renderer can afford");

        bool facesOrthogonal = true;
        for (const auto& face : lightoverlay::kFaces) {
            const auto dot = face.right.x * face.up.x + face.right.y * face.up.y +
                             face.right.z * face.up.z;
            // right x up has to point out of the face, otherwise the number
            // would be mirrored on it.
            const float crossX = face.right.y * face.up.z - face.right.z * face.up.y;
            const float crossY = face.right.z * face.up.x - face.right.x * face.up.z;
            const float crossZ = face.right.x * face.up.y - face.right.y * face.up.x;
            if (!near(dot, 0.0f) || !near(crossX, static_cast<float>(face.neighbour.x)) ||
                !near(crossY, static_cast<float>(face.neighbour.y)) ||
                !near(crossZ, static_cast<float>(face.neighbour.z))) {
                facesOrthogonal = false;
            }
        }
        check(facesOrthogonal, "every face basis is orthogonal and faces outwards");
        check(lightoverlay::faceVisible(0, true) && !lightoverlay::faceVisible(1, true) &&
                  lightoverlay::faceVisible(3, false),
              "only-top-face keeps the top face and drops the rest");
    }

    std::printf("light overlay render integration\n");

    alignas(std::max_align_t) std::array<std::byte, 640> player{};
    alignas(std::max_align_t) std::array<std::byte, 256> dimension{};
    alignas(std::max_align_t) std::array<std::byte, 16> region{};
    alignas(std::max_align_t) std::array<std::byte, 256> screenContext{};
    alignas(std::max_align_t) std::array<std::byte, 0x500> levelRenderer{};
    alignas(std::max_align_t) std::array<std::byte, 0x1100> playerRenderer{};
    alignas(std::max_align_t) std::array<float, 4> colorHolder{0.2f, 0.3f, 0.4f, 0.5f};
    alignas(std::max_align_t) Vec3 playerPosition{0.5f, 63.5f, 0.5f};
    std::uint64_t tessellator = 0;

    void* componentPointer = &playerPosition;
    void* dimensionPointer = dimension.data();
    void* regionPointer = region.data();
    writeAt(player, Actor::mStateVectorComponent, componentPointer);
    writeAt(player, Actor::mDimension, dimensionPointer);
    writeAt(dimension, Dimension::mBlockSource, regionPointer);

    void* tessellatorPointer = &tessellator;
    void* colorHolderPointer = colorHolder.data();
    writeAt(screenContext, ScreenContext::mTessellator, tessellatorPointer);
    writeAt(screenContext, ScreenContext::mColorHolder, colorHolderPointer);

    void* playerRendererPointer = playerRenderer.data();
    writeAt(levelRenderer, LevelRenderer::mLevelRendererPlayer, playerRendererPointer);
    const Vec3 camera{0.5f, 64.5f, 0.5f};
    writeAt(playerRenderer, LevelRendererPlayer::mCamPos, camera);

    {
        // Nothing has ticked yet, so the module has no player to read from.
        LightOverlayModule waiting;
        waiting.onInit();
        waiting.setMasterEnabled(true);
        g_batches.clear();
        renderLightOverlay(levelRenderer.data(), screenContext.data());
        check(g_batches.empty(), "the overlay stays off until the first client tick");
    }

    {
        LightOverlayModule module;
        module.onInit();
        module.setMasterEnabled(true);
        module.radiusHorizontal = 0;
        module.radiusVertical = 0;

        bedrocktools::events::LocalPlayerTickEvent event{
            reinterpret_cast<bedrocktools::sdk::Player*>(player.data())};
        bedrocktools::events::bus().publish(event);

        g_batches.clear();
        renderLightOverlay(levelRenderer.data(), screenContext.data());

        // The block at the player's feet is solid; only the air above it and to
        // its +X side is exposed, so exactly two faces are labelled.
        check(g_batches.size() == 1 && g_batches[0].colors.size() == 2,
              "only the faces bordering air are labelled");
        check(g_batches.size() == 1 && g_batches[0].emittedVertices == 2 * 2 * 8,
              "each face draws the strokes of a two-digit light level");
        check(g_batches.size() == 1 && g_batches[0].colors[0] == module.safeColor,
              "a fully lit face is drawn in the safe colour");
        check(near(colorHolder[0], 0.2f) && near(colorHolder[3], 0.5f),
              "renderer restores ScreenContext color state");

        module.dangerThreshold = lightoverlay::kMaxLightLevel;
        g_batches.clear();
        renderLightOverlay(levelRenderer.data(), screenContext.data());
        check(g_batches.size() == 1 && g_batches[0].colors[0] == module.dangerColor,
              "a threshold at the top of the scale marks every face dangerous");
        module.dangerThreshold = 7;

        module.onlyTopFace = true;
        g_batches.clear();
        renderLightOverlay(levelRenderer.data(), screenContext.data());
        check(g_batches.size() == 1 && g_batches[0].colors.size() == 1,
              "only-top-face labels a single face per block");
        module.onlyTopFace = false;

        module.onlySolidBlocks = false;
        module.radiusHorizontal = 1;
        g_batches.clear();
        renderLightOverlay(levelRenderer.data(), screenContext.data());
        // 6 non-air blocks: the three on the dark side expose their top face
        // only, the three on the lit side expose their top and +X faces.
        check(g_batches.size() == 1 && g_batches[0].colors.size() == 9,
              "non-solid mode labels every non-air block in the scan volume");
        int dark = 0;
        if (g_batches.size() == 1) {
            for (const auto color : g_batches[0].colors) {
                if (color == module.dangerColor) ++dark;
            }
        }
        check(dark == 3, "faces next to unlit air are drawn in the danger colour");

        module.setMasterEnabled(false);
        g_batches.clear();
        renderLightOverlay(levelRenderer.data(), screenContext.data());
        check(g_batches.empty(),
              "disabling the module forgets the player, so no stale pointer is read");

        module.setMasterEnabled(true);
        // Re-enabling does not resurrect the cached player; the next client tick
        // has to publish it again.
        bedrocktools::events::bus().publish(event);
        g_renderLevelOriginal = &countingOriginal;
        g_originalCalls = 0;
        g_batchesWhenOriginalRan = -1;
        g_batches.clear();
        renderLevelHook(levelRenderer.data(), screenContext.data(), nullptr);
        check(g_originalCalls == 1, "the hook still calls the game's own renderLevel");
        check(g_batchesWhenOriginalRan == 0 && !g_batches.empty(),
              "the world renders before the labels are submitted");
        g_renderLevelOriginal = nullptr;
    }

    std::printf("light overlay config\n");
    {
        LightOverlayModule source;
        source.radiusHorizontal = 24;
        source.radiusVertical = 4;
        source.onlyTopFace = true;
        source.onlySolidBlocks = false;
        source.dangerThreshold = 3;
        source.safeColor = 0xFF123456u;
        source.dangerColor = 0x80ABCDEFu;

        nlohmann::json saved;
        source.saveConfig(saved);
        check(saved["dangerColor"].get<std::string>() == "#80ABCDEF",
              "a translucent danger colour keeps its alpha byte");

        LightOverlayModule loaded;
        loaded.loadConfig(saved);
        check(loaded.radiusHorizontal == 24 && loaded.radiusVertical == 4 &&
                  loaded.dangerThreshold == 3,
              "radii and threshold round-trip");
        check(loaded.onlyTopFace && !loaded.onlySolidBlocks, "both mode flags round-trip");
        check(loaded.safeColor == 0xFF123456u && loaded.dangerColor == 0x80ABCDEFu,
              "both colours round-trip");
    }

    {
        nlohmann::json picker;
        picker["safeColor"] = "#0A1B2C";
        picker["radiusHorizontal"] = 1000;
        picker["radiusVertical"] = -4;
        picker["dangerThreshold"] = 30;

        LightOverlayModule migrated;
        migrated.loadConfig(picker);
        check(migrated.safeColor == 0xFF0A1B2Cu,
              "picker colours without an alpha byte become opaque");
        check(migrated.radiusHorizontal == 32 && migrated.radiusVertical == 0,
              "radii clamp to the range the renderer can afford");
        check(migrated.dangerThreshold == 15, "the threshold clamps to the light scale");
    }

    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d light overlay check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all light overlay checks passed\n");
    return 0;
}

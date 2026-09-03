// Runtime-path regression test for Block Outline line thickness.
//
// The geometry-only test proves that wide strips can be generated, but the
// original regression happened after that: renderLevelHook submitted those
// strips with Minecraft's `selection_box` material. That material forces line
// primitives, so the quad mesh was reduced to hairlines and the slider had no
// visible effect. This test drives the real render hook with a fake screen
// context/tessellator and verifies that:
//
//   * thickness 1 emits only the classic selection-box hairline;
//   * thickness > 1 emits a quad pass through a fill-capable material;
//   * changing the setting changes the width of the submitted vertices;
//   * the compatibility fill is used when the opaque fill is unavailable.
//
// The production source is included directly so the anonymous render-hook
// state can be replaced by deterministic host fakes without exposing test-only
// APIs from the module.

#include "modules/visual/blockoutline.cpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

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

struct CapturedVertex {
    float x;
    float y;
    float z;
};

struct DrawCall {
    int primitive = 0;
    int declaredVertexCount = 0;
    void* material = nullptr;
    std::vector<CapturedVertex> vertices;
};

std::vector<DrawCall> g_drawCalls;
DrawCall g_pendingCall;

void fakeBegin(void*, void*, int primitive, int vertexCount, int) {
    g_pendingCall = {};
    g_pendingCall.primitive = primitive;
    g_pendingCall.declaredVertexCount = vertexCount;
}

void fakeColor(void*, float, float, float, float) {}

void fakeVertex(void*, float x, float y, float z) {
    g_pendingCall.vertices.push_back({x, y, z});
}

void fakeRenderMesh(void*, void*, void* material, char*) {
    g_pendingCall.material = material;
    g_drawCalls.push_back(g_pendingCall);
}

int g_failures = 0;

void check(bool condition, const char* description) {
    std::printf("  %s  %s\n", condition ? "ok  " : "FAIL", description);
    if (!condition) ++g_failures;
}

template <class T>
void writeAt(std::vector<std::byte>& storage, std::size_t offset, const T& value) {
    std::memcpy(storage.data() + offset, &value, sizeof(value));
}

struct FakeRenderer {
    std::vector<std::byte> levelRenderer;
    std::vector<std::byte> playerRenderer;
    std::vector<std::byte> screenContext;
    float colorHolder[4] = {0.2f, 0.3f, 0.4f, 0.5f};
    std::uint64_t tessellator = 0;

    FakeRenderer()
        : levelRenderer(bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer +
                        sizeof(void*) + 16),
          playerRenderer(bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial +
                         64),
          screenContext(bedrocktools::sdk::offsets::ScreenContext::mTessellator +
                        sizeof(void*) + 16) {
        void* player = playerRenderer.data();
        writeAt(levelRenderer,
                bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer,
                player);

        void* tess = &tessellator;
        writeAt(screenContext,
                bedrocktools::sdk::offsets::ScreenContext::mTessellator,
                tess);

        float* colors = colorHolder;
        writeAt(screenContext,
                bedrocktools::sdk::offsets::ScreenContext::mColorHolder,
                colors);

        // A diagonal view exposes three faces and gives the frame enough
        // geometry to catch both missing-pass and wrong-width regressions.
        const bedrocktools::sdk::Vec3 camera{2.0f, 2.0f, 2.0f};
        writeAt(playerRenderer,
                bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos,
                camera);
    }

    void render() {
        g_drawCalls.clear();
        renderLevelHook(levelRenderer.data(), screenContext.data(), nullptr);
    }
};

const DrawCall* findCall(int primitive) {
    for (const auto& call : g_drawCalls) {
        if (call.primitive == primitive) return &call;
    }
    return nullptr;
}

float submittedFrameWidth(const DrawCall* quadCall) {
    // Each visible edge is emitted as a flat camera-facing ribbon. The first
    // ribbon's corners are a, b, c, d where a = "from end + side" and
    // d = "from end - side", so the distance between vertices 0 and 3 is the
    // ribbon's full width (2 * half). The camera is subtracted from every
    // vertex, so the delta is unaffected by it.
    if (!quadCall || quadCall->vertices.size() < 4) return -1.0f;
    const float dx = quadCall->vertices[3].x - quadCall->vertices[0].x;
    const float dy = quadCall->vertices[3].y - quadCall->vertices[0].y;
    const float dz = quadCall->vertices[3].z - quadCall->vertices[0].z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

int main() {
    std::printf("block outline render path - line thickness\n");

    BlockOutlineModule module;
    module.enabled = true;
    module.show3d = false;

    g_tessellatorBegin = fakeBegin;
    g_tessellatorColor = fakeColor;
    g_tessellatorVertex = fakeVertex;
    g_renderMesh = fakeRenderMesh;
    g_renderLevelOriginal = nullptr;
    g_renderMaterialGroup = 0; // keep the deterministic fake materials below

    g_matSelection.sharedPtrData[0] = reinterpret_cast<void*>(0x1111);
    g_matOpaqueFill.sharedPtrData[0] = reinterpret_cast<void*>(0x2222);
    g_matFill.sharedPtrData[0] = reinterpret_cast<void*>(0x3333);

    {
        std::lock_guard lock(g_targetMutex);
        g_target = {0, 0, 0};
        g_hasTarget = true;
    }

    FakeRenderer renderer;

    // The default remains a crisp one-pixel line and does not waste a quad
    // submission.
    module.lineThickness = 1.0f;
    renderer.render();
    check(findCall(kQuadPrimitive) == nullptr,
          "thickness 1 emits no filled frame");
    const DrawCall* hairline = findCall(kLinePrimitive);
    check(hairline != nullptr,
          "thickness 1 keeps the hairline pass");
    check(hairline && hairline->material == static_cast<void*>(&g_matSelection),
          "hairline uses selection_box material");

    // This is the reported regression: a value above 1 must reach a solid
    // quad draw, and selection_box must never be used for it.
    module.lineThickness = 2.0f;
    renderer.render();
    const DrawCall* width2 = findCall(kQuadPrimitive);
    check(width2 != nullptr && !width2->vertices.empty(),
          "thickness 2 submits filled frame vertices");
    check(width2 && width2->material == static_cast<void*>(&g_matFill),
          "filled frame uses the opaque ui_fill_color so thickness never washes out");
    check(width2 && width2->declaredVertexCount ==
                        static_cast<int>(width2->vertices.size()),
          "filled frame vertex count matches tessellator submission");
    const float smallWidth = submittedFrameWidth(width2);
    check(std::fabs(smallWidth -
                    bedrocktools::modules::blockoutline::frameWidthForLineSize(2.0f)) < 1e-6f,
          "thickness 2 reaches the submitted vertex width");

    // Exercise the same loadConfig path used by the launcher callback, rather
    // than changing only the public field.
    nlohmann::json changed;
    changed["lineThickness"] = 10.0f;
    module.loadConfig(changed);
    renderer.render();
    const DrawCall* width10 = findCall(kQuadPrimitive);
    const float largeWidth = submittedFrameWidth(width10);
    check(largeWidth > smallWidth,
          "changing the slider from 2 to 10 widens rendered vertices");
    check(std::fabs(largeWidth -
                    bedrocktools::modules::blockoutline::frameWidthForLineSize(10.0f)) < 1e-6f,
          "launcher config value 10 reaches the render hook");

    // Old versions/resource packs may not expose selection_overlay_opaque.
    // The wide pass still has to render with the position-only UI fill.
    g_matOpaqueFill.sharedPtrData[0] = nullptr;
    module.lineThickness = 5.0f;
    renderer.render();
    const DrawCall* fallback = findCall(kQuadPrimitive);
    check(fallback && fallback->material == static_cast<void*>(&g_matFill),
          "wide frame falls back to ui_fill_color when needed");
    check(fallback && fallback->material != static_cast<void*>(&g_matSelection),
          "filled geometry never falls back to line-only selection_box while a fill exists");

    // Show 3D also uses quads. Its depth-tested/front and see-through/back
    // passes must both avoid the line-only material so the same slider keeps
    // working in either module mode.
    g_matOpaqueFill.sharedPtrData[0] = reinterpret_cast<void*>(0x2222);
    module.show3d = true;
    module.lineThickness = 6.0f;
    renderer.render();
    std::vector<void*> quadMaterials;
    for (const auto& call : g_drawCalls) {
        if (call.primitive == kQuadPrimitive) quadMaterials.push_back(call.material);
    }
    check(quadMaterials.size() == 2,
          "Show 3D emits front and back filled edge passes");
    check(quadMaterials.size() == 2 &&
              quadMaterials[0] == static_cast<void*>(&g_matOpaqueFill) &&
              quadMaterials[1] == static_cast<void*>(&g_matFill),
          "Show 3D uses opaque/front and see-through/back fill materials");
    bool threeDUsedLineMaterial = false;
    for (void* material : quadMaterials) {
        if (material == static_cast<void*>(&g_matSelection)) threeDUsedLineMaterial = true;
    }
    check(!threeDUsedLineMaterial,
          "Show 3D never submits wide geometry with selection_box");

    if (g_failures != 0) {
        std::printf("%d block outline render check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("block outline render-path checks passed\n");
    return 0;
}

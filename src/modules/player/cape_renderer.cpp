#include "cape_renderer.hpp"

#include "customcapes.hpp"
#include "cape_geometry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <bedrocktools/sdk/render/LevelRenderer.hpp>
#include <bedrocktools/sdk/render/LevelRendererPlayer.hpp>

#include "core/GameHooks.hpp"
#include "core/RenderState.hpp"
#include "core/memory/Hooks.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

// Provided by customcapes.cpp (declared at namespace scope, outside the
// anonymous namespace below, so the definition matches).
const CustomCapesModule* customCapesModule();

// World-space cape renderer.
//
// Everything below mirrors the Wings module, which is known to draw correctly
// on the same devices where skin patching does nothing: hook RenderLevel, take
// the tessellator out of the screen context, submit camera-relative vertices
// and let MeshHelpers draw the batch.

namespace {

using namespace bedrocktools::sdk::offsets;
namespace cape = bedrocktools::modules::cape;

typedef void (*Tessellator_begin_t)(void* tessellator, void* debugCallback, int primitiveMode,
                                    int vertexCount, int noIndices);
typedef void (*Tessellator_color_t)(void* tessellator, float r, float g, float b, float a);
typedef void (*Tessellator_vertex_t)(void* tessellator, float x, float y, float z);
typedef void (*Tessellator_vertexUv_t)(void* tessellator, float x, float y, float z, float u, float v);
typedef void (*MeshHelpers_renderMeshImmediately_t)(void* screenContext, void* tessellator,
                                                   void* material, char* pad);

// --- Material lookup (identical approach to Wings) -------------------------

struct CapeHashedString {
    std::uint64_t mStrHash = 0;
    std::string mStr;
    mutable const CapeHashedString* mLastMatch = nullptr;

    explicit CapeHashedString(const char* text) {
        if (!text) return;
        mStr = text;
        constexpr std::uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t kPrime = 0x100000001B3ULL;
        for (char ch : mStr) {
            mStrHash = static_cast<std::uint64_t>(static_cast<unsigned char>(ch)) ^ (kPrime * mStrHash);
        }
        (void)kOffset;
    }
};

struct CapeMaterialPtr {
    void* data[2]{nullptr, nullptr};
    CapeMaterialPtr() = default;
    CapeMaterialPtr(const CapeMaterialPtr&) = delete;
    CapeMaterialPtr& operator=(const CapeMaterialPtr&) = delete;
    explicit operator bool() const { return data[0] != nullptr; }
};

std::uintptr_t g_materialGroup = 0;
CapeMaterialPtr g_capeMaterial;

// Looks up a material by name. The cape needs a *textured, depth tested*
// material, which is why "player_cape"/"entity_alphatest" are preferred over
// the untextured selection_box the Wings module uses.
void ensureCapeMaterial() {
    if (!g_materialGroup || g_capeMaterial) return;
    static const char* kNames[] = {
        "player_cape",
        "entity_alphatest",
        "entity_static",
        "ui_textured_and_glcolor",
        "ui_textured",
    };
    void** vtable = *reinterpret_cast<void***>(g_materialGroup);
    if (!vtable || !vtable[VTable::RenderMaterialGroup_getMaterial]) return;
    using getMat_t = CapeMaterialPtr (*)(void*, const CapeHashedString*);
    for (const char* name : kNames) {
        CapeHashedString hashed(name);
        CapeMaterialPtr material =
            reinterpret_cast<getMat_t>(vtable[VTable::RenderMaterialGroup_getMaterial])(
                reinterpret_cast<void*>(g_materialGroup), &hashed);
        if (material) {
            g_capeMaterial.data[0] = material.data[0];
            g_capeMaterial.data[1] = material.data[1];
            return;
        }
    }
}

std::uintptr_t resolveAdrp(std::uint32_t* insns, std::size_t count, std::uint32_t targetReg) {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;
        if ((insn & 0x9F000000) == 0x90000000) {
            const std::uintptr_t page = ((std::uintptr_t)&insns[i] & ~0xFFFULL) +
                ((std::int64_t)((std::uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29) & 3) << 43) >> 31);
            for (std::size_t j = i + 1; j < count; ++j) {
                const std::uint32_t add = insns[j];
                if ((add & 0xFF000000) == 0x91000000 && ((add >> 5) & 0x1F) == targetReg &&
                    (add & 0x1F) == targetReg) {
                    std::uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == targetReg) break;
            }
        }
        if ((insn & 0x9F000000) == 0x10000000) {
            const std::int64_t imm = (std::int64_t)((std::uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29)) << 43) >> 43;
            return (std::uintptr_t)&insns[i] + imm;
        }
    }
    return 0;
}

// --- Player anchor ---------------------------------------------------------

CapeRendererModule* g_renderer = nullptr;

void (*g_renderLevelOriginal)(void* self, void* screenContext, void* a3) = nullptr;

void* g_localPlayer = nullptr;

// Body yaw comes from the actor rotation component, never the camera: the cape
// is attached to the model, so a third-person orbit must not spin it.
bool readAnchor(void* player, bedrocktools::sdk::Vec3& feetOut, bedrocktools::sdk::Vec2& rotOut) {
    if (!player || reinterpret_cast<std::uintptr_t>(player) < 0x1000) return false;

    const std::uintptr_t actor = reinterpret_cast<std::uintptr_t>(player);
    const std::uintptr_t components = *reinterpret_cast<std::uintptr_t*>(actor + Actor::mStateVectorComponent);
    if (components < 0x1000) return false;
    const std::uintptr_t aabbComponent =
        *reinterpret_cast<std::uintptr_t*>(actor + Actor::mStateVectorComponent + BuiltInActorComponents::mAABBShapeComponent);
    if (aabbComponent < 0x1000) return false;

    struct AABB {
        bedrocktools::sdk::Vec3 min;
        bedrocktools::sdk::Vec3 max;
    };
    const AABB box = *reinterpret_cast<AABB*>(aabbComponent + AABBShapeComponent::mAABB);
    if (!std::isfinite(box.min.x) || !std::isfinite(box.min.y) || !std::isfinite(box.min.z) ||
        !std::isfinite(box.max.x) || !std::isfinite(box.max.y) || !std::isfinite(box.max.z)) {
        return false;
    }
    if (box.max.y <= box.min.y) return false;

    feetOut = bedrocktools::sdk::Vec3{(box.min.x + box.max.x) * 0.5f, box.min.y,
                                      (box.min.z + box.max.z) * 0.5f};

    std::uintptr_t rotation = *reinterpret_cast<std::uintptr_t*>(actor + Actor::mActorRotationComponent);
    rotOut = rotation < 0x1000 ? bedrocktools::sdk::Vec2{0.0f, 0.0f}
                               : *reinterpret_cast<bedrocktools::sdk::Vec2*>(rotation);
    if (!std::isfinite(rotOut.y)) rotOut.y = 0.0f;
    return true;
}

// --- RenderLevel detour -----------------------------------------------------
//
// Vertex submission has two modes:
//
//   UV mode   Tessellator::vertexUV  - the PNG is sampled, the cape looks like
//                                      the picture the user picked.
//   Colour mode  no UV routine could be located on this build, so each vertex
//                gets the cape's own average colour. The shape, the flap and
//                the position are all real; only the picture is missing.
//
// The mode is decided once, from what the signatures resolved, and reported in
// the menu so a colour-only cape is never mistaken for a working one.

float capeAverageChannel(const std::vector<std::uint8_t>& pixels, int channel) {
    if (pixels.size() < 4) return 0.8f;
    // The visible face of the classic layout is x1..11, y1..17. Averaging the
    // whole canvas would include the transparent parts of a 64x32 authored PNG.
    constexpr int kCanvasWidth = cape::kCapeCanvasWidth;
    double sum = 0.0;
    std::size_t counted = 0;
    for (int y = 1; y < 17; ++y) {
        for (int x = 1; x < 11; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kCanvasWidth + x) * 4u + channel;
            if (i >= pixels.size()) continue;
            if (pixels[(static_cast<std::size_t>(y) * kCanvasWidth + x) * 4u + 3u] < 8) continue;
            sum += pixels[i];
            ++counted;
        }
    }
    if (counted == 0) return 0.8f;
    return static_cast<float>(sum / static_cast<double>(counted) / 255.0);
}

void drawCape(void* screenContext) {
    if (!g_renderer || !g_renderer->ready()) return;

    bedrocktools::sdk::Vec3 feet{};
    bedrocktools::sdk::Vec2 rot{};
    float speed = 0.0f;
    float verticalSpeed = 0.0f;
    float swayPhase = 0.0f;
    bool onGround = true;
    {
        std::lock_guard<std::mutex> lock(g_renderer->stateMutex());
        if (!g_renderer->hasAnchorLocked()) return;
        feet = g_renderer->feetLocked();
        rot = g_renderer->bodyRotLocked();
        speed = g_renderer->speedLocked();
        verticalSpeed = g_renderer->verticalSpeedLocked();
        onGround = g_renderer->onGroundLocked();
        swayPhase = g_renderer->swayPhaseLocked();
    }

    const CustomCapesModule* capes = customCapesModule();
    if (!capes) return;

    // The pixels live in the Custom Capes module. They only change when a cape
    // is picked, fitted or reloaded, so the copy is kept until the module
    // reports a new version instead of copying 8 KiB every frame.
    static std::vector<std::uint8_t> pixels;
    static std::uint32_t cachedVersion = 0;
    static bool cacheValid = false;
    const std::uint32_t version = capes->pixelsVersion();
    if (!cacheValid || version != cachedVersion) {
        pixels = capes->snapshotPixels();
        cachedVersion = version;
        cacheValid = true;
    }
    if (pixels.empty()) return;

    auto* levelRenderer = reinterpret_cast<bedrocktools::sdk::LevelRenderer*>(
        g_renderer->levelRenderer());
    if (!levelRenderer) return;
    auto* rendererPlayer = levelRenderer->playerRenderer();
    if (!rendererPlayer) return;

    const std::uintptr_t tessellator =
        *reinterpret_cast<std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(screenContext) +
                                           ScreenContext::mTessellator);
    if (tessellator < 0x1000) return;

    const bedrocktools::sdk::Vec3 camera = rendererPlayer->cameraPosition();

    // Build the mesh first: the tessellator needs the vertex count up front.
    cape::Vec3 right{};
    cape::Vec3 forward{};
    cape::bodyAxes(rot.y, right, forward);

    const float flap = cape::flapAngleDegrees(speed, verticalSpeed, onGround);
    const float sway = std::sin(swayPhase) * 6.0f;

    cape::Vertex vertices[16]{};
    const int count = cape::buildCapeMesh(cape::Vec3{feet.x, feet.y, feet.z}, right, forward,
                                          flap, sway, vertices, 16);
    if (count <= 0) return;

    ensureCapeMaterial();
    if (!g_capeMaterial) return;

    bedrocktools::core::ScopedShaderColor shaderColor(screenContext);
    if (!shaderColor) return;

    auto tessBegin = reinterpret_cast<Tessellator_begin_t>(g_renderer->tessBegin());
    auto tessColor = reinterpret_cast<Tessellator_color_t>(g_renderer->tessColor());
    auto renderMesh = reinterpret_cast<MeshHelpers_renderMeshImmediately_t>(g_renderer->renderMesh());
    if (!tessBegin || !renderMesh) return;

    char pad[0x58];
    std::memset(pad, 0, sizeof(pad));

    tessBegin(reinterpret_cast<void*>(tessellator), nullptr, 1, count, 0); // 1 = quads

    const bool textured = g_renderer->hasVertexUv();
    if (textured) {
        auto tessVertexUv = reinterpret_cast<Tessellator_vertexUv_t>(g_renderer->vertexUv());
        if (tessColor) tessColor(reinterpret_cast<void*>(tessellator), 1.0f, 1.0f, 1.0f, 1.0f);
        for (int i = 0; i < count; ++i) {
            const cape::Vertex& v = vertices[i];
            tessVertexUv(reinterpret_cast<void*>(tessellator),
                         v.p.x - camera.x, v.p.y - camera.y, v.p.z - camera.z, v.uv.u, v.uv.v);
        }
    } else {
        // No UV routine: fall back to the cape's average colour, which still
        // puts a correctly shaped, correctly moving cape on the player's back.
        static std::vector<std::uint8_t> lastPixels;
        static float r = 0.8f, g = 0.8f, b = 0.8f;
        if (lastPixels.size() != pixels.size() || !std::equal(pixels.begin(), pixels.end(), lastPixels.begin())) {
            lastPixels = pixels;
            r = capeAverageChannel(pixels, 0);
            g = capeAverageChannel(pixels, 1);
            b = capeAverageChannel(pixels, 2);
        }
        auto tessVertex = reinterpret_cast<Tessellator_vertex_t>(g_renderer->vertex());
        if (!tessVertex) return;
        for (int i = 0; i < count; ++i) {
            const cape::Vertex& v = vertices[i];
            if (tessColor) tessColor(reinterpret_cast<void*>(tessellator), r, g, b, 1.0f);
            tessVertex(reinterpret_cast<void*>(tessellator),
                       v.p.x - camera.x, v.p.y - camera.y, v.p.z - camera.z);
        }
    }

    renderMesh(screenContext, reinterpret_cast<void*>(tessellator),
               static_cast<void*>(&g_capeMaterial), pad);

    g_renderer->noteDraw(static_cast<std::uint32_t>(count));
    (void)pixels;
}

void renderLevelDetour(void* self, void* screenContext, void* a3) {
    if (g_renderLevelOriginal) g_renderLevelOriginal(self, screenContext, a3);
    if (!g_renderer || !g_renderer->enabled) return;
    drawCape(screenContext);
}

} // namespace

CapeRendererModule::CapeRendererModule()
    : Module("Cape Renderer",
             "Draws the cape chosen in Custom Capes directly in the world, so it shows even when your "
             "skin cannot be changed. Only you can see it.") {
    g_renderer = this;
    showInMenu = true;
    hideInHudEditor = true; // world overlay, not HUD
}

CapeRendererModule* CapeRendererModule::instance() {
    return g_renderer;
}

CapeRendererModule::~CapeRendererModule() {
    if (g_renderer == this) g_renderer = nullptr;
}

void CapeRendererModule::onInit() {
    resolveGameFunctions();

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_renderer) g_renderer->onFrame();
            (void)event;
        });
}

void CapeRendererModule::resolveGameFunctions() {
    const auto patch = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    if (patch) m_patchTarget = reinterpret_cast<void*>(patch);

    if (const auto fn = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin); fn) {
        m_tessBegin = reinterpret_cast<void*>(fn);
    }
    if (const auto fn = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor); fn) {
        m_tessColor = reinterpret_cast<void*>(fn);
    }
    if (const auto fn = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex); fn) {
        m_vertex = reinterpret_cast<void*>(fn);
    }
    // Textured submission. Without it the cape is still drawn, in one flat
    // colour, and the menu says so.
    if (const auto fn = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertexUv); fn) {
        m_vertexUv = reinterpret_cast<void*>(fn);
    }
    if (const auto fn = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2); fn) {
        m_renderMesh = reinterpret_cast<void*>(fn);
    } else if (const auto fn5 = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately); fn5) {
        m_renderMesh = reinterpret_cast<void*>(fn5);
    }

    if (const auto group = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderMaterialGroupCommon); group) {
        const std::uintptr_t table = resolveAdrp(reinterpret_cast<std::uint32_t*>(group), 2, 0);
        if (table) {
            g_materialGroup = table + MaterialGroup::mRenderMaterialGroupOffset;
            m_material = g_materialGroup;
        }
    }
}

void* CapeRendererModule::levelRenderer() const {
    void* client = bedrocktools::core::gamehooks::clientInstance();
    if (!client) return nullptr;
    auto* instance = reinterpret_cast<bedrocktools::sdk::ClientInstance*>(client);
    return static_cast<void*>(instance->levelRenderer());
}

void CapeRendererModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    auto handle = bedrocktools::hooks::install(m_patchTarget, (void*)&renderLevelDetour,
                                              (void**)&g_renderLevelOriginal);
    m_patched = handle != nullptr;
}

void CapeRendererModule::onEnable() {
    m_drawCount.store(0);
    m_frameCount.store(0);
    ensureCapeMaterial();
}

void CapeRendererModule::onDisable() {
    m_hasAnchor = false;
    g_localPlayer = nullptr;
}

void* CapeRendererModule::localPlayer() const {
    // The cached pointer goes stale on Leave World (the actor is freed and the
    // next one can land on the same address), so it is only used while the
    // client instance still resolves to that same player.
    void* client = bedrocktools::core::gamehooks::clientInstance();
    if (client) {
        auto* instance = reinterpret_cast<bedrocktools::sdk::ClientInstance*>(client);
        auto* current = instance->localPlayer();
        if (current) return static_cast<void*>(current);
    }
    return nullptr;
}

void CapeRendererModule::onFrame() {
    // Runs from the tick path, so the anchor is refreshed at the game's own
    // rate. Rendering only reads the published copy.
    m_frameCount.fetch_add(1, std::memory_order_release);

    void* player = localPlayer();
    g_localPlayer = player;
    if (!player) {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_hasAnchor = false;
        return;
    }

    bedrocktools::sdk::Vec3 feet{};
    bedrocktools::sdk::Vec2 rot{};
    if (!readAnchor(player, feet, rot)) {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_hasAnchor = false;
        return;
    }

    // Wall-clock delta for the sway and for turning the position change into a
    // speed (the tick has no dt).
    const auto now = std::chrono::steady_clock::now();
    float dt = m_lastFrameSeconds > 0.0f
        ? std::chrono::duration<float>(now - std::chrono::steady_clock::time_point{}).count() - m_lastFrameSeconds
        : 0.0f;
    const float nowSeconds =
        std::chrono::duration<float>(now.time_since_epoch()).count();
    dt = m_lastFrameSeconds > 0.0f ? nowSeconds - m_lastFrameSeconds : 0.0f;
    if (dt < 0.0f || dt > 0.25f) dt = 0.0f;
    m_lastFrameSeconds = nowSeconds;

    bool onGround = true;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_hasAnchor && dt > 0.0f) {
            const float dx = feet.x - m_feet.x;
            const float dy = feet.y - m_feet.y;
            const float dz = feet.z - m_feet.z;
            const float horizontal = std::sqrt(dx * dx + dz * dz) / dt;
            // A teleport must not read as a sprint.
            if (horizontal < 30.0f) {
                m_speed = m_speed * 0.6f + horizontal * 0.4f;
                m_verticalSpeed = m_verticalSpeed * 0.6f + (dy / dt) * 0.4f;
            } else {
                m_speed = 0.0f;
                m_verticalSpeed = 0.0f;
            }
        }
        // A player standing on nothing moves down; treat any sustained descent
        // as airborne so the cape streams upward while falling.
        onGround = std::fabs(m_verticalSpeed) < 0.6f;
        m_onGround = onGround;
        m_feet = feet;
        m_bodyRot = rot;
        m_hasAnchor = true;

        // Slow idle sway, independent of movement, so a standing player's cape
        // still reads as cloth.
        m_swayPhase += dt * 0.9f;
        if (m_swayPhase > 6.28318530718f) m_swayPhase -= 6.28318530718f;
    }
}


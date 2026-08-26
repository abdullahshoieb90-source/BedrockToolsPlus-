#include "entityesp.hpp"
#include "entityesp_geometry.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace bedrocktools::sdk::offsets;

// ---------------------------------------------------------------------------
// Engine function signatures (resolved once from the game binary)
// ---------------------------------------------------------------------------

typedef void (*Tessellator_begin_t)(void* tessellator, void* debugCallback, int primitiveMode, int vertexCount, int noIndices);
typedef void (*Tessellator_color_t)(void* tessellator, float r, float g, float b, float a);
typedef void (*Tessellator_vertex_t)(void* tessellator, float x, float y, float z);
typedef void (*MeshHelpers_renderMeshImmediately_t)(void* screenContext, void* tessellator, void* material, char* pad);

typedef bool (*Actor_isPlayer_t)(void* actor);
typedef bool (*Actor_isInvisible_t)(void* actor);

struct DistanceSortedActor {
    void* mActor;
    float mDistance;
    float _pad;
};

struct ActorVec {
    DistanceSortedActor* begin;
    DistanceSortedActor* end;
    DistanceSortedActor* cap;
};

typedef ActorVec (*Actor_fetchNearbyActorsSorted_t)(void* actor, void* extent, int actorType);

// BlockSource::isSolidBlockingBlock(BlockPos const&) — true for full opaque
// blocks that block movement and sight, false for transparent/partial ones.
struct BlockPosI {
    int x, y, z;
};
typedef bool (*BlockSource_isSolidBlockingBlock_t)(void* region, const BlockPosI& pos);

// ---------------------------------------------------------------------------
// Material plumbing (same approach as the Hitbox / Block Outline modules)
// ---------------------------------------------------------------------------

struct HashedString {
    uint64_t mStrHash;
    std::string mStr;
    mutable const HashedString* mLastMatch;

    HashedString() : mStrHash(0), mStr(), mLastMatch(nullptr) {}

    explicit HashedString(const char* str) : mLastMatch(nullptr) {
        mStr = str ? str : "";
        mStrHash = computeHash(mStr);
    }

private:
    static uint64_t computeHash(const std::string& str) {
        if (str.empty()) return 0;
        constexpr uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr uint64_t kPrime = 0x100000001B3ULL;
        uint64_t hash = kOffset;
        for (char ch : str)
            hash = static_cast<uint64_t>(static_cast<unsigned char>(ch)) ^ (kPrime * hash);
        return hash;
    }
};

struct MaterialPtr {
    void* sharedPtrData[2]{nullptr, nullptr};

    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;

    MaterialPtr(MaterialPtr&& other) noexcept
        : sharedPtrData{other.sharedPtrData[0], other.sharedPtrData[1]} {
        other.sharedPtrData[0] = nullptr;
        other.sharedPtrData[1] = nullptr;
    }

    MaterialPtr& operator=(MaterialPtr&& other) noexcept {
        if (this != &other) {
            sharedPtrData[0] = other.sharedPtrData[0];
            sharedPtrData[1] = other.sharedPtrData[1];
            other.sharedPtrData[0] = nullptr;
            other.sharedPtrData[1] = nullptr;
        }
        return *this;
    }

    ~MaterialPtr() {}

    explicit operator bool() const {
        return sharedPtrData[0] != nullptr;
    }
};

static std::uintptr_t resolveADRP(uint32_t* insns, size_t count, uint32_t targetReg) {
    for (size_t i = 0; i < count; i++) {
        uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;

        if ((insn & 0x9F000000) == 0x90000000) {
            uintptr_t page = ((uintptr_t)&insns[i] & ~0xFFFULL)
                           + ((int64_t)((uint64_t)(((insn >> 3) & 0x1FFFFC) | ((insn >> 29) & 3)) << 43) >> 31);

            for (size_t j = i + 1; j < count; j++) {
                uint32_t add = insns[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == targetReg &&
                    (add & 0x1F) == targetReg) {
                    uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == targetReg) break;
            }
        }
        if ((insn & 0x9F000000) == 0x10000000) {
            int64_t imm = (int64_t)((uint64_t)(((insn >> 3) & 0x1FFFFC) | (insn >> 29)) << 43) >> 43;
            return (uintptr_t)&insns[i] + imm;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static EntityESPModule* g_module = nullptr;

static Tessellator_begin_t                s_tessBegin = nullptr;
static Tessellator_color_t                s_tessColor = nullptr;
static Tessellator_vertex_t               s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;

static Actor_isPlayer_t                   s_actorIsPlayer = nullptr;
static Actor_isInvisible_t                s_actorIsInvisible = nullptr;
static Actor_fetchNearbyActorsSorted_t    s_actorFetchNearby = nullptr;
static BlockSource_isSolidBlockingBlock_t s_isSolidBlockingBlock = nullptr;

static MaterialPtr s_matSelection;
static MaterialPtr s_matFill;
static std::uintptr_t s_renderMaterialGroup = 0;

static void (*s_renderLevelOriginal)(void*, void*, void*) = nullptr;

// The game thread (LocalPlayer::tick, 20 tps) stores the timestamp of the
// latest tick and the local player pointer; the render thread reads them in
// onRender. Both are atomics so no lock is needed.
static std::atomic<std::int64_t> g_lastTickNs{0};
static std::atomic<void*> g_localPlayer{nullptr};

static void onLocalPlayerTick(void* player) {
    g_localPlayer.store(player, std::memory_order_release);
    g_lastTickNs.store(
        std::chrono::steady_clock::now().time_since_epoch().count(),
        std::memory_order_release);
}

// Fraction of the current 50 ms tick that has elapsed (0..1), measured from
// the tick timestamps. 1.0 before the first tick or when ticks are stalled
// (game paused): the box then simply sits on the actor's current AABB.
static float currentPartialTicks() {
    const std::int64_t last = g_lastTickNs.load(std::memory_order_acquire);
    if (last == 0) return 1.0f;
    const std::int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
    const double fraction = double(now - last) * 1e-6 / 50.0;
    if (fraction <= 0.0) return 0.0f;
    if (fraction >= 1.0) return 1.0f;
    return static_cast<float>(fraction);
}

static MaterialPtr getMaterial(const char* name) {
    if (!s_renderMaterialGroup) return {};

    HashedString hs(name);

    void** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);
    if (!vtable || !vtable[VTable::RenderMaterialGroup_getMaterial]) return {};

    using getMat_t = MaterialPtr(*)(void*, const HashedString*);
    return reinterpret_cast<getMat_t>(vtable[VTable::RenderMaterialGroup_getMaterial])(
        reinterpret_cast<void*>(s_renderMaterialGroup), &hs);
}

static void ensureMaterials() {
    if (!s_renderMaterialGroup) return;

    if (!s_matSelection) s_matSelection = getMaterial("selection_box");

    // Thick geometry is drawn as filled quads. The selection overlay material
    // is built for a translucent block highlight, so a vertex-color fill
    // keeps the chosen RGBA solid.
    if (!s_matFill) {
        static const char* kFillNames[] = {
            "ui_fill_color",
            "ui_textured_and_glcolor",
            "debug_filled_box",
            "selection_box"
        };
        for (const char* name : kFillNames) {
            s_matFill = getMaterial(name);
            if (s_matFill) break;
        }
    }
}

// ---------------------------------------------------------------------------
// Actor frame reading (AABB + current/previous position)
// ---------------------------------------------------------------------------

// Reads the actor's current AABB and its current/previous positions. The
// AABB is built around the *current* position; onRender slides it to the
// interpolated position.
static bool readActorFrame(void* actor,
                           bedrocktools::sdk::AABB& aabb,
                           bedrocktools::sdk::Vec3& pos,
                           bedrocktools::sdk::Vec3& prev,
                           bool& hasPrev) {
    aabb = {{0, 0, 0}, {0, 0, 0}};
    pos = {0, 0, 0};
    prev = {0, 0, 0};
    hasPrev = false;
    if (!actor) return false;

    const uintptr_t addr = reinterpret_cast<uintptr_t>(actor);
    const uintptr_t svc = *reinterpret_cast<uintptr_t*>(addr + Actor::mStateVectorComponent);
    if (!svc || svc < 0x1000) return false;

    pos = *reinterpret_cast<bedrocktools::sdk::Vec3*>(svc + StateVectorComponent::mPosition);
    prev = *reinterpret_cast<bedrocktools::sdk::Vec3*>(svc + StateVectorComponent::mPreviousPosition);
    hasPrev = true;

    const uintptr_t aabbComp = *reinterpret_cast<uintptr_t*>(
        addr + Actor::mStateVectorComponent + BuiltInActorComponents::mAABBShapeComponent);
    if (!aabbComp || aabbComp < 0x1000) return false;

    aabb = *reinterpret_cast<bedrocktools::sdk::AABB*>(aabbComp + AABBShapeComponent::mAABB);
    return true;
}

static bool readPosition(const void* actor, bedrocktools::sdk::Vec3& out) {
    if (!actor) return false;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(actor);
    const uintptr_t svc = *reinterpret_cast<uintptr_t*>(addr + Actor::mStateVectorComponent);
    if (!svc || svc < 0x1000) return false;
    out = *reinterpret_cast<bedrocktools::sdk::Vec3*>(svc + StateVectorComponent::mPosition);
    return true;
}

// ---------------------------------------------------------------------------
// Wall culling (only active while "Through Walls" is off)
// ---------------------------------------------------------------------------

// Amanatides & Woo voxel traversal: walks every voxel the segment
// camera -> target passes through and returns true as soon as a solid
// blocking block is found. The camera's own voxel is never tested, so the
// check keeps working when the camera clips into geometry.
static bool rayHitsSolid(void* region,
                         float ox, float oy, float oz,
                         float tx, float ty, float tz) {
    if (!region || !s_isSolidBlockingBlock) return false;

    const float dx = tx - ox;
    const float dy = ty - oy;
    const float dz = tz - oz;
    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist < 0.01f) return false;

    int x = (int)floorf(ox);
    int y = (int)floorf(oy);
    int z = (int)floorf(oz);
    const int ex = (int)floorf(tx);
    const int ey = (int)floorf(ty);
    const int ez = (int)floorf(tz);
    if (x == ex && y == ey && z == ez) return false;

    int stepX, stepY, stepZ;
    float tMaxX, tMaxY, tMaxZ;
    float tDeltaX, tDeltaY, tDeltaZ;
    constexpr float kInf = 1e30f;

    if (dx > 0.0f)      { stepX = 1;  tDeltaX = 1.0f / dx;      tMaxX = (x + 1 - ox) * tDeltaX; }
    else if (dx < 0.0f) { stepX = -1; tDeltaX = -1.0f / dx;     tMaxX = (ox - x) * tDeltaX; }
    else                { stepX = 0;  tDeltaX = kInf;           tMaxX = kInf; }

    if (dy > 0.0f)      { stepY = 1;  tDeltaY = 1.0f / dy;      tMaxY = (y + 1 - oy) * tDeltaY; }
    else if (dy < 0.0f) { stepY = -1; tDeltaY = -1.0f / dy;     tMaxY = (oy - y) * tDeltaY; }
    else                { stepY = 0;  tDeltaY = kInf;           tMaxY = kInf; }

    if (dz > 0.0f)      { stepZ = 1;  tDeltaZ = 1.0f / dz;      tMaxZ = (z + 1 - oz) * tDeltaZ; }
    else if (dz < 0.0f) { stepZ = -1; tDeltaZ = -1.0f / dz;     tMaxZ = (oz - z) * tDeltaZ; }
    else                { stepZ = 0;  tDeltaZ = kInf;           tMaxZ = kInf; }

    while (true) {
        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            x += stepX;
            if (tMaxX > dist) break;
            tMaxX += tDeltaX;
        } else if (tMaxY < tMaxZ) {
            y += stepY;
            if (tMaxY > dist) break;
            tMaxY += tDeltaY;
        } else {
            z += stepZ;
            if (tMaxZ > dist) break;
            tMaxZ += tDeltaZ;
        }

        // Reached the voxel holding the target point: the target itself is
        // never treated as blocking (a mob hugging a wall stays visible).
        if (x == ex && y == ey && z == ez) break;

        BlockPosI bp{x, y, z};
        if (s_isSolidBlockingBlock(region, bp)) return true;
    }

    return false;
}

// True when every sampled point of the box is hidden behind solid blocks.
// Samples the box center and the top-center, so a tall mob whose head pokes
// over a low wall stays visible while a fully hidden one is culled.
static bool isOccluded(void* region,
                       float camX, float camY, float camZ,
                       const bedrocktools::sdk::AABB& aabb) {
    if (!region || !s_isSolidBlockingBlock) return false;

    const float cx = (aabb.min.x + aabb.max.x) * 0.5f;
    const float cz = (aabb.min.z + aabb.max.z) * 0.5f;
    const float cy = (aabb.min.y + aabb.max.y) * 0.5f;

    if (!rayHitsSolid(region, camX, camY, camZ, cx, cy, cz)) return false;
    if (!rayHitsSolid(region, camX, camY, camZ, cx, aabb.max.y, cz)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

struct RenderCtx {
    void* tessellator;
    void* screenContext;
    float camX, camY, camZ;
    float alpha;
    bool thick;
    float halfWidth;
    void* matInner;
    void* matFill;
};

// Tessellator primitive modes used by Bedrock: 1 = quad list (4 vertices per
// quad), 4 = line list (2 vertices per line).
constexpr int kQuadPrimitive = 1;
constexpr int kLinePrimitive = 4;

static void drawLines(RenderCtx& ctx,
                      const std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>>& lines,
                      float r, float g, float b) {
    if (lines.empty()) return;

    char pad[0x58];

    // Thick pass: every segment becomes a camera-facing quad, so the
    // apparent width follows the thickness setting from any angle.
    if (ctx.thick) {
        s_tessBegin(ctx.tessellator, nullptr, kQuadPrimitive,
                    static_cast<int>(lines.size() * 8), 0);
        s_tessColor(ctx.tessellator, r, g, b, ctx.alpha);

        bool emitted = false;
        for (const auto& line : lines) {
            const bedrocktools::sdk::Vec3 p1 = {
                line.first.x - ctx.camX, line.first.y - ctx.camY, line.first.z - ctx.camZ};
            const bedrocktools::sdk::Vec3 p2 = {
                line.second.x - ctx.camX, line.second.y - ctx.camY, line.second.z - ctx.camZ};

            bedrocktools::sdk::Vec3 quad[4];
            if (!entityesp::thickQuad(p1, p2, ctx.halfWidth, quad)) continue;

            // Emitted with both windings so back-face culling never eats a
            // segment.
            for (int i = 0; i < 4; ++i)
                s_tessVertex(ctx.tessellator, quad[i].x, quad[i].y, quad[i].z);
            for (int i = 3; i >= 0; --i)
                s_tessVertex(ctx.tessellator, quad[i].x, quad[i].y, quad[i].z);
            emitted = true;
        }

        if (emitted) {
            std::memset(pad, 0, sizeof(pad));
            s_renderMesh(ctx.screenContext, ctx.tessellator, ctx.matFill, pad);
        }
    }

    // Core hairline pass: keeps the edge crisp and visible even when the
    // quads shrink below a pixel at long range.
    s_tessBegin(ctx.tessellator, nullptr, kLinePrimitive,
                static_cast<int>(lines.size() * 2), 0);
    s_tessColor(ctx.tessellator, r, g, b, ctx.alpha);

    for (const auto& line : lines) {
        s_tessVertex(ctx.tessellator,
                     line.first.x - ctx.camX, line.first.y - ctx.camY, line.first.z - ctx.camZ);
        s_tessVertex(ctx.tessellator,
                     line.second.x - ctx.camX, line.second.y - ctx.camY, line.second.z - ctx.camZ);
    }

    std::memset(pad, 0, sizeof(pad));
    s_renderMesh(ctx.screenContext, ctx.tessellator, ctx.matInner, pad);
}

static void renderLevelDetour(void* levelRenderer, void* screenContext, void* a3) {
    if (s_renderLevelOriginal) {
        s_renderLevelOriginal(levelRenderer, screenContext, a3);
    }
    if (g_module && g_module->enabled) {
        g_module->onRender(screenContext, levelRenderer);
    }
}

// ---------------------------------------------------------------------------
// Color helpers
// ---------------------------------------------------------------------------

static void colorToRgba(std::uint32_t color, float alpha, float& r, float& g, float& b, float& a) {
    r = ((color >> 16) & 0xFF) / 255.0f;
    g = ((color >> 8) & 0xFF) / 255.0f;
    b = (color & 0xFF) / 255.0f;
    a = alpha;
}

} // namespace

// ---------------------------------------------------------------------------
// EntityESPModule
// ---------------------------------------------------------------------------

EntityESPModule::EntityESPModule()
    : Module("Entity ESP",
             "Draws interpolated 3D hitboxes around nearby players, mobs, dropped items, projectiles and vehicles.") {
    showInMenu = true;
    // World-space overlay, not a HUD element.
    hideInHudEditor = true;
    g_module = this;
}

EntityESPModule::~EntityESPModule() {
    if (g_module == this) g_module = nullptr;
}

void EntityESPModule::onInit() {
    using bedrocktools::memory::SignatureId;

    m_renderLevel = reinterpret_cast<void*>(
        bedrocktools::memory::resolve(SignatureId::RenderLevel));

    s_tessBegin = reinterpret_cast<Tessellator_begin_t>(
        bedrocktools::memory::resolve(SignatureId::TessellatorBegin));
    s_tessColor = reinterpret_cast<Tessellator_color_t>(
        bedrocktools::memory::resolve(SignatureId::TessellatorColor));
    s_tessVertex = reinterpret_cast<Tessellator_vertex_t>(
        bedrocktools::memory::resolve(SignatureId::TessellatorVertex));

    std::uintptr_t renderMeshAddress = bedrocktools::memory::resolve(
        SignatureId::MeshHelpersRenderMeshImmediately2);
    if (!renderMeshAddress) {
        renderMeshAddress = bedrocktools::memory::resolve(
            SignatureId::MeshHelpersRenderMeshImmediately);
    }
    s_renderMesh = reinterpret_cast<MeshHelpers_renderMeshImmediately_t>(renderMeshAddress);

    const std::uintptr_t renderMaterialGroup = bedrocktools::memory::resolve(
        SignatureId::RenderMaterialGroupCommon);
    if (renderMaterialGroup) {
        const std::uintptr_t groupAddress = resolveADRP(
            reinterpret_cast<uint32_t*>(renderMaterialGroup), 2, 0);
        if (groupAddress) {
            s_renderMaterialGroup =
                groupAddress + MaterialGroup::mRenderMaterialGroupOffset;
        }
    }

    s_actorIsPlayer = reinterpret_cast<Actor_isPlayer_t>(
        bedrocktools::memory::resolve(SignatureId::ActorIsPlayer));
    s_actorIsInvisible = reinterpret_cast<Actor_isInvisible_t>(
        bedrocktools::memory::resolve(SignatureId::ActorIsInvisible));
    s_actorFetchNearby = reinterpret_cast<Actor_fetchNearbyActorsSorted_t>(
        bedrocktools::memory::resolve(SignatureId::ActorFetchNearbyActorsSorted));
    s_isSolidBlockingBlock = reinterpret_cast<BlockSource_isSolidBlockingBlock_t>(
        bedrocktools::memory::resolve(SignatureId::BlockSourceIsSolidBlockingBlock));

    // Tick probe: stores the local player and the tick timestamp used to
    // derive the partial tick for entity interpolation.
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { onLocalPlayerTick(event.player); });
}

void EntityESPModule::installRenderHook() {
    if (m_hookInstalled || !m_renderLevel) return;
    const auto handle = bedrocktools::hooks::install(
        m_renderLevel,
        reinterpret_cast<void*>(renderLevelDetour),
        reinterpret_cast<void**>(&s_renderLevelOriginal));
    m_hookInstalled = handle != nullptr;
}

void EntityESPModule::onEnable() {
    installRenderHook();
}

void EntityESPModule::onDisable() {
    // Drop the cached references so a stale frame can never touch an actor
    // from the world we left.
    g_localPlayer.store(nullptr, std::memory_order_release);
    g_lastTickNs.store(0, std::memory_order_release);
}

void EntityESPModule::onFrame() {
    // Rendering happens in the renderLevel hook (onRender); there is no
    // per-frame work to do from the FrameEvent dispatch.
}

bool EntityESPModule::enabledFor(entityesp::Group group) const {
    switch (group) {
        case entityesp::Group::Player: return showPlayers;
        case entityesp::Group::Mob: return showMobs;
        case entityesp::Group::Item: return showItems;
        case entityesp::Group::Projectile: return showProjectiles;
        case entityesp::Group::Vehicle: return showVehicles;
        default: return false;
    }
}

std::uint32_t EntityESPModule::colorFor(entityesp::Group group) const {
    switch (group) {
        case entityesp::Group::Player: return showPlayersColor;
        case entityesp::Group::Mob: return showMobsColor;
        case entityesp::Group::Item: return showItemsColor;
        case entityesp::Group::Projectile: return showProjectilesColor;
        case entityesp::Group::Vehicle: return showVehiclesColor;
        default: return 0xFFFFFFFFu;
    }
}

void EntityESPModule::onRender(void* screenContext, void* levelRenderer) {
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) return;
    if (!screenContext || !levelRenderer) return;
    if (reinterpret_cast<std::uintptr_t>(screenContext) < 0x1000 ||
        reinterpret_cast<std::uintptr_t>(levelRenderer) < 0x1000) return;

    // Tessellator + glColor holder come from the ScreenContext.
    void* tessellator = *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(screenContext) + ScreenContext::mTessellator);
    float* colorHolder = *reinterpret_cast<float**>(
        reinterpret_cast<std::uintptr_t>(screenContext) + ScreenContext::mColorHolder);
    if (reinterpret_cast<std::uintptr_t>(tessellator) < 0x1000 ||
        reinterpret_cast<std::uintptr_t>(colorHolder) < 0x1000) return;

    // Camera position from the LevelRendererPlayer.
    void* playerRenderer = *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(levelRenderer) + LevelRenderer::mLevelRendererPlayer);
    if (reinterpret_cast<std::uintptr_t>(playerRenderer) < 0x1000) return;
    const bedrocktools::sdk::Vec3 camera = *reinterpret_cast<bedrocktools::sdk::Vec3*>(
        reinterpret_cast<std::uintptr_t>(playerRenderer) + LevelRendererPlayer::mCamPos);

    void* localPlayer = g_localPlayer.load(std::memory_order_acquire);
    if (!localPlayer) return;

    // Wall culling needs the dimension's BlockSource (resolved once per frame).
    void* region = nullptr;
    if (!throughWalls && s_isSolidBlockingBlock) {
        const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(localPlayer);
        const std::uintptr_t dimension =
            *reinterpret_cast<std::uintptr_t*>(addr + Actor::mDimension);
        if (dimension >= 0x1000) {
            const std::uintptr_t blockSource =
                *reinterpret_cast<std::uintptr_t*>(dimension + Dimension::mBlockSource);
            if (blockSource >= 0x1000) region = reinterpret_cast<void*>(blockSource);
        }
    }

    ensureMaterials();
    void* overlayMaterial = reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(playerRenderer) +
        LevelRendererPlayer::mSelectionOverlayMaterial);
    void* matInner = s_matSelection ? static_cast<void*>(&s_matSelection) : overlayMaterial;
    void* matFill = s_matFill ? static_cast<void*>(&s_matFill) : matInner;
    if (!matFill) matFill = overlayMaterial;

    // The selection material modulates glColor; force it to white so our
    // vertex colors read exactly as configured.
    const float savedColor[4] = {colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3]};
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    // Menu thickness slider -> world-space half width. 1.0 (or lower) keeps
    // the classic hairline box; anything above is drawn as real geometry,
    // because GL line width is ignored by nearly every mobile GLES driver.
    float thickness = lineThickness;
    if (thickness < 1.0f) thickness = 1.0f;
    if (thickness > 10.0f) thickness = 10.0f;
    const float alpha = boxAlpha < 0.0f ? 0.0f : (boxAlpha > 1.0f ? 1.0f : boxAlpha);

    RenderCtx ctx{
        tessellator, screenContext,
        camera.x, camera.y, camera.z,
        alpha, thickness > 1.05f, thickness * 0.01f * 0.5f,
        matInner, matFill
    };

    // Partial tick (0..1) for position interpolation.
    const float partialTicks = interpolate ? currentPartialTicks() : 1.0f;

    auto drawActorBox = [&](void* actor, entityesp::Group group) {
        bedrocktools::sdk::AABB aabb{};
        bedrocktools::sdk::Vec3 pos{}, prev{};
        bool hasPrev = false;
        if (!readActorFrame(actor, aabb, pos, prev, hasPrev)) return;
        if (aabb.min == bedrocktools::sdk::Vec3{0, 0, 0} &&
            aabb.max == bedrocktools::sdk::Vec3{0, 0, 0}) return;

        // Slide the box (built around the current position) to the
        // interpolated render position.
        const bedrocktools::sdk::AABB box =
            hasPrev ? entityesp::interpolatedBox(aabb, pos, prev, partialTicks) : aabb;

        // Boxes fully hidden behind solid blocks are culled (ESP off).
        if (region && isOccluded(region, camera.x, camera.y, camera.z, box)) return;

        float r, g, b, a;
        colorToRgba(colorFor(group), alpha, r, g, b, a);

        const auto edges = entityesp::boxEdges(box);
        std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> lines;
        lines.reserve(edges.size());
        for (const auto& edge : edges) lines.push_back({edge.from, edge.to});
        drawLines(ctx, lines, r, g, b);
    };

    // ---- Self (third person only) ------------------------------------
    if (showSelf) {
        bedrocktools::sdk::Vec3 localPos{};
        if (readPosition(localPlayer, localPos)) {
            const float dx = camera.x - localPos.x;
            const float dy = camera.y - (localPos.y + 1.62f);
            const float dz = camera.z - localPos.z;
            const bool isThirdPerson = dx * dx + dy * dy + dz * dz > 0.05f;
            if (isThirdPerson) {
                drawActorBox(localPlayer, entityesp::Group::Player);
            }
        }
    }

    // ---- Nearby actors ------------------------------------------------
    float range = fetchRange;
    if (range < 8.0f) range = 8.0f;
    if (range > 180.0f) range = 180.0f;

    ActorVec actors{};
    if (s_actorFetchNearby) {
        bedrocktools::sdk::Vec3 extent = {range, range, range};
        actors = s_actorFetchNearby(localPlayer, &extent, 1);
    }

    if (actors.begin && actors.end) {
        for (DistanceSortedActor* it = actors.begin; it < actors.end; ++it) {
            void* ent = it->mActor;
            if (!ent || ent == localPlayer) continue;
            // The fetch extent is a cube around the player; its corners reach
            // further than the range, so drop anything outside the radius.
            if (it->mDistance > range) continue;

            const std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(ent);
            const std::uint32_t categories =
                *reinterpret_cast<std::uint32_t*>(addr + Actor::mCategories);
            const bool isPlayer = s_actorIsPlayer ? s_actorIsPlayer(ent) : false;

            const auto group = entityesp::classify(categories, isPlayer);
            if (!enabledFor(group)) continue;
            if (s_actorIsInvisible && s_actorIsInvisible(ent)) continue;

            drawActorBox(ent, group);
        }
    }

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

void EntityESPModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    auto readBool = [&](const char* key, bool& out) {
        if (j.contains(key) && j[key].is_boolean()) out = j[key].get<bool>();
    };
    readBool("showPlayers", showPlayers);
    readBool("showMobs", showMobs);
    readBool("showItems", showItems);
    readBool("showProjectiles", showProjectiles);
    readBool("showVehicles", showVehicles);
    readBool("throughWalls", throughWalls);
    readBool("interpolate", interpolate);
    readBool("showSelf", showSelf);

    if (j.contains("lineThickness")) {
        try { lineThickness = j["lineThickness"].get<float>(); } catch (...) {}
    }
    if (lineThickness < 1.0f) lineThickness = 1.0f;
    if (lineThickness > 10.0f) lineThickness = 10.0f;

    if (j.contains("boxAlpha")) {
        try { boxAlpha = j["boxAlpha"].get<float>(); } catch (...) {}
    }
    if (boxAlpha < 0.0f) boxAlpha = 0.0f;
    if (boxAlpha > 1.0f) boxAlpha = 1.0f;

    if (j.contains("fetchRange")) {
        try { fetchRange = j["fetchRange"].get<float>(); } catch (...) {}
    }
    if (fetchRange < 8.0f) fetchRange = 8.0f;
    if (fetchRange > 180.0f) fetchRange = 180.0f;

    // Colors are stored as AARRGGBB with the alpha byte kept opaque; the
    // effective alpha comes from boxAlpha. The menu color picker often
    // stores #RRGGBB (no alpha byte), so a short string is padded to
    // opaque.
    auto parseColor = [&](const std::string& key, std::uint32_t& outColor) {
        if (!j.contains(key) || !j[key].is_string()) return;
        std::string hexStr = j[key].get<std::string>();
        if (hexStr.empty()) return;
        if (hexStr[0] == '#') hexStr = hexStr.substr(1);
        else if (hexStr.size() > 1 && hexStr[0] == '0' &&
                 (hexStr[1] == 'x' || hexStr[1] == 'X')) hexStr = hexStr.substr(2);
        try {
            unsigned long parsed = std::stoul(hexStr, nullptr, 16);
            if (hexStr.size() <= 6) {
                outColor = 0xFF000000u | static_cast<std::uint32_t>(parsed);
            } else {
                outColor = 0xFF000000u | (static_cast<std::uint32_t>(parsed) & 0x00FFFFFFu);
            }
        } catch (...) {}
    };

    parseColor("showPlayersColor", showPlayersColor);
    parseColor("showMobsColor", showMobsColor);
    parseColor("showItemsColor", showItemsColor);
    parseColor("showProjectilesColor", showProjectilesColor);
    parseColor("showVehiclesColor", showVehiclesColor);
}

void EntityESPModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    j["showPlayers"] = showPlayers;
    j["showMobs"] = showMobs;
    j["showItems"] = showItems;
    j["showProjectiles"] = showProjectiles;
    j["showVehicles"] = showVehicles;
    j["throughWalls"] = throughWalls;
    j["interpolate"] = interpolate;
    j["showSelf"] = showSelf;
    j["lineThickness"] = lineThickness;
    j["boxAlpha"] = boxAlpha;
    j["fetchRange"] = fetchRange;

    // Single RGB picker values ("#RRGGBB"), like the Block Outline color.
    char hex[8];
    auto writeColor = [&](const char* key, std::uint32_t color) {
        std::snprintf(hex, sizeof(hex), "%06X", color & 0x00FFFFFFu);
        j[key] = std::string("#") + hex;
    };
    writeColor("showPlayersColor", showPlayersColor);
    writeColor("showMobsColor", showMobsColor);
    writeColor("showItemsColor", showItemsColor);
    writeColor("showProjectilesColor", showProjectilesColor);
    writeColor("showVehiclesColor", showVehiclesColor);
}

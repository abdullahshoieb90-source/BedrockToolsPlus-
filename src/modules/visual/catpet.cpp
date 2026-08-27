#include "modules/visual/catpet.hpp"

#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

// Cat geometry, palette, pose solver and follow solver (pure, host-testable -
// see catpet_shape.hpp).
namespace catpet = bedrocktools::modules::catpet;
namespace wings = bedrocktools::modules::wings;

namespace {

using namespace bedrocktools::sdk::offsets;

// ---------------------------------------------------------------------------
// RenderLevel hook plumbing (same pattern as Wings/Hitbox/Breadcrumbs)
// ---------------------------------------------------------------------------

typedef void (*Tessellator_begin_t)(void* tessellator, void* debugCallback, int primitiveMode, int vertexCount, int noIndices);
typedef void (*Tessellator_color_t)(void* tessellator, float r, float g, float b, float a);
typedef void (*Tessellator_vertex_t)(void* tessellator, float x, float y, float z);
typedef void (*MeshHelpers_renderMeshImmediately_t)(void* screenContext, void* tessellator, void* material, char* pad);

struct HashedString {
    std::uint64_t mStrHash;
    std::string mStr;
    mutable const HashedString* mLastMatch;
    HashedString() : mStrHash(0), mStr(), mLastMatch(nullptr) {}
    explicit HashedString(const char* str) : mLastMatch(nullptr) {
        mStr = str ? str : "";
        mStrHash = computeHash(mStr);
    }
private:
    static std::uint64_t computeHash(const std::string& s) {
        if (s.empty()) return 0;
        constexpr std::uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t kPrime = 0x100000001B3ULL;
        std::uint64_t h = kOffset;
        for (char ch : s) h = static_cast<std::uint64_t>(static_cast<unsigned char>(ch)) ^ (kPrime * h);
        return h;
    }
};

struct MaterialPtr {
    void* sharedPtrData[2]{nullptr, nullptr};
    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;
    MaterialPtr(MaterialPtr&& o) noexcept : sharedPtrData{o.sharedPtrData[0], o.sharedPtrData[1]} {
        o.sharedPtrData[0] = nullptr; o.sharedPtrData[1] = nullptr;
    }
    MaterialPtr& operator=(MaterialPtr&& o) noexcept {
        if (this != &o) {
            sharedPtrData[0] = o.sharedPtrData[0];
            sharedPtrData[1] = o.sharedPtrData[1];
            o.sharedPtrData[0] = nullptr; o.sharedPtrData[1] = nullptr;
        }
        return *this;
    }
    ~MaterialPtr() {}
    explicit operator bool() const { return sharedPtrData[0] != nullptr; }
};

static std::uintptr_t resolveADRP(std::uint32_t* insns, size_t count, std::uint32_t targetReg) {
    for (size_t i = 0; i < count; i++) {
        std::uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;
        if ((insn & 0x9F000000) == 0x90000000) {
            std::uintptr_t page = ((std::uintptr_t)&insns[i] & ~0xFFFULL)
                + ((int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29) & 3) << 43) >> 31);
            for (size_t j = i + 1; j < count; j++) {
                std::uint32_t add = insns[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == targetReg && (add & 0x1F) == targetReg) {
                    std::uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == targetReg) break;
            }
        }
        if ((insn & 0x9F000000) == 0x10000000) {
            int64_t imm = (int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29)) << 43) >> 43;
            return (std::uintptr_t)&insns[i] + imm;
        }
    }
    return 0;
}

CatPetModule* g_catpet = nullptr;

static Tessellator_begin_t s_tessBegin = nullptr;
static Tessellator_color_t s_tessColor = nullptr;
static Tessellator_vertex_t s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;

static MaterialPtr s_matSelection;
static MaterialPtr s_matFill;
static std::uintptr_t s_renderMaterialGroup = 0;

static void (*_renderLevel_orig)(void* _this, void* screenContext, void* a3) = nullptr;

// ---------------------------------------------------------------------------
// Tick -> render shared state
// ---------------------------------------------------------------------------

struct AABB {
    bedrocktools::sdk::Vec3 min{0, 0, 0};
    bedrocktools::sdk::Vec3 max{0, 0, 0};
};

struct CatSample {
    catpet::Vec3 pos{0.0f, 0.0f, 0.0f};   // cat feet center (world blocks)
    float yawDeg = 0.0f;
};

static std::mutex s_stateMutex;
static bool s_hasCat = false;
static CatSample s_prevSample{};
static CatSample s_curSample{};
static float s_tickInterval = 0.05f;
static std::chrono::steady_clock::time_point s_lastTickTime{};
static bool s_lastTickTimeValid = false;
static bool s_hasPrevSample = false;

// Follow state lives on the tick thread only.
static catpet::FollowState s_follow{};

static float lerpAngleDeg(float a, float b, float t) {
    float diff = b - a;
    while (diff > 180.0f) diff -= 360.0f;
    while (diff < -180.0f) diff += 360.0f;
    return a + diff * t;
}

static AABB getActorAABB(void* actor) {
    AABB aabb{};
    std::uintptr_t actorAddr = (std::uintptr_t)actor;
    if (actorAddr < 0x1000) return aabb;
    std::uintptr_t builtInPtr = *(std::uintptr_t*)(actorAddr + Actor::mStateVectorComponent);
    if (builtInPtr < 0x1000) return aabb;
    std::uintptr_t aabbComp = *(std::uintptr_t*)(actorAddr + Actor::mStateVectorComponent + BuiltInActorComponents::mAABBShapeComponent);
    if (aabbComp < 0x1000) return aabb;
    aabb = *(AABB*)(aabbComp + AABBShapeComponent::mAABB);
    return aabb;
}

static bedrocktools::sdk::Vec2 getActorRotation(void* actor) {
    bedrocktools::sdk::Vec2 rot{0, 0};
    std::uintptr_t actorAddr = (std::uintptr_t)actor;
    if (actorAddr < 0x1000) return rot;
    std::uintptr_t rotComp = *(std::uintptr_t*)(actorAddr + Actor::mActorRotationComponent);
    if (rotComp < 0x1000) return rot;
    rot = *(bedrocktools::sdk::Vec2*)rotComp;
    return rot;
}

static MaterialPtr getMaterial(const char* name) {
    if (!s_renderMaterialGroup) return {};
    HashedString hs(name);
    void** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);
    if (!vtable || !vtable[2]) return {};
    using getMat_t = MaterialPtr(*)(void*, const HashedString*);
    return reinterpret_cast<getMat_t>(vtable[2])((void*)s_renderMaterialGroup, &hs);
}

static void ensureMaterials() {
    if (!s_renderMaterialGroup) return;
    if (!s_matSelection) s_matSelection = getMaterial("selection_box");
    if (!s_matFill) {
        static const char* kFillNames[] = { "ui_fill_color", "ui_textured_and_glcolor", "debug_filled_box", "selection_box" };
        for (const char* n : kFillNames) {
            s_matFill = getMaterial(n);
            if (s_matFill) break;
        }
    }
}

// ---------------------------------------------------------------------------
// Cat rendering
// ---------------------------------------------------------------------------

// One face; both windings so back-face culling can never eat a face.
static void emitFace(void* tess, const catpet::Vec3 corners[wings::kCornerCount], const int ring[4],
                     const wings::FaceColor& color) {
    s_tessColor(tess, color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1.0f);
    for (int i = 0; i < 4; ++i) {
        const catpet::Vec3& v = corners[ring[i]];
        s_tessVertex(tess, v.x, v.y, v.z);
    }
    for (int i = 3; i >= 0; --i) {
        const catpet::Vec3& v = corners[ring[i]];
        s_tessVertex(tess, v.x, v.y, v.z);
    }
}

static void renderCatOverlay(void* levelRenderer, void* screenContext) {
    if (!screenContext || (std::uintptr_t)screenContext < 0x1000) return;
    if (!levelRenderer || (std::uintptr_t)levelRenderer < 0x1000) return;
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) return;
    if (!g_catpet) return;

    CatSample prev{}, cur{};
    float tickInterval = 0.05f;
    bool hasPrev = false;
    bool lastTickValid = false;
    std::chrono::steady_clock::time_point lastTick{};
    {
        std::lock_guard<std::mutex> lock(s_stateMutex);
        if (!s_hasCat) return;
        prev = s_prevSample;
        cur = s_curSample;
        tickInterval = s_tickInterval;
        hasPrev = s_hasPrevSample;
        lastTick = s_lastTickTime;
        lastTickValid = s_lastTickTimeValid;
    }

    if (!std::isfinite(cur.pos.x) || !std::isfinite(cur.pos.y) || !std::isfinite(cur.pos.z) ||
        !std::isfinite(cur.yawDeg)) return;

    // Interpolate the cat's anchor and yaw between the previous and current
    // tick samples by the elapsed fraction of the current tick (the same
    // client-side lerp the Wings module uses), so the cat glides at the
    // render rate instead of stuttering at 20 Hz.
    catpet::Vec3 pos = cur.pos;
    float yawDeg = cur.yawDeg;
    if (hasPrev && lastTickValid && tickInterval > 0.0f) {
        auto now = std::chrono::steady_clock::now();
        float f = std::chrono::duration<float>(now - lastTick).count() / tickInterval;
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        pos.x = prev.pos.x + (cur.pos.x - prev.pos.x) * f;
        pos.y = prev.pos.y + (cur.pos.y - prev.pos.y) * f;
        pos.z = prev.pos.z + (cur.pos.z - prev.pos.z) * f;
        yawDeg = lerpAngleDeg(prev.yawDeg, cur.yawDeg, f);
    }

    std::uintptr_t tessPtr = *(std::uintptr_t*)((std::uintptr_t)screenContext + ScreenContext::mTessellator);
    if (!tessPtr || tessPtr < 0x1000) return;
    void* tess = (void*)tessPtr;

    std::uintptr_t lrpPtr = *(std::uintptr_t*)((std::uintptr_t)levelRenderer + LevelRenderer::mLevelRendererPlayer);
    if (!lrpPtr || lrpPtr < 0x1000) return;

    const float camX = *(float*)(lrpPtr + LevelRendererPlayer::mCamPos);
    const float camY = *(float*)(lrpPtr + LevelRendererPlayer::mCamPos + 4);
    const float camZ = *(float*)(lrpPtr + LevelRendererPlayer::mCamPos + 8);

    const float scale = std::clamp(g_catpet->m_scale, 0.1f, 5.0f);

    // Skip when the camera is inside the cat (a first-person camera swinging
    // over the pet) so the screen never fills with a single face color.
    {
        const float cx = pos.x - camX;
        const float cy = (pos.y + 0.45f * scale) - camY;
        const float cz = pos.z - camZ;
        const float nearRadius = 0.55f * scale;
        if (cx * cx + cy * cy + cz * cz < nearRadius * nearRadius) return;
    }
    // Far cull: the pet is always near its owner; anything further is stale.
    {
        const float dx = pos.x - camX, dy = pos.y - camY, dz = pos.z - camZ;
        if (dx * dx + dy * dy + dz * dz > 128.0f * 128.0f) return;
    }

    ensureMaterials();
    void* overlayMat = (void*)(lrpPtr + LevelRendererPlayer::mSelectionOverlayMaterial);
    void* matInner = s_matSelection ? (void*)&s_matSelection : overlayMat;
    void* matFill = s_matFill ? (void*)&s_matFill : matInner;
    if (!matFill) matFill = overlayMat;

    std::uintptr_t colorHolderPtr = *(std::uintptr_t*)((std::uintptr_t)screenContext + ScreenContext::mColorHolder);
    if (!colorHolderPtr || colorHolderPtr < 0x1000) return;
    float* colorHolder = (float*)colorHolderPtr;
    float savedColor[4] = { colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3] };
    colorHolder[0] = 1.0f; colorHolder[1] = 1.0f; colorHolder[2] = 1.0f; colorHolder[3] = 1.0f;

    // Pose extrapolated by the time since the last tick (smooth at any FPS).
    const catpet::CatPose pose = g_catpet->currentPoseInterpolated();

    int styleIdx = g_catpet->m_catStyleIndex;
    if (styleIdx < 0 || styleIdx >= catpet::kCatStyleCount) styleIdx = 0;
    const catpet::CatStyle& style = catpet::kCatStyles[styleIdx];

    catpet::Vec3 right, forward;
    catpet::catBasis(yawDeg, right, forward);
    const catpet::Vec3 origin{pos.x, pos.y, pos.z};
    const catpet::Vec3 cam{camX, camY, camZ};

    catpet::Affine xforms[catpet::kCatPartCount];
    catpet::partTransforms(pose, xforms);

    char pad[0x58];
    std::memset(pad, 0, sizeof(pad));

    const int vertexCount = catpet::kCatPartCount * wings::kFaceCount * 4 * 2;  // both windings
    s_tessBegin(tess, nullptr, 1, vertexCount, 0);  // 1 = quad

    for (int i = 0; i < catpet::kCatPartCount; ++i) {
        catpet::Vec3 local[wings::kCornerCount];
        catpet::buildPartCorners(i, xforms[i], pose.blink, local);

        // Camera-relative world corners.
        catpet::Vec3 corners[wings::kCornerCount];
        for (int c = 0; c < wings::kCornerCount; ++c) {
            const catpet::Vec3 w = catpet::catPointToWorld(local[c], origin, right, forward, scale);
            corners[c] = {w.x - cam.x, w.y - cam.y, w.z - cam.z};
        }

        int slots[wings::kFaceCount];
        catpet::partFaceSlots(i, slots);

        for (int f = 0; f < wings::kFaceCount; ++f) {
            const int* ring = wings::kFaceRings[f];

            catpet::Vec3 faceCenter{0.0f, 0.0f, 0.0f};
            for (int k = 0; k < 4; ++k) faceCenter = faceCenter + corners[ring[k]];
            faceCenter = faceCenter * 0.25f;

            // Corners are camera relative, so to-camera is the negated center.
            const catpet::Vec3 toCamera = faceCenter * -1.0f;
            const catpet::Vec3 nLocal = catpet::matApply(xforms[i].r, catpet::faceNormalLocal(f));
            const catpet::Vec3 normal = catpet::catDirToWorld(nLocal, right, forward);
            const float brightness =
                wings::faceBrightness(normal, toCamera, wings::kDefaultLight) * catpet::kCatParts[i].tint;
            emitFace(tess, corners, ring, wings::shadeFace(style.rgb[slots[f]], brightness));
        }
    }

    s_renderMesh(screenContext, tess, matFill, pad);

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

static void _renderLevel_hook(void* _this, void* screenContext, void* a3) {
    if (_renderLevel_orig) _renderLevel_orig(_this, screenContext, a3);
    if (!g_catpet || !g_catpet->enabled) return;
    renderCatOverlay(_this, screenContext);
}

}  // namespace

CatPetModule::CatPetModule()
    : Module("Cat Pet",
             "A big chibi voxel cat that follows you around: it trots at your heel, sprints to catch up, "
             "looks at you, twitches its ears, blinks, swishes its tail and sits down when you stand still. "
             "Pick a coat in the Cat Style selector and its size with the Scale slider. Fully client-side.") {
    g_catpet = this;
    showInMenu = true;
    hideInHudEditor = true;  // world overlay, not HUD
}

CatPetModule::~CatPetModule() {
    if (g_catpet == this) g_catpet = nullptr;
}

void CatPetModule::onInit() {
    std::uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    if (addr != 0) m_patchTarget = (void*)addr;

    std::uintptr_t tb = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin);
    if (tb) s_tessBegin = (Tessellator_begin_t)tb;

    std::uintptr_t tc = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor);
    if (tc) s_tessColor = (Tessellator_color_t)tc;

    std::uintptr_t tv = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex);
    if (tv) s_tessVertex = (Tessellator_vertex_t)tv;

    std::uintptr_t rm = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if (rm) {
        s_renderMesh = (MeshHelpers_renderMeshImmediately_t)rm;
    } else {
        std::uintptr_t rm5 = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);
        if (rm5) s_renderMesh = (MeshHelpers_renderMeshImmediately_t)rm5;
    }

    std::uintptr_t rmg = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
    if (rmg) {
        std::uintptr_t groupAddr = resolveADRP(reinterpret_cast<std::uint32_t*>(rmg), 2, 0);
        if (groupAddr) s_renderMaterialGroup = groupAddr + MaterialGroup::mRenderMaterialGroupOffset;
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_catpet) g_catpet->onLocalPlayerTick(event.player);
        });
}

void CatPetModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    auto handle = bedrocktools::hooks::install(m_patchTarget, (void*)_renderLevel_hook, (void**)&_renderLevel_orig);
    m_patched = handle != nullptr;
}

void CatPetModule::onEnable() {
    applyPatch();
    {
        std::lock_guard<std::mutex> animationLock(m_animationMutex);
        m_animTime = 0.0f;
        m_stridePhase = 0.0f;
        m_strideRate = 0.0f;
        m_moveBlend = 0.0f;
        m_sitBlend = 0.0f;
        m_idleTime = 0.0f;
        m_clockStarted = false;
    }
    std::lock_guard<std::mutex> lock(s_stateMutex);
    s_hasCat = false;
    s_hasPrevSample = false;
    s_lastTickTimeValid = false;
    s_follow = catpet::FollowState{};
}

void CatPetModule::onDisable() {
    std::lock_guard<std::mutex> lock(s_stateMutex);
    s_hasCat = false;
    s_hasPrevSample = false;
    s_lastTickTimeValid = false;
    s_follow = catpet::FollowState{};
}

// ---------------------------------------------------------------------------
// Animation clocks
// ---------------------------------------------------------------------------

void CatPetModule::advanceCatAnimation(float dtSeconds, float chaseSpeed) {
    if (dtSeconds <= 0.0f) return;
    std::lock_guard<std::mutex> animationLock(m_animationMutex);

    if (chaseSpeed < 0.0f) chaseSpeed = 0.0f;
    m_animTime += dtSeconds;

    const float rate = catpet::strideRateForSpeed(chaseSpeed);
    m_strideRate = rate;
    m_stridePhase += dtSeconds * rate;
    // Keep the phase bounded so float precision never degrades the gait.
    constexpr float kTwoPi = 2.0f * catpet::kPi;
    if (m_stridePhase > 256.0f * kTwoPi) m_stridePhase -= 256.0f * kTwoPi;

    const float moveTarget = std::clamp(chaseSpeed / kMoveBlendFullSpeed, 0.0f, 1.0f);
    const float moveRate = (moveTarget > m_moveBlend) ? kMoveAttackRate : kMoveDecayRate;
    m_moveBlend += (moveTarget - m_moveBlend) * std::min(1.0f, dtSeconds * moveRate);

    m_idleTime = (chaseSpeed < kIdleSpeedThreshold) ? m_idleTime + dtSeconds : 0.0f;
    const float sitTarget = (m_sitWhenIdle && m_idleTime > kSitDelaySeconds) ? 1.0f : 0.0f;
    const float sitRate = (sitTarget > m_sitBlend) ? kSitAttackRate : kSitDecayRate;
    m_sitBlend += (sitTarget - m_sitBlend) * std::min(1.0f, dtSeconds * sitRate);
}

bedrocktools::modules::catpet::CatPose CatPetModule::currentPose() const {
    std::lock_guard<std::mutex> animationLock(m_animationMutex);
    return catpet::computeCatPose(m_animTime, m_stridePhase, m_moveBlend, m_sitBlend);
}

bedrocktools::modules::catpet::CatPose CatPetModule::currentPoseInterpolated() const {
    std::lock_guard<std::mutex> animationLock(m_animationMutex);
    float extra = 0.0f;
    if (m_clockStarted) {
        auto now = std::chrono::steady_clock::now();
        extra = std::chrono::duration<float>(now - m_lastTick).count();
        if (extra < 0.0f) extra = 0.0f;
        if (extra > 0.1f) extra = 0.1f;  // clamp after hitches
    }
    return catpet::computeCatPose(m_animTime + extra, m_stridePhase + extra * m_strideRate,
                                  m_moveBlend, m_sitBlend);
}

// ---------------------------------------------------------------------------
// Tick: follow the owner, publish samples for the render thread
// ---------------------------------------------------------------------------

void CatPetModule::onLocalPlayerTick(void* player) {
    if (!player || !enabled) {
        std::lock_guard<std::mutex> lock(s_stateMutex);
        s_hasCat = false;
        s_hasPrevSample = false;
        s_lastTickTimeValid = false;
        s_follow.hasPos = false;
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    {
        std::lock_guard<std::mutex> animationLock(m_animationMutex);
        if (m_clockStarted) {
            dt = std::chrono::duration<float>(now - m_lastTick).count();
            if (dt > 0.25f) dt = 0.25f;
        }
        m_lastTick = now;
        m_clockStarted = true;
    }

    const AABB aabb = getActorAABB(player);
    const bedrocktools::sdk::Vec2 rot = getActorRotation(player);

    const bool aabbValid =
        std::isfinite(aabb.min.x) && std::isfinite(aabb.min.y) && std::isfinite(aabb.min.z) &&
        std::isfinite(aabb.max.x) && std::isfinite(aabb.max.y) && std::isfinite(aabb.max.z) &&
        aabb.max.x > aabb.min.x && aabb.max.y > aabb.min.y && aabb.max.z > aabb.min.z &&
        std::isfinite(rot.y);
    if (!aabbValid || dt <= 0.0f) return;

    const catpet::Vec3 ownerFeet{
        (aabb.min.x + aabb.max.x) * 0.5f,
        aabb.min.y,
        (aabb.min.z + aabb.max.z) * 0.5f,
    };

    // The game's yaw (rot.y) maps to the wings-style forward (-sin, cos); the
    // cat basis uses forward (sin, cos), so the owner's yaw is simply negated.
    const float ownerYawDeg = -rot.y;

    const float scale = std::clamp(m_scale, 0.1f, 5.0f);
    const catpet::Vec3 target = catpet::heelTarget(ownerFeet, ownerYawDeg, scale);
    const catpet::FollowResult res = catpet::stepCatFollow(s_follow, target, ownerFeet, ownerYawDeg, dt);

    advanceCatAnimation(dt, s_follow.smoothedSpeed);

    {
        std::lock_guard<std::mutex> lock(s_stateMutex);
        const CatSample sample{{s_follow.x, s_follow.y, s_follow.z}, s_follow.yawDeg};
        if (!s_hasPrevSample || res.teleported) {
            s_prevSample = sample;
            s_hasPrevSample = true;
        } else {
            s_prevSample = s_curSample;
        }
        s_curSample = sample;

        if (s_lastTickTimeValid) {
            const float meas = std::chrono::duration<float>(now - s_lastTickTime).count();
            if (meas > 0.001f && meas < 0.5f) s_tickInterval = meas;
        }
        s_lastTickTime = now;
        s_lastTickTimeValid = true;
        s_hasCat = true;
    }
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

void CatPetModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_scale")) {
        m_scale = std::clamp(j["m_scale"].get<float>(), 0.1f, 5.0f);
    } else if (j.contains("scale")) {
        m_scale = std::clamp(j["scale"].get<float>(), 0.1f, 5.0f);
    }
    if (j.contains("m_catStyle")) {
        m_catStyleIndex = catpet::resolveCatStyleIndex(j["m_catStyle"].get<std::string>());
        m_catStyle = catpet::kCatStyles[m_catStyleIndex].id;
    } else if (j.contains("catStyle")) {
        m_catStyleIndex = catpet::resolveCatStyleIndex(j["catStyle"].get<std::string>());
        m_catStyle = catpet::kCatStyles[m_catStyleIndex].id;
    }
    if (j.contains("m_sitWhenIdle")) m_sitWhenIdle = j["m_sitWhenIdle"].get<bool>();
}

void CatPetModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_scale"] = std::clamp(m_scale, 0.1f, 5.0f);
    m_catStyleIndex = catpet::catStyleIndexForId(m_catStyle);
    j["m_catStyle"] = catpet::catStyleRadioValue(m_catStyleIndex);
    j["m_sitWhenIdle"] = m_sitWhenIdle;
}

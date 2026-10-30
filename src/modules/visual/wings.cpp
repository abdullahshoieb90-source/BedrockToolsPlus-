#include <bedrocktools/modules/visual/wings.hpp>
#include <bedrocktools/modules/visual/wings_default.hpp>

#include "modules/ModuleRegistry.hpp"
#include "modules/visual/wings_shape.hpp"
#include "modules/visual/wings_design.hpp"
#include "modules/visual/wings_surface.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"
#include "core/RenderState.hpp"
#include "../../config/ConfigManager.hpp"

#include <stb/stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <mutex>

// Wing shape, styles and shading (pure, host-testable - see wings_shape.hpp).
namespace wings = bedrocktools::modules::wings;

namespace {

using namespace bedrocktools::sdk::offsets;

// ---------------------------------------------------------------------------
// RenderLevel hook plumbing (same pattern as Hitbox/Breadcrumbs)
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

WingsModule* g_wings = nullptr;

static Tessellator_begin_t s_tessBegin = nullptr;
static Tessellator_color_t s_tessColor = nullptr;
static Tessellator_vertex_t s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;

static MaterialPtr s_matSelection;
static MaterialPtr s_matFill;
static std::uintptr_t s_renderMaterialGroup = 0;

static void (*_renderLevel_orig)(void* _this, void* screenContext, void* a3) = nullptr;

// Player tracking (written in tick, read in render)
struct AABB {
    bedrocktools::sdk::Vec3 min{0,0,0};
    bedrocktools::sdk::Vec3 max{0,0,0};
};

static std::mutex s_stateMutex;
static AABB s_playerAABB{};
static bedrocktools::sdk::Vec2 s_playerRot{0,0};
static WingBoneAngles s_boneAngles{};
static void* s_localPlayerPtr = nullptr;
static bool s_hasPlayer = false;

// --- Render-thread anchor interpolation (mirrors entity client-side lerp) ---
// The collision AABB and the rotation are only refreshed once per server tick
// (20 Hz), but the player model is rendered with client-side interpolation, so
// anchoring the wings to the raw tick samples makes them stutter against the
// smoothly moving body while walking.  We keep the previous and current tick
// samples and lerp between them by the partial-tick fraction, exactly like the
// game interpolates the player mesh.  That is what removes the walking jitter.
static AABB s_prevAABB{};
static AABB s_curAABB{};
static bedrocktools::sdk::Vec2 s_prevRot{0, 0};
static bedrocktools::sdk::Vec2 s_curRot{0, 0};
static float s_tickInterval = 0.05f;  // seconds between ticks (measured)
static bool s_hasPrevSample = false;

static std::chrono::steady_clock::time_point s_lastTickTime{};
static bool s_lastTickTimeValid = false;

// Shortest-arc interpolation for angles in degrees, so yaw/pitch never wrap
// the 360 degree boundary mid-lerp (which would snap the wings the wrong way).
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

// ActorRotationComponent is the body transform used by the player model.
// Do not use the camera/view rotation here: the view can orbit independently
// of the model (third person), while the wings must remain children of the
// chest/body transform.
static bedrocktools::sdk::Vec2 getActorBodyRotation(void* actor) {
    bedrocktools::sdk::Vec2 bodyRotation{0, 0};
    std::uintptr_t actorAddr = (std::uintptr_t)actor;
    if (actorAddr < 0x1000) return bodyRotation;
    std::uintptr_t rotComp = *(std::uintptr_t*)(actorAddr + Actor::mActorRotationComponent);
    if (rotComp < 0x1000) return bodyRotation;
    bodyRotation = *(bedrocktools::sdk::Vec2*)rotComp;
    return bodyRotation;
}

static float getActorBodyYaw(void* actor) {
    return getActorBodyRotation(actor).y;
}

static MaterialPtr getMaterial(const char* name) {
    if (!s_renderMaterialGroup) return {};
    HashedString hs(name);
    void** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);
    if (!vtable || !vtable[2]) return {};
    using getMat_t = MaterialPtr(*)(void*, const HashedString*);
    return reinterpret_cast<getMat_t>(vtable[2])((void*)s_renderMaterialGroup, &hs);
}

// Material for the membrane quads.
//
// Depth test is the requirement here, not looks: the wings are mounted on the
// player's back, so from the front (or from any tight orbit) the player's body
// has to occlude them. ui_fill_color is deliberately NOT preferred, even though
// it gives the cleanest vertex-colour fill: on current Bedrock builds it has no
// terrain/depth test (see the Through Walls note in blockoutline.cpp), so the
// whole wing gets painted straight over the torso and reads as a translucent
// white sheet. selection_box is what the game itself uses for the block
// highlight - it is depth tested, takes the tessellator colour and alpha.
static void ensureMaterials() {
    if (!s_renderMaterialGroup) return;
    if (!s_matSelection) s_matSelection = getMaterial("selection_box");
    if (!s_matFill) {
        static const char* kFillNames[] = { "selection_box", "debug_filled_box", "ui_fill_color", "ui_textured_and_glcolor" };
        for (const char* n : kFillNames) {
            s_matFill = getMaterial(n);
            if (s_matFill) break;
        }
    }
}

static std::string wingsDirectoryForConfig() {
    const std::string configPath = bedrocktools::config::ConfigManager::get().getConfigPath();
    const std::size_t lastSlash = configPath.find_last_of('/');
    std::string dir = (lastSlash != std::string::npos) ? configPath.substr(0, lastSlash) : "/sdcard/games/BedrockToolsPlus";
    return dir + "/wings";
}

// ---------------------------------------------------------------------------
// Wings as one continuous membrane per side.
//
// The style table (modules/visual/wings_design.hpp) and the surface builder
// (modules/visual/wings_surface.hpp) are pure: they turn a pose into a list of
// double-sided quads in Bedrock model pixels, and the host tests plus
// tools/wings_preview.cpp evaluate exactly the same code. What is left here is
// the tessellator plumbing - map the quads into camera-relative world space,
// shade each side and submit both windings.
//
// This replaced the old per-bone prism renderer. Seven disjoint boxes per wing
// read as a comb of planks: gaps between the "feathers", flat faces and no
// silhouette. The membrane gives a real leading edge, scalloped fingers, ribs,
// a claw and layered feathers, and it is cheaper to draw (one tessellator call
// per wing either way, but far fewer overdraw slivers).

static wings::WingQuad s_rightQuads[wings::kMaxWingQuads];
static wings::WingQuad s_leftQuads[wings::kMaxWingQuads];

// Maps the animated bone angles onto the surface builder's pose. The mapping
// itself lives in wings_surface.hpp so the host tests cover it.
static wings::WingPose poseFromAngles(const WingBoneAngles& a) {
    return wings::makeWingPose(a.shoulderDeg, a.upperDeg, a.tipDeg, a.flapPhase,
                               a.intensity, a.glide);
}

// Player body box in world space, used to hide the wings behind the body.
struct BodyBox {
    wings::Vec3 min;
    wings::Vec3 max;
    wings::ShadowCone cone;
    wings::Vec3 apex;
    bool valid = false;
    // The shadow cone is only used when the camera is in front of the player.
    // From behind, the wings sit between the camera and the body, so nothing is
    // ever hidden there and clipping would be wrong.
    bool clip = false;
};

// Receives the visible wing geometry. With `tess` null it only accumulates the
// exact vertex count, so the same pass can size the tessellator batch up front.
struct WingSink {
    void* tess = nullptr;
    wings::Vec3 cam;
    int vertices = 0;
};

// Vertices a convex polygon costs. A cut-free quad stays a quad; anything the
// body has cut is fan-triangulated, with each triangle submitted as a
// four-vertex quad whose last corner repeats the previous one. That keeps the
// tessellator in quad mode however many cut edges there are.
static int polygonVertexCost(int count) {
    if (count < 3) return 0;
    if (count == 4) return 4 * 2;
    return (count - 2) * 4 * 2;
}

// Both windings are submitted for every primitive, so back-face culling can
// never eat the membrane when the wing folds over itself.
static void submitTriangle(void* tess, const wings::ClipVertex* a, const wings::ClipVertex* b,
                           const wings::ClipVertex* c, const wings::ClipVertex* cAgain,
                           const wings::Vec3& cam) {
    const wings::ClipVertex* quad[4] = {a, b, c, cAgain};
    for (int k = 0; k < 4; ++k) {
        const wings::ClipVertex& v = *quad[k];
        s_tessColor(tess, v.c.r / 255.0f, v.c.g / 255.0f, v.c.b / 255.0f, 1.0f);
        s_tessVertex(tess, v.p.x - cam.x, v.p.y - cam.y, v.p.z - cam.z);
    }
    for (int k = 3; k >= 0; --k) {
        const wings::ClipVertex& v = *quad[k];
        s_tessColor(tess, v.c.r / 255.0f, v.c.g / 255.0f, v.c.b / 255.0f, 1.0f);
        s_tessVertex(tess, v.p.x - cam.x, v.p.y - cam.y, v.p.z - cam.z);
    }
}

static void writePolygon(WingSink& sink, const wings::ClipVertex* v, int count) {
    if (count < 3) return;
    if (!sink.tess) {
        sink.vertices += polygonVertexCost(count);
        return;
    }
    if (count == 4) {
        // Untouched quad: one quad, no fan needed.
        submitTriangle(sink.tess, &v[0], &v[1], &v[2], &v[3], sink.cam);
        return;
    }
    // Convex fan from v[0]; a cut polygon is still convex, so this is always valid.
    for (int t = 1; t + 1 < count; ++t) {
        submitTriangle(sink.tess, &v[0], &v[t], &v[t + 1], &v[t + 1], sink.cam);
    }
}

// Shades the wing and cuts out whatever the player's body hides from the camera.
// The quad is clipped rather than culled whole: a membrane cell is ~2 model
// pixels across but tens of screen pixels at third-person distance, so dropping
// whole quads would leave a staircase instead of the body's silhouette.
static void resolveWing(const wings::WingQuad* quads, int count, const BodyBox& body,
                        const wings::Vec3& feet, const wings::Vec3& right,
                        const wings::Vec3& forward, const wings::Vec3& cam, bool seeBack,
                        WingSink& sink) {
    for (int i = 0; i < count; ++i) {
        const wings::WingQuad& q = quads[i];

        wings::ClipVertex corner[4];
        wings::Vec3 center{0.0f, 0.0f, 0.0f};
        for (int k = 0; k < 4; ++k) {
            const wings::Vec3 p =
                wings::modelPointToWorld(q.p[k].x, q.p[k].y, q.p[k].z, feet, right, forward);
            corner[k].p = p;
            center = center + p;
        }
        center = center * 0.25f;

        // The headlight vector runs from the surface towards the camera.
        // `center` is a WORLD point, so it must be subtracted from the camera
        // position - negating it would treat a world point as camera-relative
        // and point the light at the world origin.
        const wings::Vec3 toCamera = wings::toCameraVector(cam, center);
        const wings::Vec3 normal = wings::modelDirToWorld(q.n, right, forward);
        const wings::Vec3 litNormal = seeBack ? normal : normal * -1.0f;

        float brightness = wings::faceBrightness(litNormal, toCamera, wings::kDefaultLight) *
                           wings::spanTint(q.spanT);
        if (q.emissive > 0.0f && brightness < q.emissive) brightness = q.emissive;

        // Per-corner colours: the tessellator interpolates them across the
        // polygon (Gouraud), so the sail shows its leading-edge highlight,
        // trailing shadow and shoulder-to-tip gradient instead of one flat tone.
        const wings::FaceColor* cornerColors = seeBack ? q.backC : q.frontC;
        for (int k = 0; k < 4; ++k) corner[k].c = wings::shadeFace(cornerColors[k], brightness);

        if (!body.clip) {
            writePolygon(sink, corner, 4);
            continue;
        }

        wings::ClipPolygon pieces[wings::kMaxShadowPlanes];
        const int pieceCount = wings::clipPolygonOutsideCone(corner, 4, body.cone, body.apex,
                                                             pieces, wings::kMaxShadowPlanes);
        for (int p = 0; p < pieceCount; ++p) writePolygon(sink, pieces[p].v, pieces[p].count);
    }
}

// Camera-inside-AABB tolerance for WingsModule::isThirdPersonCamera. In
// first-person the camera is inside the player's head (inside the collision
// box); the small margin absorbs jitter at the box edges. A third-person
// camera is pulled back well outside the box.
constexpr float kFirstPersonMargin = 0.05f;  // blocks

// ---------------------------------------------------------------------------
// Wing rendering
// ---------------------------------------------------------------------------

static void renderWingsOverlay(void* levelRenderer, void* screenContext) {
    if (!screenContext || (std::uintptr_t)screenContext < 0x1000) return;
    if (!levelRenderer || (std::uintptr_t)levelRenderer < 0x1000) return;
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) return;

    AABB aabb{};
    bedrocktools::sdk::Vec2 rot{};
    WingBoneAngles angles{};
    void* playerPtr = nullptr;
    AABB prevAABB{};
    bedrocktools::sdk::Vec2 prevRot{0, 0};
    float tickInterval = 0.05f;
    bool hasPrevSample = false;
    bool hasPlayer = false;
    bool lastTickValid = false;
    std::chrono::steady_clock::time_point lastTick{};

    {
        std::lock_guard<std::mutex> lock(s_stateMutex);
        if (!s_hasPlayer) return;
        hasPlayer = s_hasPlayer;
        aabb = s_curAABB;
        rot = s_curRot;
        angles = s_boneAngles;
        playerPtr = s_localPlayerPtr;
        prevAABB = s_prevAABB;
        prevRot = s_prevRot;
        tickInterval = s_tickInterval;
        hasPrevSample = s_hasPrevSample;
        lastTick = s_lastTickTime;
        lastTickValid = s_lastTickTimeValid;
    }

    if (!hasPlayer) return;

    // --- Low-latency anchor: take the freshest AABB/rotation directly from the
    // actor (the same authoritative component the tick uses), then interpolate
    // it against the previous tick's sample by the partial-tick fraction.
    // Reading the live AABB alone leaves the wings snapping at the 20 Hz tick
    // rate while the body glides at the render rate - that mismatch is exactly
    // the walking jitter.  Interpolating, like the game's own client-side lerp,
    // makes the wings ride the smoothly rendered body instead of stuttering.
    bool liveValid = false;
    if (playerPtr && (std::uintptr_t)playerPtr >= 0x1000) {
        AABB liveAABB = getActorAABB(playerPtr);
        const float width = liveAABB.max.x - liveAABB.min.x;
        const float height = liveAABB.max.y - liveAABB.min.y;
        const float depth = liveAABB.max.z - liveAABB.min.z;
        const bool finite =
            std::isfinite(liveAABB.min.x) && std::isfinite(liveAABB.min.y) && std::isfinite(liveAABB.min.z) &&
            std::isfinite(liveAABB.max.x) && std::isfinite(liveAABB.max.y) && std::isfinite(liveAABB.max.z);
        liveValid = finite && width > 0.0f && width < 16.0f &&
                    height > 0.0f && height < 16.0f &&
                    depth > 0.0f && depth < 16.0f;
        if (liveValid) {
            aabb = liveAABB;             // freshest "current" sample
            bedrocktools::sdk::Vec2 liveRot = getActorBodyRotation(playerPtr);
            if (std::isfinite(liveRot.x) && std::isfinite(liveRot.y)) {
                rot = liveRot;
            }
        }
    }

    // The current sample (live if available, otherwise the last tick's) must be
    // finite before we can anchor the wings to it.
    const bool curValid =
        std::isfinite(aabb.min.x) && std::isfinite(aabb.min.y) && std::isfinite(aabb.min.z) &&
        std::isfinite(aabb.max.x) && std::isfinite(aabb.max.y) && std::isfinite(aabb.max.z) &&
        aabb.max.x > aabb.min.x && aabb.max.y > aabb.min.y && aabb.max.z > aabb.min.z &&
        std::isfinite(rot.x) && std::isfinite(rot.y);
    if (!curValid) return;

    // Interpolate the anchor and the facing between the previous and current
    // tick samples by the elapsed fraction of the current tick.  This is the
    // exact counterpart of the entity client-side lerp, so the wings track the
    // smoothly rendered body at any frame rate.  We only interpolate within a
    // single tick (never extrapolate past the next one), so a hitch on the tick
    // thread just freezes the wings for a frame instead of flinging them.
    if (hasPrevSample && lastTickValid && tickInterval > 0.0f) {
        auto now = std::chrono::steady_clock::now();
        float f = std::chrono::duration<float>(now - lastTick).count() / tickInterval;
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        aabb.min.x = prevAABB.min.x + (aabb.min.x - prevAABB.min.x) * f;
        aabb.min.y = prevAABB.min.y + (aabb.min.y - prevAABB.min.y) * f;
        aabb.min.z = prevAABB.min.z + (aabb.min.z - prevAABB.min.z) * f;
        aabb.max.x = prevAABB.max.x + (aabb.max.x - prevAABB.max.x) * f;
        aabb.max.y = prevAABB.max.y + (aabb.max.y - prevAABB.max.y) * f;
        aabb.max.z = prevAABB.max.z + (aabb.max.z - prevAABB.max.z) * f;
        rot.x = lerpAngleDeg(prevRot.x, rot.x, f);
        rot.y = lerpAngleDeg(prevRot.y, rot.y, f);
    }

    // --- Interpolated bone angles for smooth flap between ticks ---
    if (g_wings) {
        // Use interpolated angles if available (adds extra flap time)
        angles = g_wings->currentBoneAnglesInterpolated();
    }

    std::uintptr_t tessPtr = *(std::uintptr_t*)((std::uintptr_t)screenContext + ScreenContext::mTessellator);
    if (!tessPtr || tessPtr < 0x1000) return;
    void* tess = (void*)tessPtr;

    std::uintptr_t lrpPtr = *(std::uintptr_t*)((std::uintptr_t)levelRenderer + LevelRenderer::mLevelRendererPlayer);
    if (!lrpPtr || lrpPtr < 0x1000) return;

    float camX = *(float*)(lrpPtr + LevelRendererPlayer::mCamPos);
    float camY = *(float*)(lrpPtr + LevelRendererPlayer::mCamPos + 4);
    float camZ = *(float*)(lrpPtr + LevelRendererPlayer::mCamPos + 8);

    // Feet center (AABB bottom). Computed here (before the color holder is
    // touched) so the first-person guard below can early-return without leaving
    // the GPU color stuck at white, and later reused to place the wings.
    float feetX = (aabb.min.x + aabb.max.x) * 0.5f;
    float feetY = aabb.min.y;
    float feetZ = (aabb.min.z + aabb.max.z) * 0.5f;

    // First-person: the camera sits inside the player's head (inside the AABB),
    // so the back-mounted wings overlap/clip the view. Only draw them from a
    // real third-person point of view, matching how the Hitbox module hides its
    // own box in first-person.
    if (!WingsModule::isThirdPersonCamera(camX, camY, camZ,
                                          aabb.min.x, aabb.min.y, aabb.min.z,
                                          aabb.max.x, aabb.max.y, aabb.max.z)) return;

    ensureMaterials();
    void* overlayMat = (void*)(lrpPtr + LevelRendererPlayer::mSelectionOverlayMaterial);
    void* matInner = s_matSelection ? (void*)&s_matSelection : overlayMat;
    void* matFill = s_matFill ? (void*)&s_matFill : matInner;
    if (!matFill) matFill = overlayMat;

    // Resolve the selected wing style (index is kept in sync with m_wingStyle
    // on the tick/config thread; reading an int here is safe for the render
    // thread).
    int styleIdx = 0;
    if (g_wings) styleIdx = g_wings->m_wingStyleIndex;
    if (styleIdx < 0 || styleIdx >= wings::kWingStyleCount) styleIdx = 0;
    const wings::WingDesign& design = wings::kWingDesigns[styleIdx];

    // Build both sides first so the exact vertex count can be handed to the
    // tessellator (Bedrock needs it up front). This runs before the GPU colour
    // holder is touched, so the early return below cannot leave it at white.
    const wings::WingPose pose = poseFromAngles(angles);
    const int rightCount = wings::buildWing(design, pose, -1, s_rightQuads, wings::kMaxWingQuads);
    const int leftCount = wings::buildWing(design, pose, 1, s_leftQuads, wings::kMaxWingQuads);
    if (rightCount <= 0 && leftCount <= 0) return;

    bedrocktools::core::ScopedShaderColor shaderColor(screenContext);
    if (!shaderColor) return;

    // Player yaw -> right/forward vectors
    constexpr float kPi = 3.14159265358979323846f;
    // This is deliberately the actor's body yaw, never the camera yRot/yaw.
    // The AABB center is the model origin and the root pivot (0, 24, 0) is
    // the chest/back pivot, so both translation and rotation share one body
    // transform.
    const float bodyYaw = rot.y;
    float yawRad = bodyYaw * kPi / 180.0f;
    float cosYaw = std::cos(yawRad);
    float sinYaw = std::sin(yawRad);

    // right = (-cosYaw, -sinYaw) in XZ, forward = (-sinYaw, cosYaw)
    // See HitboxModule comments for derivation.
    float rightX = -cosYaw;
    float rightZ = -sinYaw;
    float fwdX = -sinYaw;
    float fwdZ = cosYaw;

    char pad[0x58];
    std::memset(pad, 0, sizeof(pad));

    const wings::Vec3 feet{feetX, feetY, feetZ};
    const wings::Vec3 rightVec{rightX, 0.0f, rightZ};
    const wings::Vec3 forwardVec{fwdX, 0.0f, fwdZ};
    const wings::Vec3 cam{camX, camY, camZ};
    const wings::Vec3 bodyCenter{feetX, feetY + 1.25f, feetZ};

    // Body occlusion: the wings sit on the back, so from the front the player's
    // own collision box has to hide them. This is done here instead of trusting
    // the material to depth test (ui_fill_color does not on current builds),
    // which is what used to paint the wings straight over the torso.
    // The occluder is the *rendered* body, not the full collision box: a
    // player's AABB is 0.6 blocks deep in every direction, while the model
    // torso is only 0.25 blocks deep, so the collision box would swallow the
    // wing mounts and leave the roots painted over the chest.
    BodyBox body;
    wings::bodyOccluderFromAabb({aabb.min.x, aabb.min.y, aabb.min.z},
                                {aabb.max.x, aabb.max.y, aabb.max.z},
                                body.min, body.max);
    body.valid = aabb.max.x > aabb.min.x && aabb.max.y > aabb.min.y && aabb.max.z > aabb.min.z;

    // Cut the body out of the wings when the camera is on the front side. From
    // behind (and at side-on) the wings are between the camera and the body, so
    // nothing is hidden and they are submitted untouched.
    if (body.valid && wings::cameraBackness(cam, bodyCenter, forwardVec) < 0.0f) {
        body.clip = true;
        body.apex = cam;
        body.cone = wings::buildShadowCone(cam, body.min, body.max);
    }

    const bool seeBack = wings::cameraSeesBackSide(cam, bodyCenter, rightVec, forwardVec);

    // Pass 1: count the exact vertices the visible geometry needs. Bedrock's
    // tessellator requires the count up front, and after clipping the number of
    // vertices is no longer a function of the quad count.
    WingSink counter;
    counter.cam = cam;
    resolveWing(s_rightQuads, rightCount, body, feet, rightVec, forwardVec, cam, seeBack, counter);
    resolveWing(s_leftQuads, leftCount, body, feet, rightVec, forwardVec, cam, seeBack, counter);
    if (counter.vertices <= 0) {
        // Everything is behind the body: nothing to draw. The colour holder was
        // not modified yet, so returning here cannot leave the screen white.
        return;
    }

    s_tessBegin(tess, nullptr, 1, counter.vertices, 0);  // 1 = quad

    // Pass 2: the same computation, this time submitting. Both passes are pure
    // functions of the same inputs, so the counts always match.
    WingSink emitter;
    emitter.tess = tess;
    emitter.cam = cam;
    resolveWing(s_rightQuads, rightCount, body, feet, rightVec, forwardVec, cam, seeBack, emitter);
    resolveWing(s_leftQuads, leftCount, body, feet, rightVec, forwardVec, cam, seeBack, emitter);

    s_renderMesh(screenContext, tess, matFill, pad);
}

static void _renderLevel_hook(void* _this, void* screenContext, void* a3) {
    if (_renderLevel_orig) _renderLevel_orig(_this, screenContext, a3);
    if (!g_wings || !g_wings->enabled) return;
    if (!s_localPlayerPtr) return;
    renderWingsOverlay(_this, screenContext);
}

} // namespace

WingsModule::WingsModule()
    : Module("Wings", "Renders animated 3D wings on your back, driven by your movement. Pick a shape in the Wing Style selector: Dragon, Angel, Demon, Bat, Butterfly, Phoenix, Fairy, Demon Wings, Vampire Bat or Red Bat. Does not modify your skin.") {
    g_wings = this;
    showInMenu = true;
    hideInHudEditor = true; // world overlay, not HUD
}

WingsModule::~WingsModule() {
    if (g_wings == this) g_wings = nullptr;
}

void WingsModule::onInit() {
    m_wingsDir = wingsDirectoryForConfig();
    ensureWingsAssetFiles();

    std::uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    if (addr != 0) m_patchTarget = (void*)addr;

    std::uintptr_t tb = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin);
    if (tb) { m_tessBeginAddr = (void*)tb; s_tessBegin = (Tessellator_begin_t)tb; }

    std::uintptr_t tc = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor);
    if (tc) { m_tessColorAddr = (void*)tc; s_tessColor = (Tessellator_color_t)tc; }

    std::uintptr_t tv = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex);
    if (tv) { m_tessVertexAddr = (void*)tv; s_tessVertex = (Tessellator_vertex_t)tv; }

    std::uintptr_t rm = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if (rm) {
        m_renderMesh2Addr = (void*)rm;
        s_renderMesh = (MeshHelpers_renderMeshImmediately_t)rm;
    } else {
        std::uintptr_t rm5 = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);
        if (rm5) {
            m_renderMeshAddr = (void*)rm5;
            s_renderMesh = (MeshHelpers_renderMeshImmediately_t)rm5;
        }
    }

    std::uintptr_t rmg = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
    if (rmg) {
        m_renderMaterialGroupAddr = (void*)rmg;
        std::uintptr_t groupAddr = resolveADRP(reinterpret_cast<std::uint32_t*>(rmg), 2, 0);
        if (groupAddr) s_renderMaterialGroup = groupAddr + MaterialGroup::mRenderMaterialGroupOffset;
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_wings) g_wings->onLocalPlayerTick(event.player);
        });
}

void WingsModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    auto handle = bedrocktools::hooks::install(m_patchTarget, (void*)_renderLevel_hook, (void**)&_renderLevel_orig);
    m_patched = handle != nullptr;
}

void WingsModule::onEnable() {
    applyPatch();
    {
        std::lock_guard<std::mutex> animationLock(m_animationMutex);
        m_flapTime = 0.0f;
        m_intensity = 0.0f;
        m_glide = 0.0f;
        m_airTime = 0.0f;
        m_flapClockStarted = false;
    }
    m_hasPrevCenter = false;
    {
        std::lock_guard<std::mutex> lock(s_stateMutex);
        s_boneAngles = WingBoneAngles{};
        s_hasPrevSample = false;
        s_prevAABB = {};
        s_curAABB = {};
        s_prevRot = {0, 0};
        s_curRot = {0, 0};
        s_tickInterval = 0.05f;
        s_lastTickTimeValid = false;
    }
}

void WingsModule::onDisable() {
    // No skin to restore; just clear tracked player so overlay disappears immediately
    std::lock_guard<std::mutex> lock(s_stateMutex);
    s_hasPlayer = false;
    s_localPlayerPtr = nullptr;
    s_hasPrevSample = false;
    s_lastTickTimeValid = false;
}

// ---------------------------------------------------------------------------
// Idle / flap / glide animation driven by the player's speed
// ---------------------------------------------------------------------------

static float lerpFloat(float a, float b, float t) {
    return a + (b - a) * t;
}

float WingsModule::currentFlapAngleDegrees() const {
    return kFlapAmplitudeDegrees * std::sin(m_flapTime * kFlapBaseRate * m_flapSpeed);
}

float WingsModule::currentFlapAngleRadians() const {
    return currentFlapAngleDegrees() * wings::kDegToRad;
}

// Shared idle/flap/glide pose solver. Both currentBoneAngles() (the pose
// published on the tick) and currentBoneAnglesInterpolated() (the pose
// extrapolated on the render thread) go through here, so the two can no
// longer drift apart - they used to be two hand-copied blocks.
static WingBoneAngles computeWingPose(float flapTime, float flapSpeed, float intensity, float glide) {
    WingBoneAngles out;
    const float phase = flapTime * WingsModule::kFlapBaseRate * flapSpeed;
    const float idlePh = flapTime * WingsModule::kIdleRate * flapSpeed;
    const float glidePh = flapTime * WingsModule::kGlideRate * flapSpeed;
    const float w = intensity;
    const float g = glide;

    // idle <-> flap blended pose (a wave travelling from the shoulder out to
    // the feathers, matching animation.wings.idle / animation.wings.flap).
    const float shoulderFlap = lerpFloat(WingsModule::kIdleBaseDegrees, WingsModule::kFlightBaseDegrees, w)
        + lerpFloat(WingsModule::kIdleAmplitudeDegrees * std::sin(idlePh),
                    WingsModule::kFlapAmplitudeDegrees * std::sin(phase), w);
    const float upperFlap = lerpFloat(6.0f, 0.0f, w)
        + lerpFloat(4.0f * std::sin(idlePh - WingsModule::kIdleUpperLag),
                    WingsModule::kFlightUpperAmplitudeDegrees * std::sin(phase - WingsModule::kUpperLag), w);
    const float tipFlap = lerpFloat(8.0f, 0.0f, w)
        + lerpFloat(3.0f * std::sin(idlePh - WingsModule::kIdleTipLag),
                    WingsModule::kFlightTipAmplitudeDegrees * std::sin(phase - WingsModule::kTipLag), w);

    // glide pose (animation.wings.glide): wings spread high, segments
    // straightened outward. Signs match the "raise positive" value the JSON
    // animations produce for the right-side bones.
    const float shoulderGlide = WingsModule::kGlideBaseDegrees + 3.0f * std::sin(glidePh);
    const float upperGlide = -15.0f + 3.0f * std::sin(glidePh - 0.5235988f); // -30 deg
    const float tipGlide = -15.0f + 2.0f * std::sin(glidePh - 1.0471976f);   // -60 deg

    out.shoulderDeg = lerpFloat(shoulderFlap, shoulderGlide, g);
    out.upperDeg = lerpFloat(upperFlap, upperGlide, g);
    out.tipDeg = lerpFloat(tipFlap, tipGlide, g);
    for (int i = 0; i < 4; ++i) {
        const float fi = static_cast<float>(i);
        const float featherFlap =
            lerpFloat(2.5f * std::sin(idlePh - WingsModule::kIdleFeatherLagBase - fi * WingsModule::kIdleFeatherLagStep),
                      WingsModule::kFlightFeatherAmplitudeDegrees
                          * std::sin(phase - WingsModule::kFeatherLagBase - fi * WingsModule::kFeatherLagStep), w);
        const float featherGlide = 4.0f + 2.0f * std::sin(glidePh - 1.5707963f - fi * WingsModule::kIdleFeatherLagStep);
        out.featherDeg[i] = lerpFloat(featherFlap, featherGlide, g);
    }
    out.flapPhase = phase;
    out.intensity = w;
    out.glide = g;
    return out;
}

WingBoneAngles WingsModule::currentBoneAngles() const {
    std::lock_guard<std::mutex> animationLock(m_animationMutex);
    return computeWingPose(m_flapTime, m_flapSpeed, m_intensity, m_glide);
}

WingBoneAngles WingsModule::currentBoneAnglesInterpolated() const {
    // The render hook is called concurrently with the tick callback. Take one
    // coherent animation snapshot before extrapolating it; reading m_flapTime
    // and m_lastFlapTick without this lock occasionally produced a one-frame
    // phase reset, perceived as wing stutter while moving.
    std::lock_guard<std::mutex> animationLock(m_animationMutex);

    // Ultra-low latency: add time since last tick to flapTime for smooth 60+ FPS animation
    float effectiveFlapTime = m_flapTime;
    if (m_flapClockStarted) {
        auto now = std::chrono::steady_clock::now();
        float extra = std::chrono::duration<float>(now - m_lastFlapTick).count();
        if (extra < 0.0f) extra = 0.0f;
        if (extra > 0.1f) extra = 0.1f; // clamp to 100ms to avoid large jumps after hitch
        effectiveFlapTime += extra;
    }

    return computeWingPose(effectiveFlapTime, m_flapSpeed, m_intensity, m_glide);
}

void WingsModule::advanceWingAnimation(float dtSeconds, float horizontalSpeed, float verticalSpeed) {
    if (dtSeconds <= 0.0f) return;
    // Keep the tick update atomic with the render-thread interpolation above.
    // In particular, the phase and its timestamp must belong to the same tick.
    std::lock_guard<std::mutex> animationLock(m_animationMutex);
    m_flapTime += dtSeconds;

    horizontalSpeed = std::clamp(horizontalSpeed, 0.0f, 40.0f);
    verticalSpeed = std::clamp(verticalSpeed, -40.0f, 40.0f);

    // Flap target from horizontal movement; rising fast (e.g. jumping off a
    // ledge) also triggers strong flapping - instant full flap on jump for zero latency.
    float moveT = std::clamp(horizontalSpeed / kWalkSpeedFull, 0.0f, 1.0f);
    if (verticalSpeed > kRiseSpeedFlap) moveT = std::max(moveT, 1.0f);

    // Glide target requires a sustained descent so short hops do not count.
    const bool descending = verticalSpeed < kGlideFallSpeed;
    m_airTime = descending ? m_airTime + dtSeconds : 0.0f;
    const float glideT = (m_airTime > kGlideAirTime) ? 1.0f : 0.0f;

    // While gliding the wings are spread instead of flapping.
    const float intensityT = std::clamp(moveT * (1.0f - glideT), 0.0f, 1.0f);

    const float rate = (intensityT > m_intensity) ? kIntensityAttackRate : kIntensityDecayRate;
    m_intensity += (intensityT - m_intensity) * std::min(1.0f, dtSeconds * rate);
    const float grate = (glideT > m_glide) ? kGlideAttackRate : kGlideDecayRate;
    m_glide += (glideT - m_glide) * std::min(1.0f, dtSeconds * grate);

    // Publish the pose for the render hook (only the live module instance,
    // so host-test instances cannot clobber the in-game render state).
    if (this == g_wings) {
        // computeWingPose() directly: m_animationMutex is already held here and
        // currentBoneAngles() takes it again.
        WingBoneAngles angles = computeWingPose(m_flapTime, m_flapSpeed, m_intensity, m_glide);
        std::lock_guard<std::mutex> lock(s_stateMutex);
        s_boneAngles = angles;
    }
}

void WingsModule::advanceFlapAnimation(float dtSeconds) {
    advanceWingAnimation(dtSeconds, 0.0f, 0.0f);
}

void WingsModule::onLocalPlayerTick(void* player) {
    if (!player) {
        std::lock_guard<std::mutex> lock(s_stateMutex);
        s_hasPlayer = false;
        s_localPlayerPtr = nullptr;
        s_hasPrevSample = false;
        s_lastTickTimeValid = false;
        return;
    }

    if (!enabled) {
        std::lock_guard<std::mutex> lock(s_stateMutex);
        s_hasPlayer = false;
        s_localPlayerPtr = nullptr;
        s_hasPrevSample = false;
        s_lastTickTimeValid = false;
        return;
    }

    // Tick dt from the real clock (clamped after hitches).
    const auto now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (m_flapClockStarted) {
        dt = std::chrono::duration<float>(now - m_lastFlapTick).count();
        if (dt > 0.25f) dt = 0.25f;
    }
    // Publish the tick timestamp before advancing the phase. This keeps the
    // render interpolation origin aligned with the phase produced by this
    // tick, instead of briefly exposing a new phase with the previous origin.
    {
        std::lock_guard<std::mutex> animationLock(m_animationMutex);
        m_lastFlapTick = now;
        m_flapClockStarted = true;
    }

    // Track player AABB and rotation for rendering, and derive the player's
    // speed from consecutive AABB centers (teleports are ignored).
    AABB aabb = getActorAABB(player);
    bedrocktools::sdk::Vec2 rot = getActorBodyRotation(player);

    const float centerX = (aabb.min.x + aabb.max.x) * 0.5f;
    const float centerY = (aabb.min.y + aabb.max.y) * 0.5f;
    const float centerZ = (aabb.min.z + aabb.max.z) * 0.5f;

    float horizontalSpeed = 0.0f;
    float verticalSpeed = 0.0f;
    float velX = 0.0f, velY = 0.0f, velZ = 0.0f;
    bool hasVel = false;

    if (dt > 0.0f) {
        if (m_hasPrevCenter) {
            const float dx = centerX - m_prevCenterX;
            const float dy = centerY - m_prevCenterY;
            const float dz = centerZ - m_prevCenterZ;
            if (dx * dx + dy * dy + dz * dz < 25.0f) { // 5 blocks: not a teleport
                horizontalSpeed = std::sqrt(dx * dx + dz * dz) / dt;
                verticalSpeed = dy / dt;
                velX = dx / dt;
                velY = dy / dt;
                velZ = dz / dt;
                hasVel = true;
            }
        }
        advanceWingAnimation(dt, horizontalSpeed, verticalSpeed);
    }

    m_prevCenterX = centerX;
    m_prevCenterY = centerY;
    m_prevCenterZ = centerZ;
    m_hasPrevCenter = true;

    {
        std::lock_guard<std::mutex> lock(s_stateMutex);

        // Shift the interpolation window: the sample we published last tick
        // becomes the "previous" one and this tick's measured sample becomes
        // the "current" one.  The render hook lerps between them by the
        // partial-tick fraction, which is what removes the walking jitter.  On
        // the very first sample, or after a teleport, keep prev == cur so the
        // lerp is a no-op instead of dragging the wings across a huge gap.
        const bool teleport = m_hasPrevCenter && dt > 0.0f &&
            ((centerX - m_prevCenterX) * (centerX - m_prevCenterX) +
             (centerY - m_prevCenterY) * (centerY - m_prevCenterY) +
             (centerZ - m_prevCenterZ) * (centerZ - m_prevCenterZ) >= 25.0f);
        if (!s_hasPrevSample || teleport) {
            s_prevAABB = aabb;
            s_prevRot = rot;
            s_hasPrevSample = true;
        } else {
            s_prevAABB = s_curAABB;
            s_prevRot = s_curRot;
        }
        s_curAABB = aabb;
        s_curRot = rot;

        // Measure the tick interval (clamped) so the partial-tick fraction
        // stays correct even when the game is not running at exactly 20 Hz.
        if (s_lastTickTimeValid) {
            float meas = std::chrono::duration<float>(now - s_lastTickTime).count();
            if (meas > 0.001f && meas < 0.5f) s_tickInterval = meas;
        }

        s_playerAABB = aabb;
        s_playerRot = rot;
        s_localPlayerPtr = player;
        s_hasPlayer = true;
        s_lastTickTime = now;
        s_lastTickTimeValid = true;
    }
}

bool WingsModule::isThirdPersonCamera(float camX, float camY, float camZ,
                                      float aabbMinX, float aabbMinY, float aabbMinZ,
                                      float aabbMaxX, float aabbMaxY, float aabbMaxZ) {
    // In first-person the camera is at the player's head, which lies inside the
    // collision box. A third-person camera is pulled back outside the box.
    const float m = kFirstPersonMargin;
    const bool cameraInsideBox =
        camX >= aabbMinX - m && camX <= aabbMaxX + m &&
        camY >= aabbMinY - m && camY <= aabbMaxY + m &&
        camZ >= aabbMinZ - m && camZ <= aabbMaxZ + m;
    return !cameraInsideBox;
}

// ---------------------------------------------------------------------------
// Embedded asset files
// ---------------------------------------------------------------------------

static bool writeTextFileIfMissing(const std::string& path, const char* contents) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return true;
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << contents;
    return out.good();
}

void WingsModule::ensureWingsAssetFiles() {
    if (m_wingsDir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(m_wingsDir, ec);
    if (ec) return;

    writeTextFileIfMissing(m_wingsDir + "/wings_geometry.json", wings_default::GeometryJson);
    writeTextFileIfMissing(m_wingsDir + "/wings_animation.json", wings_default::AnimationJson);
    writeTextFileIfMissing(m_wingsDir + "/wings_animation_controllers.json", wings_default::AnimationControllerJson);

    const std::string pngPath = m_wingsDir + "/wings.png";
    std::error_code ec2;
    if (!std::filesystem::exists(pngPath, ec2)) {
        stbi_write_png(pngPath.c_str(),
                       static_cast<int>(wings_default::TextureWidth),
                       static_cast<int>(wings_default::TextureHeight),
                       4, wings_default::TexturePixels,
                       static_cast<int>(wings_default::TextureWidth * 4));
    }
}

void WingsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_flapSpeed")) {
        float speed = j["m_flapSpeed"].get<float>();
        m_flapSpeed = std::clamp(speed, 0.1f, 10.0f);
    } else if (j.contains("flapSpeed")) {
        float speed = j["flapSpeed"].get<float>();
        m_flapSpeed = std::clamp(speed, 0.1f, 10.0f);
    }
    if (j.contains("m_wingStyle")) {
        m_wingStyleIndex = wings::resolveWingStyleIndex(j["m_wingStyle"].get<std::string>());
        m_wingStyle = wings::kWingDesigns[m_wingStyleIndex].id;
    } else if (j.contains("wingStyle")) {
        m_wingStyleIndex = wings::resolveWingStyleIndex(j["wingStyle"].get<std::string>());
        m_wingStyle = wings::kWingDesigns[m_wingStyleIndex].id;
    }
}

void WingsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    float flapSpeed = std::clamp(m_flapSpeed, 0.1f, 10.0f);
    j["m_flapSpeed"] = flapSpeed;
    j["flapSpeed"] = flapSpeed; // keep both keys for compatibility
    m_wingStyleIndex = wings::wingStyleIndexForId(m_wingStyle);
    j["m_wingStyle"] = wings::wingStyleRadioValue(m_wingStyleIndex);
}

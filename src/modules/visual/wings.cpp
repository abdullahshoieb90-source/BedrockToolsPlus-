#include <bedrocktools/modules/visual/wings.hpp>

#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"
#include "../../config/ConfigManager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <mutex>

namespace {

using namespace bedrocktools::sdk::offsets;

// ---------------------------------------------------------------------------
// RenderLevel hook plumbing (same pattern as Hitbox/Breadcrumbs/BlockOutline)
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
static float s_flapAngleRad = 0.0f;
static void* s_localPlayerPtr = nullptr;
static bool s_hasPlayer = false;

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
    bedrocktools::sdk::Vec2 rot{0,0};
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

static std::string wingsDirectoryForConfig() {
    const std::string configPath = bedrocktools::config::ConfigManager::get().getConfigPath();
    const std::size_t lastSlash = configPath.find_last_of('/');
    std::string dir = (lastSlash != std::string::npos) ? configPath.substr(0, lastSlash) : "/sdcard/games/BedrockTools";
    return dir + "/wings";
}

// ---------------------------------------------------------------------------
// Wing rendering
// ---------------------------------------------------------------------------

static void renderWingsOverlay(void* levelRenderer, void* screenContext) {
    if (!screenContext || (std::uintptr_t)screenContext < 0x1000) return;
    if (!levelRenderer || (std::uintptr_t)levelRenderer < 0x1000) return;
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) return;

    AABB aabb;
    bedrocktools::sdk::Vec2 rot;
    float flapRad;
    {
        std::lock_guard<std::mutex> lock(s_stateMutex);
        if (!s_hasPlayer) return;
        aabb = s_playerAABB;
        rot = s_playerRot;
        flapRad = s_flapAngleRad;
    }

    // Validate AABB
    if (aabb.min.x == 0 && aabb.min.y == 0 && aabb.min.z == 0 &&
        aabb.max.x == 0 && aabb.max.y == 0 && aabb.max.z == 0) {
        return;
    }

    std::uintptr_t tessPtr = *(std::uintptr_t*)((std::uintptr_t)screenContext + ScreenContext::mTessellator);
    if (!tessPtr || tessPtr < 0x1000) return;
    void* tess = (void*)tessPtr;

    std::uintptr_t lrpPtr = *(std::uintptr_t*)((std::uintptr_t)levelRenderer + LevelRenderer::mLevelRendererPlayer);
    if (!lrpPtr || lrpPtr < 0x1000) return;

    float camX = *(float*)(lrpPtr + LevelRendererPlayer::mCamPos);
    float camY = *(float*)(lrpPtr + LevelRendererPlayer::mCamPos + 4);
    float camZ = *(float*)(lrpPtr + LevelRendererPlayer::mCamPos + 8);

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

    // Player yaw -> right/forward vectors
    constexpr float kPi = 3.14159265358979323846f;
    float yawDeg = rot.y;
    float yawRad = yawDeg * kPi / 180.0f;
    float cosYaw = std::cos(yawRad);
    float sinYaw = std::sin(yawRad);

    // right = (-cosYaw, -sinYaw) in XZ, forward = (-sinYaw, cosYaw)
    // See HitboxModule comments for derivation.
    float rightX = -cosYaw;
    float rightZ = -sinYaw;
    float fwdX = -sinYaw;
    float fwdZ = cosYaw;

    // Feet center
    float feetX = (aabb.min.x + aabb.max.x) * 0.5f;
    float feetY = aabb.min.y;
    float feetZ = (aabb.min.z + aabb.max.z) * 0.5f;

    // Pivot offsets in local (right, up, forward)
    constexpr float kPivotRightOffset = 0.3f;
    constexpr float kPivotUp = 1.2f;
    constexpr float kPivotBack = -0.20f; // behind player (negative forward)

    // Wing dimensions
    constexpr float kWingW = WingsModule::kWingWidth;  // 0.5
    constexpr float kWingH = WingsModule::kWingHeight; // 0.7

    struct Local2 { float x; float y; };
    // Right wing local corners before flap rotation (relative to pivot)
    Local2 rCorners[4] = {
        {0.0f, 0.0f},
        {kWingW, 0.0f},
        {kWingW, -kWingH},
        {0.0f, -kWingH}
    };
    // Left wing
    Local2 lCorners[4] = {
        {0.0f, 0.0f},
        {-kWingW, 0.0f},
        {-kWingW, -kWingH},
        {0.0f, -kWingH}
    };

    float cosFlapR = std::cos(flapRad);
    float sinFlapR = std::sin(flapRad);
    float cosFlapL = std::cos(-flapRad);
    float sinFlapL = std::sin(-flapRad);

    auto rotateZ = [](Local2 p, float c, float s) -> Local2 {
        return { p.x * c - p.y * s, p.x * s + p.y * c };
    };

    Local2 rRot[4], lRot[4];
    for (int i = 0; i < 4; ++i) rRot[i] = rotateZ(rCorners[i], cosFlapR, sinFlapR);
    for (int i = 0; i < 4; ++i) lRot[i] = rotateZ(lCorners[i], cosFlapL, sinFlapL);

    // Compute pivot world positions
    // pivot = feetCenter + right* xPivot + up* yPivot + forward* zPivot
    float rPivotX = feetX + rightX * kPivotRightOffset + fwdX * kPivotBack;
    float rPivotY = feetY + kPivotUp;
    float rPivotZ = feetZ + rightZ * kPivotRightOffset + fwdZ * kPivotBack;

    float lPivotX = feetX + rightX * (-kPivotRightOffset) + fwdX * kPivotBack;
    float lPivotY = feetY + kPivotUp;
    float lPivotZ = feetZ + rightZ * (-kPivotRightOffset) + fwdZ * kPivotBack;

    struct WorldPos { float x, y, z; };
    WorldPos rWorld[4], lWorld[4];
    for (int i = 0; i < 4; ++i) {
        rWorld[i].x = rPivotX + rightX * rRot[i].x + fwdX * 0.0f;
        rWorld[i].y = rPivotY + rRot[i].y;
        rWorld[i].z = rPivotZ + rightZ * rRot[i].x + fwdZ * 0.0f;
    }
    for (int i = 0; i < 4; ++i) {
        lWorld[i].x = lPivotX + rightX * lRot[i].x;
        lWorld[i].y = lPivotY + lRot[i].y;
        lWorld[i].z = lPivotZ + rightZ * lRot[i].x;
    }

    // Convert to camera-relative
    for (int i = 0; i < 4; ++i) { rWorld[i].x -= camX; rWorld[i].y -= camY; rWorld[i].z -= camZ; }
    for (int i = 0; i < 4; ++i) { lWorld[i].x -= camX; lWorld[i].y -= camY; lWorld[i].z -= camZ; }

    // Render - two quads double-sided (8 verts per wing * 2 sides = 16 per wing? Actually 4+4 per wing)
    // We'll emit 16 vertices total: 2 wings * 8 (front+back)
    constexpr float kBrownR = 94.0f / 255.0f;
    constexpr float kBrownG = 62.0f / 255.0f;
    constexpr float kBrownB = 36.0f / 255.0f;
    constexpr float kAlpha = 1.0f;

    char pad[0x58];
    std::memset(pad, 0, sizeof(pad));

    s_tessBegin(tess, nullptr, 1, 16, 0); // 1 = quad
    s_tessColor(tess, kBrownR, kBrownG, kBrownB, kAlpha);

    // Right wing front
    for (int i = 0; i < 4; ++i) s_tessVertex(tess, rWorld[i].x, rWorld[i].y, rWorld[i].z);
    // Right wing back (reversed)
    for (int i = 3; i >= 0; --i) s_tessVertex(tess, rWorld[i].x, rWorld[i].y, rWorld[i].z);
    // Left wing front
    for (int i = 0; i < 4; ++i) s_tessVertex(tess, lWorld[i].x, lWorld[i].y, lWorld[i].z);
    // Left wing back
    for (int i = 3; i >= 0; --i) s_tessVertex(tess, lWorld[i].x, lWorld[i].y, lWorld[i].z);

    s_renderMesh(screenContext, tess, matFill, pad);

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

static void _renderLevel_hook(void* _this, void* screenContext, void* a3) {
    if (_renderLevel_orig) _renderLevel_orig(_this, screenContext, a3);
    if (!g_wings || !g_wings->enabled) return;
    if (!s_localPlayerPtr) return;
    renderWingsOverlay(_this, screenContext);
}

} // namespace

WingsModule::WingsModule()
    : Module("Wings", "Renders wings as a world-space overlay attached to your back (flaps with sin(time), adjustable Flap Speed). Does not modify your skin.") {
    g_wings = this;
    showInMenu = true;
    hideInHudEditor = true; // world overlay, not HUD
}

WingsModule::~WingsModule() {
    if (g_wings == this) g_wings = nullptr;
}

void WingsModule::onInit() {
    m_wingsDir = wingsDirectoryForConfig();

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
    m_flapTime = 0.0f;
    m_flapClockStarted = false;
    {
        std::lock_guard<std::mutex> lock(s_stateMutex);
        s_flapAngleRad = 0.0f;
    }
}

void WingsModule::onDisable() {
    // No skin to restore; just clear tracked player so overlay disappears immediately
    std::lock_guard<std::mutex> lock(s_stateMutex);
    s_hasPlayer = false;
    s_localPlayerPtr = nullptr;
}

float WingsModule::currentFlapAngleDegrees() const {
    return kFlapAmplitudeDegrees * std::sin(m_flapTime * kFlapBaseRate * m_flapSpeed);
}

float WingsModule::currentFlapAngleRadians() const {
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    return currentFlapAngleDegrees() * kDegToRad;
}

void WingsModule::advanceFlapAnimation(float dtSeconds) {
    if (dtSeconds <= 0.0f) return;
    m_flapTime += dtSeconds;
    float angleRad = currentFlapAngleRadians();
    {
        std::lock_guard<std::mutex> lock(s_stateMutex);
        s_flapAngleRad = angleRad;
    }
}

void WingsModule::onLocalPlayerTick(void* player) {
    if (!player) {
        std::lock_guard<std::mutex> lock(s_stateMutex);
        s_hasPlayer = false;
        s_localPlayerPtr = nullptr;
        return;
    }

    if (!enabled) {
        std::lock_guard<std::mutex> lock(s_stateMutex);
        s_hasPlayer = false;
        s_localPlayerPtr = nullptr;
        return;
    }

    // Advance flap clock with real elapsed time
    const auto now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (m_flapClockStarted) {
        dt = std::chrono::duration<float>(now - m_lastFlapTick).count();
        if (dt > 0.25f) dt = 0.25f; // clamp after hitch
    }
    m_lastFlapTick = now;
    m_flapClockStarted = true;
    if (dt > 0.0f) advanceFlapAnimation(dt);

    // Track player AABB and rotation for rendering
    AABB aabb = getActorAABB(player);
    bedrocktools::sdk::Vec2 rot = getActorRotation(player);

    {
        std::lock_guard<std::mutex> lock(s_stateMutex);
        s_playerAABB = aabb;
        s_playerRot = rot;
        s_localPlayerPtr = player;
        s_hasPlayer = true;
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
}

void WingsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    float flapSpeed = std::clamp(m_flapSpeed, 0.1f, 10.0f);
    j["m_flapSpeed"] = flapSpeed;
    j["flapSpeed"] = flapSpeed; // keep both keys for compatibility
}

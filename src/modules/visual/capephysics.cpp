#include "capephysics.hpp"
#include "modules/player/customcapes.hpp"
#include "modules/player/customcapes_files.hpp"
#include "../../config/ConfigManager.hpp"

#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"

#include <stb/stb_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <system_error>

// See capephysics_sim.hpp for the cloth solver and the palette sampler; this
// file is the plumbing: live cape pixels, the vanilla-mesh hide, and the
// tessellator overlay (the same RenderLevel hook pattern as Wings/Hitbox).
namespace cp = bedrocktools::modules::capephysics;

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
        o.sharedPtrData[0] = nullptr;
        o.sharedPtrData[1] = nullptr;
    }
    MaterialPtr& operator=(MaterialPtr&& o) noexcept {
        if (this != &o) {
            sharedPtrData[0] = o.sharedPtrData[0];
            sharedPtrData[1] = o.sharedPtrData[1];
            o.sharedPtrData[0] = nullptr;
            o.sharedPtrData[1] = nullptr;
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

// ---------------------------------------------------------------------------
// Player tracking helpers (identical to Wings: the collision AABB and body
// rotation are sampled on the tick thread and interpolated on the render
// thread exactly like the game interpolates the player mesh).
// ---------------------------------------------------------------------------

struct AABB {
    bedrocktools::sdk::Vec3 min{0, 0, 0};
    bedrocktools::sdk::Vec3 max{0, 0, 0};
};

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

static bedrocktools::sdk::Vec2 getActorBodyRotation(void* actor) {
    bedrocktools::sdk::Vec2 bodyRotation{0, 0};
    std::uintptr_t actorAddr = (std::uintptr_t)actor;
    if (actorAddr < 0x1000) return bodyRotation;
    std::uintptr_t rotComp = *(std::uintptr_t*)(actorAddr + Actor::mActorRotationComponent);
    if (rotComp < 0x1000) return bodyRotation;
    bodyRotation = *(bedrocktools::sdk::Vec2*)rotComp;
    return bodyRotation;
}

static float lerpAngleDeg(float a, float b, float t) {
    float diff = b - a;
    while (diff > 180.0f) diff -= 360.0f;
    while (diff < -180.0f) diff += 360.0f;
    return a + diff * t;
}

static AABB lerpAABB(const AABB& a, const AABB& b, float t) {
    AABB out;
    out.min.x = a.min.x + (b.min.x - a.min.x) * t;
    out.min.y = a.min.y + (b.min.y - a.min.y) * t;
    out.min.z = a.min.z + (b.min.z - a.min.z) * t;
    out.max.x = a.max.x + (b.max.x - a.max.x) * t;
    out.max.y = a.max.y + (b.max.y - a.max.y) * t;
    out.max.z = a.max.z + (b.max.z - a.max.z) * t;
    return out;
}

constexpr float kFirstPersonMargin = 0.05f; // blocks (same as Wings/Hitbox)

static bool isThirdPersonCamera(float camX, float camY, float camZ,
                                const AABB& aabb) {
    const float m = kFirstPersonMargin;
    const bool cameraInsideBox =
        camX >= aabb.min.x - m && camX <= aabb.max.x + m &&
        camY >= aabb.min.y - m && camY <= aabb.max.y + m &&
        camZ >= aabb.min.z - m && camZ <= aabb.max.z + m;
    return !cameraInsideBox;
}

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------

static Tessellator_begin_t s_tessBegin = nullptr;
static Tessellator_color_t s_tessColor = nullptr;
static Tessellator_vertex_t s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;

static MaterialPtr s_matSelection;
static MaterialPtr s_matFill;
static std::uintptr_t s_renderMaterialGroup = 0;

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
// Overlay geometry emission
// ---------------------------------------------------------------------------

// One quad of the cloth sheet. Both windings are emitted so back-face
// culling can never eat a face when the cape folds over itself (same trick
// as the Wings module's emitFace).
static void emitQuad(void* tess, const cp::Vec3& a, const cp::Vec3& b,
                     const cp::Vec3& c, const cp::Vec3& d, const float rgb[3]) {
    s_tessColor(tess, rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f, 1.0f);
    s_tessVertex(tess, a.x, a.y, a.z);
    s_tessVertex(tess, b.x, b.y, b.z);
    s_tessVertex(tess, c.x, c.y, c.z);
    s_tessVertex(tess, d.x, d.y, d.z);
    s_tessVertex(tess, d.x, d.y, d.z);
    s_tessVertex(tess, c.x, c.y, c.z);
    s_tessVertex(tess, b.x, b.y, b.z);
    s_tessVertex(tess, a.x, a.y, a.z);
}

static cp::Vec3 toCameraRelative(const cp::Vec3& p, const cp::Vec3& cam) {
    return {p.x - cam.x, p.y - cam.y, p.z - cam.z};
}

static void (*_renderLevel_orig)(void* _this, void* screenContext, void* a3) = nullptr;

static void renderCapeOverlay(CapePhysicsModule* self, void* levelRenderer, void* screenContext) {
    if (!screenContext || (std::uintptr_t)screenContext < 0x1000) return;
    if (!levelRenderer || (std::uintptr_t)levelRenderer < 0x1000) return;
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) return;

    // Snapshot of the tick-thread state (player box, rotation, colors).
    const CapePhysicsSnapshot snap = self->copySnapshot();
    const bool hasPlayer = snap.hasPlayer;
    const bool hasColors = snap.hasColors;
    AABB aabb{};
    aabb.min = {snap.aabbMin.x, snap.aabbMin.y, snap.aabbMin.z};
    aabb.max = {snap.aabbMax.x, snap.aabbMax.y, snap.aabbMax.z};
    AABB prevAABB{};
    prevAABB.min = {snap.prevAabbMin.x, snap.prevAabbMin.y, snap.prevAabbMin.z};
    prevAABB.max = {snap.prevAabbMax.x, snap.prevAabbMax.y, snap.prevAabbMax.z};
    bedrocktools::sdk::Vec2 rot{snap.rotX, snap.rotY};
    const bedrocktools::sdk::Vec2 prevRot{snap.prevRotX, snap.prevRotY};
    const float tickInterval = snap.tickInterval;
    const bool hasPrevSample = snap.hasPrevSample;
    const bool lastTickValid = snap.lastTickValid;
    const std::chrono::steady_clock::time_point lastTick = snap.lastTick;
    const std::uint32_t canvasVersion = snap.canvasVersion;
    void* playerPtr = snap.localPlayer;
    if (!hasPlayer || !hasColors) return;

    // --- Low-latency anchor: the freshest AABB/rotation straight from the
    // actor, then partial-tick interpolation against the previous sample
    // (identical to Wings — this is what removes the walking jitter).
    if (playerPtr && (std::uintptr_t)playerPtr >= 0x1000) {
        AABB liveAABB = getActorAABB(playerPtr);
        const float width = liveAABB.max.x - liveAABB.min.x;
        const float height = liveAABB.max.y - liveAABB.min.y;
        const float depth = liveAABB.max.z - liveAABB.min.z;
        const bool finite =
            std::isfinite(liveAABB.min.x) && std::isfinite(liveAABB.min.y) && std::isfinite(liveAABB.min.z) &&
            std::isfinite(liveAABB.max.x) && std::isfinite(liveAABB.max.y) && std::isfinite(liveAABB.max.z);
        if (finite && width > 0.0f && width < 16.0f &&
            height > 0.0f && height < 16.0f &&
            depth > 0.0f && depth < 16.0f) {
            aabb = liveAABB;
            bedrocktools::sdk::Vec2 liveRot = getActorBodyRotation(playerPtr);
            if (std::isfinite(liveRot.x) && std::isfinite(liveRot.y)) {
                rot = liveRot;
            }
        }
    }

    const bool curValid =
        std::isfinite(aabb.min.x) && std::isfinite(aabb.min.y) && std::isfinite(aabb.min.z) &&
        std::isfinite(aabb.max.x) && std::isfinite(aabb.max.y) && std::isfinite(aabb.max.z) &&
        aabb.max.x > aabb.min.x && aabb.max.y > aabb.min.y && aabb.max.z > aabb.min.z &&
        std::isfinite(rot.x) && std::isfinite(rot.y);
    if (!curValid) return;

    if (hasPrevSample && lastTickValid && tickInterval > 0.0f) {
        auto now = std::chrono::steady_clock::now();
        float f = std::chrono::duration<float>(now - lastTick).count() / tickInterval;
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        aabb = lerpAABB(prevAABB, aabb, f);
        rot.x = lerpAngleDeg(prevRot.x, rot.x, f);
        rot.y = lerpAngleDeg(prevRot.y, rot.y, f);
    }

    std::uintptr_t tessPtr = *(std::uintptr_t*)((std::uintptr_t)screenContext + ScreenContext::mTessellator);
    if (!tessPtr || tessPtr < 0x1000) return;
    void* tess = (void*)tessPtr;

    std::uintptr_t lrpPtr = *(std::uintptr_t*)((std::uintptr_t)levelRenderer + LevelRenderer::mLevelRendererPlayer);
    if (!lrpPtr || lrpPtr < 0x1000) return;

    float camX = *(float*)(lrpPtr + LevelRendererPlayer::mCamPos);
    float camY = *(float*)(lrpPtr + LevelRendererPlayer::mCamPos + 4);
    float camZ = *(float*)(lrpPtr + LevelRendererPlayer::mCamPos + 8);

    const float feetX = (aabb.min.x + aabb.max.x) * 0.5f;
    const float feetY = aabb.min.y;
    const float feetZ = (aabb.min.z + aabb.max.z) * 0.5f;

    // First-person: the camera sits inside the player's head, so the cape
    // would clip the view; only draw from a real third-person viewpoint.
    if (!isThirdPersonCamera(camX, camY, camZ, aabb)) return;

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

    // ------------------------------------------------------------------
    // Palette refresh: rebuild the per-cell colors whenever the published
    // canvas or the detail preset changed (cheap: 10x16 or 14x22 cells).
    // ------------------------------------------------------------------
    self->refreshPalette(canvasVersion);

    const int detail = cp::clampDetailIndex(self->m_detail);
    const int cols = cp::kDetailPresets[detail].cols;
    const int rows = cp::kDetailPresets[detail].rows;
    if (self->paletteForRender().empty()) {
        colorHolder[0] = savedColor[0];
        colorHolder[1] = savedColor[1];
        colorHolder[2] = savedColor[2];
        colorHolder[3] = savedColor[3];
        return;
    }

    const cp::Vec3 cam{camX, camY, camZ};
    const cp::Vec3 feet{feetX, feetY, feetZ};
    const cp::BodyFrame body{feet, rot.y, aabb.max.y - aabb.min.y};

    // Shoulder anchor line for this frame.
    cp::Vec3 anchors[64];
    cp::buildAnchors(body, cols, anchors);

    // ------------------------------------------------------------------
    // Cloth step: fixed 1/60 s substeps from a frame accumulator, so the
    // cape behaves identically at 30, 60 or 120 fps and a hitch can never
    // fling it (backlog is capped at 5 substeps per frame).
    // ------------------------------------------------------------------
    const auto now = std::chrono::steady_clock::now();
    float frameDt = 1.0f / 60.0f;
    if (self->frameClockStarted()) {
        frameDt = std::chrono::duration<float>(now - self->lastFrameTime()).count();
        if (frameDt < 0.0f) frameDt = 0.0f;
        if (frameDt > 0.1f) frameDt = 0.1f;
    }
    self->noteFrameTime(now);

    cp::ClothParams params;
    params.gravityMul = self->m_gravity;
    params.windMul = self->m_windStrength;
    params.stiffness = self->m_stiffness;

    self->advanceCloth(frameDt, anchors, body, params, cols, rows);

    // ------------------------------------------------------------------
    // Emit the cloth: outer face (cape design) and inner face (lining)
    // offset by half the cape thickness along each cell normal, plus side
    // strips along the four borders so the sheet keeps the vanilla cape's
    // visible thickness. All vertices are camera-relative; every face is
    // shaded with the same headlight+sky model the Wings overlay uses.
    // ------------------------------------------------------------------
    const cp::Cloth& cloth = self->clothForTest();
    const cp::CapePalette& palette = self->paletteForRender();

    const int cells = cols * rows;
    const int vertexCount = (cells * 2 + 2 * cols + 2 * rows) * 8;

    char pad[0x58];
    std::memset(pad, 0, sizeof(pad));
    s_tessBegin(tess, nullptr, 1, vertexCount, 0); // 1 = quad

    const float halfThick = cp::kCapeThicknessBlocks * 0.5f;

    // Emits a border strip (a quad connecting the outer and the inner
    // sheet) with its own face shading.
    const auto emitStrip = [&](const cp::Vec3& a, const cp::Vec3& b,
                               const cp::Vec3& c, const cp::Vec3& d, const std::uint8_t rgb[3]) {
        const cp::Vec3 normal = cp::cross(b - a, c - a);
        const cp::Vec3 center = (a + b + c + d) * 0.25f;
        const float brightness = cp::faceBrightness(normal, center * -1.0f, cp::kDefaultLight);
        float col[3];
        cp::shadeRgb(rgb, brightness, col);
        emitQuad(tess, a, b, c, d, col);
    };

    for (int j = 0; j < rows; ++j) {
        for (int i = 0; i < cols; ++i) {
            const cp::Vec3 p00 = toCameraRelative(cloth.at(i, j), cam);
            const cp::Vec3 p10 = toCameraRelative(cloth.at(i + 1, j), cam);
            const cp::Vec3 p11 = toCameraRelative(cloth.at(i + 1, j + 1), cam);
            const cp::Vec3 p01 = toCameraRelative(cloth.at(i, j + 1), cam);

            cp::Vec3 normal = cp::cross(p10 - p00, p01 - p00);
            const cp::Vec3 center = (p00 + p10 + p11 + p01) * 0.25f;

            // The outer face points away from the body axis, whichever way
            // the cell is currently folded.
            const cp::Vec3 axisPoint{feetX - cam.x, center.y, feetZ - cam.z};
            if (cp::dot(normal, center - axisPoint) < 0.0f) {
                normal = normal * -1.0f;
            }
            const cp::Vec3 n = cp::normalized(normal);

            const float brightness = cp::faceBrightness(n, center * -1.0f, cp::kDefaultLight);
            const std::size_t cell = (static_cast<std::size_t>(j) * cols + i) * 3u;
            float outerCol[3], innerCol[3];
            cp::shadeRgb(&palette.outer[cell], brightness, outerCol);
            cp::shadeRgb(&palette.inner[cell], brightness * 0.92f, innerCol);

            const cp::Vec3 o00 = p00 + n * halfThick;
            const cp::Vec3 o10 = p10 + n * halfThick;
            const cp::Vec3 o11 = p11 + n * halfThick;
            const cp::Vec3 o01 = p01 + n * halfThick;
            const cp::Vec3 i00 = p00 - n * halfThick;
            const cp::Vec3 i10 = p10 - n * halfThick;
            const cp::Vec3 i11 = p11 - n * halfThick;
            const cp::Vec3 i01 = p01 - n * halfThick;

            emitQuad(tess, o00, o10, o11, o01, outerCol);
            emitQuad(tess, i00, i10, i11, i01, innerCol);

            // Border strips keep the cape's 1-px thickness visible along the
            // sheet's edges, colored from the cape's edge texels.
            if (i == 0) {
                emitStrip(o00, i00, i01, o01, &palette.edgeLeft[static_cast<std::size_t>(j) * 3u]);
            }
            if (i == cols - 1) {
                emitStrip(i10, o10, o11, i11, &palette.edgeRight[static_cast<std::size_t>(j) * 3u]);
            }
            if (j == 0) {
                emitStrip(o00, o10, i10, i00, &palette.edgeTop[static_cast<std::size_t>(i) * 3u]);
            }
            if (j == rows - 1) {
                emitStrip(i01, i11, o11, o01, &palette.edgeBottom[static_cast<std::size_t>(i) * 3u]);
            }
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
    if (!g_capePhysics || !g_capePhysics->enabled) return;
    renderCapeOverlay(g_capePhysics, _this, screenContext);
}

} // namespace

CapePhysicsModule* g_capePhysics = nullptr;

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

namespace {

constexpr int kLoadRetryTicks = 120;
constexpr std::uint32_t kAcceptedFormats[] = {3, 4}; // RGBA8Unorm and Custom Capes' tag
constexpr std::uint32_t kMaxLiveCanvasDimension = 2048;

bool isAcceptedCapeFormat(std::uint32_t format) {
    for (std::uint32_t f : kAcceptedFormats) {
        if (f == format) return true;
    }
    return false;
}

std::string capeDirectoryForConfig() {
    const std::string configPath = bedrocktools::config::ConfigManager::get().getConfigPath();
    const std::size_t lastSlash = configPath.find_last_of('/');
    std::string dir = (lastSlash != std::string::npos) ? configPath.substr(0, lastSlash)
                                                       : "/sdcard/games/BedrockToolsPlus";
    return dir + "/capes";
}

// Radio value for the Cape picker: "<index>,Worn Cape,<file1>,..." — entry 0
// is the cape the game currently renders (vanilla / marketplace / whatever
// Custom Capes patched in), everything after it is a file from the shared
// capes folder. Same radio convention as Custom Capes' picker.
std::string makeCapeSourceRadioValue(int selectedIndex, const std::vector<std::string>& files) {
    const int optionCount = 1 + static_cast<int>(files.size());
    if (selectedIndex < 0) selectedIndex = 0;
    if (selectedIndex >= optionCount) selectedIndex = optionCount - 1;

    std::string value = std::to_string(selectedIndex);
    value += ",Worn Cape";
    for (const std::string& file : files) {
        value += ',';
        value += file;
    }
    return value;
}

} // namespace

CapePhysicsModule::CapePhysicsModule()
    : Module("Cape Physics",
             "Real cloth physics for your cape: it hangs, sways, flutters while running and "
             "streams behind you while falling. Works with the cape you are wearing or any PNG "
             "from the capes folder (any size, 22x23 to 704x736 and beyond). Third-person only, "
             "visual only.") {
    g_capePhysics = this;
    hideInHudEditor = true; // world overlay, not HUD
}

CapePhysicsModule::~CapePhysicsModule() {
    if (g_capePhysics == this) g_capePhysics = nullptr;
}

void CapePhysicsModule::onInit() {
    m_capesDir = capeDirectoryForConfig();
    ensureCapesDirectory();
    m_files = customcapes::scanCapeFiles(m_capesDir);

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
            if (g_capePhysics) g_capePhysics->onLocalPlayerTick(event.player);
        });
}

void CapePhysicsModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    auto handle = bedrocktools::hooks::install(m_patchTarget, (void*)_renderLevel_hook, (void**)&_renderLevel_orig);
    m_patched = handle != nullptr;
}

void CapePhysicsModule::onEnable() {
    applyPatch();
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_clothNeedsReset = true;
    m_substepAccumulator = 0.0f;
    m_frameClockStarted = false;
}

void CapePhysicsModule::onDisable() {
    // The skin is only ever written from the tick with a live level link,
    // so the cape-id restore happens on the next tick (the subscription
    // keeps firing); here we only stop the overlay.
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_snapshot.hasPlayer = false;
        m_snapshot.hasColors = false;
        m_clothNeedsReset = true;
    }
    if (g_customCapes) g_customCapes->setCapeMeshSuppressed(false);
}

void CapePhysicsModule::ensureCapesDirectory() const {
    std::error_code ec;
    if (!std::filesystem::is_directory(m_capesDir, ec)) {
        std::filesystem::create_directories(m_capesDir, ec);
    }
    // The sample cape is written by the Custom Capes module; the folder is
    // shared, so there is nothing else to seed here.
}

void CapePhysicsModule::releaseLoadedCapeFile() {
    m_fileCanvas.clear();
    m_fileCanvas.shrink_to_fit();
    m_fileLoaded = false;
    m_fileLoadFailed = false;
    m_fileRetryTicks = 0;
}

void CapePhysicsModule::loadSelectedCapeFile() {
    releaseLoadedCapeFile();
    if (m_selectedFileIndex <= 0 || m_selectedFileIndex > static_cast<int>(m_files.size())) return;

    const std::string path = m_capesDir + "/" + m_files[static_cast<std::size_t>(m_selectedFileIndex - 1)];

    int width = 0, height = 0, channels = 0;
    stbi_uc* decoded = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0 ||
        width > static_cast<int>(customcapes::kMaxSourceDimension) ||
        height > static_cast<int>(customcapes::kMaxSourceDimension)) {
        if (decoded) stbi_image_free(decoded);
        m_fileLoadFailed = true;
        return;
    }

    // The Custom Capes resampler maps ANY size onto the classic 64x32 cape
    // canvas (Fit / Fill / Crop — see its module docs), so the physics cape
    // accepts exactly the same sources: 22x23, 64x32, HD 128x64, 704x736...
    m_fileCanvas = customcapes::resampleToCape(decoded, static_cast<std::uint32_t>(width),
                                               static_cast<std::uint32_t>(height),
                                               customcapes::capeFitModeFromIndex(m_capeFit));
    stbi_image_free(decoded);
    m_fileLoaded = true;
}

bool CapePhysicsModule::capeIdIsEmpty(const void* capeIdAddr) {
    // A short-string id is non-empty when its length byte is non-zero; a
    // long-string id always has non-zero capacity/size bytes somewhere in
    // the 24-byte std::string. "All bytes zero" is the canonical empty
    // representation in both layouts.
    const std::uint8_t* p = static_cast<const std::uint8_t*>(capeIdAddr);
    for (int i = 0; i < 24; ++i) {
        if (p[i] != 0) return false;
    }
    return true;
}

bool CapePhysicsModule::updateCanvasFromSkin(void* skin) {
    if (!skin) return false;
    if (*reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mIsPersona)) {
        return false; // persona skins have no classic cape image
    }

    const uintptr_t capeImage =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mCapeImage;
    const std::uint32_t format =
        *reinterpret_cast<std::uint32_t*>(capeImage + Image::mImageFormat);
    const std::uint32_t w =
        *reinterpret_cast<std::uint32_t*>(capeImage + SkinImage::mWidth);
    const std::uint32_t h =
        *reinterpret_cast<std::uint32_t*>(capeImage + SkinImage::mHeight);
    void* blob = *reinterpret_cast<void**>(capeImage + Image::mBytesOffset);
    const std::size_t blobSize =
        *reinterpret_cast<std::size_t*>(capeImage + Image::mBlobSizeOffset);

    if (!blob || w == 0 || h == 0) return false; // no cape worn
    if (w > kMaxLiveCanvasDimension || h > kMaxLiveCanvasDimension) return false;
    if (!isAcceptedCapeFormat(format)) return false;
    if (blobSize < static_cast<std::size_t>(w) * h * 4u) return false;
    if (blob == m_liveBlobToken && blobSize == m_liveBlobSizeToken) {
        return true; // pixels unchanged since the last copy
    }

    // Sample proportionally: the cape face regions are fractions of the
    // canvas, so 64x32, HD and oversized cape images all resolve to the
    // same 10x16 face (see capephysics_sim.hpp).
    std::vector<std::uint8_t> normalized;
    cp::normalizeCapeCanvas(static_cast<const std::uint8_t*>(blob), w, h, normalized);

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_canvas.size() != cp::kCanvasBytes || std::memcmp(m_canvas.data(), normalized.data(), cp::kCanvasBytes) != 0) {
            m_canvas = normalized;
            ++m_canvasSerial;
            m_snapshot.canvasVersion = m_canvasSerial;
        }
    }
    m_liveBlobToken = blob;
    m_liveBlobSizeToken = blobSize;
    return true;
}

void CapePhysicsModule::hideCapeMeshIfWanted(void* skin, bool persona) {
    const bool wantHide = m_hideVanilla && skin != nullptr && !persona;
    if (g_customCapes) g_customCapes->setCapeMeshSuppressed(wantHide);
    if (!wantHide) {
        unhideCapeMesh(skin);
        return;
    }

    const uintptr_t capeIdAddr =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mCapeId;
    if (capeIdIsEmpty(reinterpret_cast<const void*>(capeIdAddr))) {
        // No classic cape id in place (either no cape is worn or it is
        // already hidden) — nothing to clear, but keep any backup we own.
        return;
    }

    if (m_idHiddenSkin != skin) {
        // Fresh skin object: our old backup belongs to a skin the engine
        // has replaced, drop it before sampling the new one.
        m_idHiddenSkin = skin;
        m_hasSavedCapeId = false;
    }
    if (!m_hasSavedCapeId) {
        std::memcpy(m_savedCapeId, reinterpret_cast<const void*>(capeIdAddr), sizeof(m_savedCapeId));
        m_hasSavedCapeId = true;
    }
    // An empty id tells the game there is no classic cape to render — the
    // same field Custom Capes fills to make its cape show up, in reverse.
    std::memset(reinterpret_cast<void*>(capeIdAddr), 0, 24);
}

void CapePhysicsModule::unhideCapeMesh(void* skin) {
    if (!m_hasSavedCapeId) {
        m_idHiddenSkin = nullptr;
        return;
    }
    if (!skin || skin != m_idHiddenSkin) {
        // The skin object we patched is gone (world exit / re-skin): the
        // engine destroyed it together with our cleared id — drop the
        // backup, nothing to restore into.
        m_hasSavedCapeId = false;
        m_idHiddenSkin = nullptr;
        return;
    }

    const uintptr_t capeIdAddr =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mCapeId;
    if (capeIdIsEmpty(reinterpret_cast<const void*>(capeIdAddr))) {
        // Restore the id exactly as we found it (bit-for-bit, so long-string
        // ids are restored too, not just short ones).
        std::memcpy(reinterpret_cast<void*>(capeIdAddr), m_savedCapeId, sizeof(m_savedCapeId));
    }
    // If the id is non-empty somebody else (e.g. Custom Capes re-applying)
    // has taken ownership of the field in the meantime — leave theirs.
    m_hasSavedCapeId = false;
    m_idHiddenSkin = nullptr;
}

bool CapePhysicsModule::playerHasLiveLevel(const void* player) {
    if (!player) return false;
    const void* level = *reinterpret_cast<const void* const*>(
        reinterpret_cast<std::uintptr_t>(player) + Actor::mLevel);
    return level != nullptr;
}

void* CapePhysicsModule::resolvePlayerSkin(void* player) {
    if (!player) return nullptr;
    void* skinRefPtr = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(player) + Player::mSkin);
    if (!skinRefPtr) return nullptr;
    void* sharedBase = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(skinRefPtr) + SerializedSkinRef::mSkinImpl);
    if (!sharedBase) return nullptr;
    void* threadOwner = *reinterpret_cast<void**>(sharedBase);
    if (!threadOwner) return nullptr;
    return reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(threadOwner) + ThreadOwner::mObject);
}

void CapePhysicsModule::onWorldExit() {
    // The level is gone: the player and its skin are gone or on their way
    // out. Never write to the skin here (the Leave World crash Custom Capes
    // documents) — dropping our references is all that is needed: the cape
    // id is re-hidden and the pixels re-sampled from the fresh skin object
    // when the player joins a world again.
    m_idHiddenSkin = nullptr;
    m_hasSavedCapeId = false;
    m_liveBlobToken = nullptr;
    m_liveBlobSizeToken = 0;
    if (g_customCapes) g_customCapes->setCapeMeshSuppressed(false);
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_snapshot = CapePhysicsSnapshot{};
        m_clothNeedsReset = true;
    }
}

void CapePhysicsModule::onLocalPlayerTick(void* player) {
    // --- World-exit guard (see Custom Capes for the Leave World crash this
    // prevents: after the engine detaches the player from its level the
    // skin object must not be read or written).
    if (!player || !playerHasLiveLevel(player)) {
        onWorldExit();
        return;
    }

    void* skin = resolvePlayerSkin(player);
    const bool persona = skin != nullptr && *reinterpret_cast<bool*>(
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mIsPersona);

    if (!enabled) {
        unhideCapeMesh(skin);
        if (g_customCapes) g_customCapes->setCapeMeshSuppressed(false);
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_snapshot.hasPlayer = false;
        m_snapshot.hasColors = false;
        m_clothNeedsReset = true;
        return;
    }

    // --- Pixel source --------------------------------------------------
    bool hasColors = false;
    if (m_selectedFileIndex > 0) {
        if (!m_fileLoaded && !m_fileLoadFailed) {
            if (m_fileRetryTicks <= 0) {
                loadSelectedCapeFile();
                if (!m_fileLoaded) m_fileRetryTicks = kLoadRetryTicks;
            } else {
                --m_fileRetryTicks;
            }
        }
        if (m_fileLoaded) {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (m_canvas.size() != m_fileCanvas.size() ||
                std::memcmp(m_canvas.data(), m_fileCanvas.data(), m_fileCanvas.size()) != 0) {
                m_canvas = m_fileCanvas;
                ++m_canvasSerial;
                m_snapshot.canvasVersion = m_canvasSerial;
            }
            hasColors = true;
        }
    } else {
        hasColors = updateCanvasFromSkin(skin);
    }

    // --- Hide the flat vanilla cape mesh while the cloth is drawn -------
    // Only when there is something to replace it with: hiding the mesh
    // while the physics cape has no colors (unreadable/absent cape source)
    // would leave the player with no cape at all.
    if (hasColors) {
        hideCapeMeshIfWanted(skin, persona);
    } else {
        unhideCapeMesh(skin);
        if (g_customCapes) g_customCapes->setCapeMeshSuppressed(false);
    }

    // --- Publish the player sample for the render hook ------------------
    AABB aabb = getActorAABB(player);
    bedrocktools::sdk::Vec2 rot = getActorBodyRotation(player);

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(m_stateMutex);

    m_snapshot.hasColors = hasColors;
    m_snapshot.localPlayer = player;

    const bool teleport =
        m_snapshot.hasPlayer && m_snapshot.hasPrevSample &&
        (aabb.min.x - m_snapshot.aabbMin.x) * (aabb.min.x - m_snapshot.aabbMin.x) +
                (aabb.min.z - m_snapshot.aabbMin.z) * (aabb.min.z - m_snapshot.aabbMin.z) >= 25.0f;
    if (!m_snapshot.hasPrevSample || teleport) {
        m_snapshot.prevAabbMin = {aabb.min.x, aabb.min.y, aabb.min.z};
        m_snapshot.prevAabbMax = {aabb.max.x, aabb.max.y, aabb.max.z};
        m_snapshot.prevRotX = rot.x;
        m_snapshot.prevRotY = rot.y;
        m_snapshot.hasPrevSample = true;
        m_clothNeedsReset = true; // never drag the cape across a teleport
    } else {
        m_snapshot.prevAabbMin = m_snapshot.aabbMin;
        m_snapshot.prevAabbMax = m_snapshot.aabbMax;
        m_snapshot.prevRotX = m_snapshot.rotX;
        m_snapshot.prevRotY = m_snapshot.rotY;
    }

    if (m_snapshot.hasPlayer && m_snapshot.lastTickValid) {
        float meas = std::chrono::duration<float>(now - m_snapshot.lastTick).count();
        if (meas > 0.001f && meas < 0.5f) m_snapshot.tickInterval = meas;
    }

    m_snapshot.aabbMin = {aabb.min.x, aabb.min.y, aabb.min.z};
    m_snapshot.aabbMax = {aabb.max.x, aabb.max.y, aabb.max.z};
    m_snapshot.rotX = rot.x;
    m_snapshot.rotY = rot.y;
    m_snapshot.hasPlayer = true;
    m_snapshot.lastTick = now;
    m_snapshot.lastTickValid = true;
}

void CapePhysicsModule::refreshPalette(std::uint32_t canvasVersion) {
    // Render-thread only (called from the RenderLevel hook). Rebuild the
    // per-cell colors when the published canvas or the detail preset
    // changed; otherwise reuse the cached palette.
    const int detail = cp::clampDetailIndex(m_detail);
    if (canvasVersion == m_paletteCanvasVersion && detail == m_paletteDetail &&
        !m_palette.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_renderCanvas = m_canvas;
    }
    if (m_renderCanvas.size() != cp::kCanvasBytes) return;
    cp::buildCapePalette(m_renderCanvas.data(), cp::kCanvasWidth, cp::kCanvasHeight,
                         cp::kDetailPresets[detail].cols, cp::kDetailPresets[detail].rows, m_palette);
    m_paletteCanvasVersion = canvasVersion;
    m_paletteDetail = detail;
}

void CapePhysicsModule::advanceCloth(float frameDt, const cp::Vec3 anchors[],
                                     const cp::BodyFrame& body, const cp::ClothParams& params,
                                     int cols, int rows) {
    // Render-thread only (called from the RenderLevel hook).
    if (m_cloth.cols() != cols || m_cloth.rows() != rows || m_clothNeedsReset) {
        m_cloth.configure(cols, rows);
        m_cloth.reset(anchors, body);
        m_clothNeedsReset = false;
        m_substepAccumulator = 0.0f;
    }

    constexpr float kSubstepDt = 1.0f / 60.0f;
    m_substepAccumulator += frameDt;
    int steps = 0;
    while (m_substepAccumulator >= kSubstepDt && steps < 5) {
        m_simTime += kSubstepDt;
        m_cloth.step(kSubstepDt, anchors, body, params, m_simTime);
        m_substepAccumulator -= kSubstepDt;
        ++steps;
    }
    if (steps == 5) m_substepAccumulator = 0.0f; // drop the backlog after a hitch
}

void CapePhysicsModule::noteFrameTime(std::chrono::steady_clock::time_point now) {
    m_lastFrame = now;
    m_frameClockStarted = true;
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

void CapePhysicsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    if (m_capesDir.empty()) m_capesDir = capeDirectoryForConfig();

    const int previousFile = m_selectedFileIndex;
    const int previousFit = m_capeFit;

    if (j.contains("m_cape")) {
        int parsedIndex = 0;
        std::string parsedName;
        if (j["m_cape"].is_string()) {
            customcapes::parseRadioValue(j["m_cape"].get<std::string>(), parsedIndex, parsedName);
        } else if (j["m_cape"].is_number_integer()) {
            parsedIndex = j["m_cape"].get<int>();
        }
        m_files = customcapes::scanCapeFiles(m_capesDir);
        if (!parsedName.empty() && parsedName != "Worn Cape" && parsedName != customcapes::kNoneLabel) {
            m_selectedFileIndex = 0;
            for (std::size_t i = 0; i < m_files.size(); ++i) {
                if (m_files[i] == parsedName) {
                    m_selectedFileIndex = static_cast<int>(i) + 1;
                    break;
                }
            }
        } else {
            m_selectedFileIndex = (parsedIndex >= 0 && parsedIndex <= static_cast<int>(m_files.size()))
                                      ? parsedIndex
                                      : 0;
        }
        m_cape = m_selectedFileIndex;
    }

    if (j.contains("m_capeFit")) {
        int parsedFit = 0;
        if (j["m_capeFit"].is_string()) {
            int idx = 0;
            std::string label;
            customcapes::parseRadioValue(j["m_capeFit"].get<std::string>(), idx, label);
            const int byLabel = customcapes::capeFitIndexFromLabel(label);
            parsedFit = byLabel >= 0 ? byLabel : customcapes::clampCapeFitIndex(idx);
        } else if (j["m_capeFit"].is_number_integer()) {
            parsedFit = j["m_capeFit"].get<int>();
        }
        m_capeFit = customcapes::clampCapeFitIndex(parsedFit);
    }

    if (j.contains("m_detail")) {
        int parsedDetail = m_detail;
        if (j["m_detail"].is_string()) {
            int idx = 0;
            std::string label;
            customcapes::parseRadioValue(j["m_detail"].get<std::string>(), idx, label);
            const int byLabel = cp::detailIndexFromLabel(label);
            parsedDetail = byLabel >= 0 ? byLabel : cp::clampDetailIndex(idx);
        } else if (j["m_detail"].is_number_integer()) {
            parsedDetail = j["m_detail"].get<int>();
        }
        m_detail = cp::clampDetailIndex(parsedDetail);
    }

    if (j.contains("m_windStrength")) {
        m_windStrength = std::clamp(j["m_windStrength"].get<float>(), 0.0f, 2.0f);
    }
    if (j.contains("m_gravity")) {
        m_gravity = std::clamp(j["m_gravity"].get<float>(), 0.0f, 2.0f);
    }
    if (j.contains("m_stiffness")) {
        m_stiffness = std::clamp(j["m_stiffness"].get<float>(), 0.05f, 1.0f);
    }
    if (j.contains("m_hideVanilla")) {
        m_hideVanilla = j["m_hideVanilla"].get<bool>();
    }

    if (m_selectedFileIndex != previousFile || m_capeFit != previousFit) {
        releaseLoadedCapeFile();
        if (m_selectedFileIndex > 0) loadSelectedCapeFile();
    }
}

void CapePhysicsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    if (m_capesDir.empty()) m_capesDir = capeDirectoryForConfig();
    m_files = customcapes::scanCapeFiles(m_capesDir);
    if (m_selectedFileIndex > static_cast<int>(m_files.size())) m_selectedFileIndex = 0;
    m_cape = m_selectedFileIndex;
    j["m_cape"] = makeCapeSourceRadioValue(m_selectedFileIndex, m_files);
    m_capeFit = customcapes::clampCapeFitIndex(m_capeFit);
    j["m_capeFit"] = customcapes::makeLabelRadioValue(m_capeFit, customcapes::capeFitLabelList());
    m_detail = cp::clampDetailIndex(m_detail);
    j["m_detail"] = cp::detailRadioValue(m_detail);
    j["m_windStrength"] = std::clamp(m_windStrength, 0.0f, 2.0f);
    j["m_gravity"] = std::clamp(m_gravity, 0.0f, 2.0f);
    j["m_stiffness"] = std::clamp(m_stiffness, 0.05f, 1.0f);
    j["m_hideVanilla"] = m_hideVanilla;
}

// ---------------------------------------------------------------------------
// Test hooks
// ---------------------------------------------------------------------------

bool CapePhysicsModule::hasRenderColors() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_snapshot.hasColors;
}

CapePhysicsSnapshot CapePhysicsModule::copySnapshot() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    return m_snapshot;
}

const std::vector<std::uint8_t>& CapePhysicsModule::renderCanvasForTest() const {
    return m_canvas;
}

const cp::Cloth& CapePhysicsModule::clothForTest() const {
    return m_cloth;
}

bool CapePhysicsModule::hidingVanillaCapeForTest() const {
    return m_hasSavedCapeId;
}

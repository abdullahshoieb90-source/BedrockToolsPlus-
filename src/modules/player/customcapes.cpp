#include "customcapes.hpp"
#include "customcapes_files.hpp"
#include "customcapes_render_math.hpp"

#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/ClientInstanceUpdateEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"
#include "../../config/ConfigManager.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_JPEG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include <stb/stb_image.h>
#include <stb/stb_image_write.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <vector>

#if defined(__ANDROID__)
// The self-rendered cape overlay uploads its 64x32 PNG through the GLES
// context directly (the game's render context is current inside the
// RenderLevel hook). All of this is compiled out on the host so the unit
// tests never need EGL/GLES headers.
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#endif

CustomCapesModule* g_customCapes = nullptr;

namespace {

using namespace bedrocktools::sdk::offsets;

void freeBlobDeleter(unsigned char* data) {
    std::free(data);
}

// mce::ImageFormat, exactly as declared in the game binary:
//     enum class ImageFormat : uint32 {
//         Unknown = 0, R8Unorm = 1, RGB8Unorm = 2, RGBA8Unorm = 3
//     };
// (verified against Reference/mce/ImageFormat.h in the 1.20.51 client symbols
// and Flarial's 1.21.x SDK — both agree). Skin and cape textures are decoded
// by the game as RGBA8Unorm == 3. Writing any value >= 4 hands the texture
// factory an out-of-range format, so texture creation fails and the cape is
// silently never drawn — the reported "no cape on the player" symptom. The old
// constant used 4 (a naive "4 channels" guess), which the engine rejected for
// every cape, even the generated sample.
constexpr std::uint32_t kCapeImageFormat = 3;
// mce::Image::mDepth is 1 for every 2D texture. The engine derives the
// texture description and the pixel-byte count (width * height * depth *
// bytesPerPixel) from this field, so a cape image left at depth 0 is
// rejected by the texture factory and never reaches the screen.
constexpr std::uint32_t kCapeImageDepth = 1;
constexpr int kLoadRetryTicks = 120;
constexpr const char* kCapeIdBase = "bedrocktoolsplus";
constexpr std::size_t kCapeIdBaseLen = 16;

void writeShortStdString(uintptr_t addr, const char* text, std::size_t len) {
    if (len > 22) return; // libc++ short-string capacity inside 24 bytes
    unsigned char* p = reinterpret_cast<unsigned char*>(addr);
    std::memset(p, 0, 24);
    p[0] = static_cast<unsigned char>(len << 1);
    std::memcpy(p + 1, text, len);
}

bool shortStdStringEquals(uintptr_t addr, const char* text, std::size_t len) {
    if (len > 22) return false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(addr);
    return p[0] == static_cast<unsigned char>(len << 1) &&
           std::memcmp(p + 1, text, len) == 0;
}

bool shortStdStringHasPrefix(uintptr_t addr, const char* prefix, std::size_t prefixLen) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(addr);
    // We only write short strings (<= 22 bytes). If the low bit is set this is
    // a libc++ long string, which is not one of our synthetic ids.
    if ((p[0] & 1u) != 0u) return false;
    const std::size_t len = static_cast<std::size_t>(p[0] >> 1);
    return len >= prefixLen && len <= 22 && std::memcmp(p + 1, prefix, prefixLen) == 0;
}

bool isPlausibleCapeImage(uintptr_t capeImage) {
    const std::uint32_t format = *reinterpret_cast<std::uint32_t*>(capeImage + Image::mImageFormat);
    const std::uint32_t width = *reinterpret_cast<std::uint32_t*>(capeImage + SkinImage::mWidth);
    const std::uint32_t height = *reinterpret_cast<std::uint32_t*>(capeImage + SkinImage::mHeight);
    const std::uint32_t depth = *reinterpret_cast<std::uint32_t*>(capeImage + Image::mDepth);
    const std::uint32_t usage = *reinterpret_cast<std::uint32_t*>(capeImage + Image::mUsage);
    void* blob = *reinterpret_cast<void**>(capeImage + Image::mBytesOffset);
    const std::size_t size = *reinterpret_cast<std::size_t*>(capeImage + Image::mBlobSizeOffset);

    const bool emptyCape = width == 0 && height == 0 && blob == nullptr && size == 0;
    const bool classicCape = ((width == customcapes::kCapeWidth && height == customcapes::kCapeHeight) ||
                              (width == customcapes::kCapeWidth * 2 && height == customcapes::kCapeHeight * 2)) &&
                             blob != nullptr && size >= static_cast<std::size_t>(width) * height * 4u;

    // ImageFormat is the 0..3 enum above. An all-zero (Unknown) image is the
    // capeless-player case the patch exists to fill in; 3 (RGBA8Unorm) is the
    // format every real skin/cape image uses. Values >= 4 are out of enum
    // range and mean the offset no longer points at an mce::Image — touching
    // it would corrupt the skin (observed as a Steve fallback or a
    // skin-change crash on shifted game layouts).
    return (emptyCape || classicCape) && (format == 0 || format == kCapeImageFormat) &&
           (depth == 0 || depth == 1) && usage <= 4;
}

std::string capeDirectoryForConfig() {
    const std::string configPath = bedrocktools::config::ConfigManager::get().getConfigPath();
    const std::size_t lastSlash = configPath.find_last_of('/');
    std::string dir = (lastSlash != std::string::npos) ? configPath.substr(0, lastSlash)
                                                       : "/sdcard/games/BedrockToolsPlus";
    return dir + "/capes";
}

void* resolvePlayerSkin(void* player) {
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

} // namespace

namespace {

// ---------------------------------------------------------------------------
// Self-rendered cape overlay (RenderMode Overlay / Both)
//
// Draws the selected cape as a textured rectangle on the local player's back
// in the RenderLevel hook, reusing the exact plumbing the Wings module proves
// on-device (tessellator + renderMeshImmediately + render-material group).
// The texture is our own GLES texture built from the same resampled 64x32
// pixels the skin patch hands the engine, so the overlay does not depend on
// the engine's cape mesh at all — it is the fallback for the case where the
// ActorRendererData was built before the early patch could land.
// ---------------------------------------------------------------------------

using namespace bedrocktools::sdk::offsets;

typedef void (*Tessellator_begin_t)(void*, void*, int, int, int);
typedef void (*Tessellator_color_t)(void*, float, float, float, float);
typedef void (*Tessellator_vertex_t)(void*, float, float, float);
typedef void (*MeshHelpers_renderMeshImmediately_t)(void*, void*, void*, char*);

struct RenderMaterialPtr {
    void* sharedPtrData[2]{nullptr, nullptr};
    explicit operator bool() const { return sharedPtrData[0] != nullptr; }
};

// Render-thread shared state (written on the game thread from the tick /
// update hooks, read on the render thread inside the RenderLevel hook).
std::mutex g_renderMutex;

struct CapePose {
    bool valid = false;
    float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
    float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
    float yaw = 0.0f;
};
CapePose g_pose;

// Snapshot of the resampled 64x32 RGBA cape pixels for the render thread.
// g_capeRevision mirrors the module's m_capeIdSerial so the render thread can
// tell when a new cape was selected and re-upload the texture.
std::vector<std::uint8_t> g_capePixels;
std::uint64_t g_capeRevision = 0;
bool g_hasCapePixels = false;

// Engine functions resolved from the signature table (0 when unavailable).
Tessellator_begin_t g_tessBegin = nullptr;
Tessellator_color_t g_tessColor = nullptr;
Tessellator_vertex_t g_tessVertex = nullptr;
MeshHelpers_renderMeshImmediately_t g_renderMesh = nullptr;
std::uintptr_t g_renderMaterialGroup = 0;
RenderMaterialPtr g_matTextured;  // ui_textured_and_glcolor (samples a texture)
RenderMaterialPtr g_matFallback;  // selection_box / ui_fill_color / debug_filled_box
void (*g_renderLevelOrig)(void*, void*, void*) = nullptr;
bedrocktools::hooks::Handle g_renderLevelHook = nullptr;

#if defined(__ANDROID__)
GLuint g_capeTexId = 0;
std::uint64_t g_capeTexUploadedRevision = 0;
#endif

// The RenderMaterialGroupCommon signature resolves to an ADRP+ADD pair that
// materializes the render-material-group singleton pointer; copied from the
// Wings module (same engine mechanism, verified on-device there).
std::uintptr_t resolveADRP(std::uint32_t* insns, std::size_t count, std::uint32_t targetReg) {
    for (std::size_t i = 0; i < count; i++) {
        std::uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;
        if ((insn & 0x9F000000) == 0x90000000) {
            std::uintptr_t page = ((std::uintptr_t)&insns[i] & ~0xFFFULL)
                + ((int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29) & 3) << 43) >> 31);
            for (std::size_t j = i + 1; j < count; j++) {
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

struct RenderHashedString {
    std::uint64_t mStrHash;
    std::string mStr;
    mutable const RenderHashedString* mLastMatch;
    RenderHashedString() : mStrHash(0), mStr(), mLastMatch(nullptr) {}
    explicit RenderHashedString(const char* str) : mLastMatch(nullptr) {
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

RenderMaterialPtr getRenderMaterial(const char* name) {
    if (!g_renderMaterialGroup) return {};
    RenderHashedString hs(name);
    void** vtable = *reinterpret_cast<void***>(g_renderMaterialGroup);
    if (!vtable || !vtable[VTable::RenderMaterialGroup_getMaterial]) return {};
    using getMat_t = RenderMaterialPtr (*)(void*, const RenderHashedString*);
    return reinterpret_cast<getMat_t>(vtable[VTable::RenderMaterialGroup_getMaterial])((void*)g_renderMaterialGroup, &hs);
}

void ensureRenderMaterials() {
    if (!g_renderMaterialGroup) return;
    if (!g_matTextured) g_matTextured = getRenderMaterial("ui_textured_and_glcolor");
    if (!g_matFallback) {
        static const char* kNames[] = {"selection_box", "ui_fill_color", "debug_filled_box"};
        for (const char* n : kNames) {
            g_matFallback = getRenderMaterial(n);
            if (g_matFallback) break;
        }
    }
}

// The actor pose (collision box + body yaw) published by the tick/update
// hooks, exactly like the Wings module reads it.
bool readActorPose(void* player, CapePose& out) {
    if (!player) return false;
    const std::uintptr_t actorAddr = reinterpret_cast<std::uintptr_t>(player);
    if (actorAddr < 0x1000) return false;

    const std::uintptr_t builtIn = *reinterpret_cast<std::uintptr_t*>(
        actorAddr + Actor::mStateVectorComponent);
    if (builtIn < 0x1000) return false;
    // Wings/Hitbox/Breadcrumbs layout: the AABB-component pointer is at
    // actor+0x210 (mStateVectorComponent + 8), NOT at *(actor+0x208)+8.
    // 0x208 is not a usable pointer on the live layout; dereferencing it
    // read garbage and crashed on world entry even when the module was off.
    const std::uintptr_t aabbComp = *reinterpret_cast<std::uintptr_t*>(
        actorAddr + Actor::mStateVectorComponent + BuiltInActorComponents::mAABBShapeComponent);
    if (aabbComp < 0x1000) return false;
    const float* aabb = reinterpret_cast<const float*>(aabbComp + AABBShapeComponent::mAABB);

    const std::uintptr_t rotComp = *reinterpret_cast<std::uintptr_t*>(
        actorAddr + Actor::mActorRotationComponent);
    if (rotComp < 0x1000) return false;
    const float* rot = reinterpret_cast<const float*>(rotComp);

    out.minX = aabb[0]; out.minY = aabb[1]; out.minZ = aabb[2];
    out.maxX = aabb[3]; out.maxY = aabb[4]; out.maxZ = aabb[5];
    out.yaw = rot[1];  // ActorRotationComponent is {pitch, yaw} like wings reads
    const float dx = out.maxX - out.minX;
    const float dy = out.maxY - out.minY;
    const float dz = out.maxZ - out.minZ;
    out.valid = std::isfinite(dx) && std::isfinite(dy) && std::isfinite(dz) &&
                dx > 0.0f && dx < 16.0f && dy > 0.0f && dy < 16.0f && dz > 0.0f && dz < 16.0f;
    return out.valid;
}

void setTessUv(void* tess, float u, float v) {
    *reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(tess) + Tessellator::mTextureU) = u;
    *reinterpret_cast<float*>(reinterpret_cast<std::uintptr_t>(tess) + Tessellator::mTextureV) = v;
}

void emitCapeVertex(void* tess, const customcapes::render::CapeVertex& v,
                    float camX, float camY, float camZ) {
    g_tessColor(tess, 1.0f, 1.0f, 1.0f, 1.0f);
    setTessUv(tess, v.u, v.v);
    g_tessVertex(tess, v.pos.x - camX, v.pos.y - camY, v.pos.z - camZ);
}

// Emits one quad (4 corners) twice — once per winding — so back-face culling
// can never eat the cape.
void emitCapeQuad(void* tess, const customcapes::render::CapeVertex corners[4],
                  float camX, float camY, float camZ) {
    for (int i = 0; i < 4; ++i) emitCapeVertex(tess, corners[i], camX, camY, camZ);
    for (int i = 3; i >= 0; --i) emitCapeVertex(tess, corners[i], camX, camY, camZ);
}

#if defined(__ANDROID__)
// Uploads (or refreshes) the GLES texture from a render-thread snapshot of
// the cape pixels. `pixels` is the copy taken under the mutex, so the upload
// is atomic with the size check even if the user switches capes mid-frame.
void ensureCapeTexture(std::uint64_t revision, const std::vector<std::uint8_t>& pixels) {
    if (g_capeTexId == 0) glGenTextures(1, &g_capeTexId);
    glBindTexture(GL_TEXTURE_2D, g_capeTexId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 static_cast<GLsizei>(customcapes::kCapeWidth),
                 static_cast<GLsizei>(customcapes::kCapeHeight), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    g_capeTexUploadedRevision = revision;
}
#endif

void renderCapeOverlay(void* levelRenderer, void* screenContext) {
    if (!screenContext || (std::uintptr_t)screenContext < 0x1000) return;
    if (!levelRenderer || (std::uintptr_t)levelRenderer < 0x1000) return;
    if (!g_tessBegin || !g_tessColor || !g_tessVertex || !g_renderMesh) return;
    if (!g_customCapes || !g_customCapes->enabled) return;
    if (g_customCapes->renderMode() == CustomCapesModule::RenderModeEngine) return;

    CapePose pose;
    std::vector<std::uint8_t> pixels;
    [[maybe_unused]] std::uint64_t revision = 0;  // used by the GLES upload (Android)
    bool hasCape = false;
    {
        std::lock_guard<std::mutex> lock(g_renderMutex);
        pose = g_pose;
        hasCape = g_hasCapePixels;
        pixels = g_capePixels;
        revision = g_capeRevision;
    }
    if (!pose.valid || !hasCape || pixels.size() !=
            static_cast<std::size_t>(customcapes::kCapeWidth) * customcapes::kCapeHeight * 4u) {
        return;
    }

    const std::uintptr_t lrpPtr = *(std::uintptr_t*)((std::uintptr_t)levelRenderer + LevelRenderer::mLevelRendererPlayer);
    if (!lrpPtr || lrpPtr < 0x1000) return;
    const float camX = *(float*)(lrpPtr + LevelRendererPlayer::mCamPos);
    const float camY = *(float*)(lrpPtr + LevelRendererPlayer::mCamPos + 4);
    const float camZ = *(float*)(lrpPtr + LevelRendererPlayer::mCamPos + 8);

    // First-person: the camera sits inside the player's head (inside the
    // AABB), so the back-mounted cape would clip the view — same convention
    // as the Wings and Hitbox modules.
    if (!customcapes::render::isThirdPersonCamera(
            camX, camY, camZ, pose.minX, pose.minY, pose.minZ,
            pose.maxX, pose.maxY, pose.maxZ)) {
        return;
    }

    const std::uintptr_t tessPtr = *(std::uintptr_t*)((std::uintptr_t)screenContext + ScreenContext::mTessellator);
    if (!tessPtr || tessPtr < 0x1000) return;
    void* tess = (void*)tessPtr;

    ensureRenderMaterials();
    void* overlayMat = (void*)(lrpPtr + LevelRendererPlayer::mSelectionOverlayMaterial);
    void* mat = g_matTextured ? (void*)&g_matTextured
                              : (g_matFallback ? (void*)&g_matFallback : overlayMat);
    if (!mat) return;

    std::uintptr_t colorHolderPtr = *(std::uintptr_t*)((std::uintptr_t)screenContext + ScreenContext::mColorHolder);
    if (!colorHolderPtr || colorHolderPtr < 0x1000) return;
    float* colorHolder = (float*)colorHolderPtr;
    const float savedColor[4] = {colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3]};
    colorHolder[0] = 1.0f; colorHolder[1] = 1.0f; colorHolder[2] = 1.0f; colorHolder[3] = 1.0f;

#if defined(__ANDROID__)
    // Bind our cape texture on unit 0 and let the textured material sample it.
    if (revision != g_capeTexUploadedRevision) ensureCapeTexture(revision, pixels);
    if (g_capeTexId == 0) {
        colorHolder[0] = savedColor[0]; colorHolder[1] = savedColor[1];
        colorHolder[2] = savedColor[2]; colorHolder[3] = savedColor[3];
        return;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_capeTexId);
#endif

    // Feet center + body yaw -> cape corners with a time-based hem sway.
    const float feetX = (pose.minX + pose.maxX) * 0.5f;
    const float feetY = pose.minY;
    const float feetZ = (pose.minZ + pose.maxZ) * 0.5f;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const float swayPhase = std::chrono::duration<float>(now).count() *
                            customcapes::render::kCapeSwaySpeed;

    customcapes::render::CapeVertex corners[6];
    customcapes::render::buildCapeQuad(feetX, feetY, feetZ, pose.yaw, swayPhase, corners);

    const int vertexCount = 2 /* segments */ * 4 /* quad */ * 2 /* windings */;
    g_tessBegin(tess, nullptr, 1, vertexCount, 0);  // 1 = quads

    const customcapes::render::CapeVertex upper[4] = {corners[0], corners[1], corners[2], corners[3]};
    const customcapes::render::CapeVertex lower[4] = {corners[3], corners[2], corners[5], corners[4]};
    emitCapeQuad(tess, upper, camX, camY, camZ);
    emitCapeQuad(tess, lower, camX, camY, camZ);

    char pad[0x58];
    std::memset(pad, 0, sizeof(pad));
    g_renderMesh(screenContext, tess, mat, pad);

#if defined(__ANDROID__)
    glBindTexture(GL_TEXTURE_2D, 0);
#endif
    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

void renderLevelHook(void* _this, void* screenContext, void* a3) {
    if (g_renderLevelOrig) g_renderLevelOrig(_this, screenContext, a3);
    if (!g_customCapes || !g_customCapes->enabled) return;
    renderCapeOverlay(_this, screenContext);
}

} // namespace

CustomCapesModule::CustomCapesModule()
    : Module("Custom Capes",
            "Wear any PNG from the BedrockToolsPlus capes folder as your cape (local only). "
            "Render mode: Engine mesh patches the skin before the cape mesh is built (apply at world join); "
            "Overlay draws the cape itself (works mid-game); Both is diagnostic.") {
    g_customCapes = this;
}

CustomCapesModule::~CustomCapesModule() {
    if (g_customCapes == this) g_customCapes = nullptr;
}

void CustomCapesModule::onInit() {
    m_capesDir = capeDirectoryForConfig();
    ensureCapesDirectory();
    m_files = customcapes::scanCapeFiles(m_capesDir);

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_customCapes) g_customCapes->onLocalPlayerTick(event.player);
        });

    // Every-frame, before-the-render-pass hook: this is what fixes the
    // KNOWN LIMITATION. ClientInstance::update runs once per frame and the
    // level render follows it in the same frame, so the first update after
    // the local player is created patches the cape into SerializedSkinImpl
    // before ActorRendererData is built from the skin — the engine then
    // creates the cape mesh and uploads our texture itself.
    bedrocktools::events::bus().subscribe<bedrocktools::events::ClientInstanceUpdateEvent>(
        [](auto& event) {
            if (g_customCapes) g_customCapes->onClientInstanceUpdate(event.clientInstance);
        });

    // Resolve the render-side functions for the Overlay/Both render modes
    // (RenderLevel hook + tessellator + material group). These are the same
    // signatures the Wings module uses; resolving returns 0 harmlessly on
    // hosts or on builds where a signature shifted, in which case the
    // overlay simply never draws.
    const std::uintptr_t tb = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin);
    if (tb) g_tessBegin = reinterpret_cast<Tessellator_begin_t>(tb);
    const std::uintptr_t tc = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor);
    if (tc) g_tessColor = reinterpret_cast<Tessellator_color_t>(tc);
    const std::uintptr_t tv = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex);
    if (tv) g_tessVertex = reinterpret_cast<Tessellator_vertex_t>(tv);
    const std::uintptr_t rm = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if (rm) {
        g_renderMesh = reinterpret_cast<MeshHelpers_renderMeshImmediately_t>(rm);
    } else {
        const std::uintptr_t rm5 = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);
        if (rm5) g_renderMesh = reinterpret_cast<MeshHelpers_renderMeshImmediately_t>(rm5);
    }
    const std::uintptr_t rmg = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
    if (rmg) {
        const std::uintptr_t groupAddr = resolveADRP(reinterpret_cast<std::uint32_t*>(rmg), 2, 0);
        if (groupAddr) g_renderMaterialGroup = groupAddr + MaterialGroup::mRenderMaterialGroupOffset;
    }
}

void CustomCapesModule::onEnable() {
    std::lock_guard<std::mutex> patchLock(m_patchMutex);
    if (m_selectedIndex > 0 && !m_capeLoaded && !m_loadFailed) loadSelectedCape();
    m_needsApply = true;
    publishOverlayCape();
    syncOverlayHook();
}

void CustomCapesModule::onDisable() {
    std::lock_guard<std::mutex> patchLock(m_patchMutex);
    {
        std::lock_guard<std::mutex> lock(g_renderMutex);
        g_pose = CapePose{};
    }
    syncOverlayHook();
}

void CustomCapesModule::ensureCapesDirectory() {
    std::error_code ec;
    if (std::filesystem::is_directory(m_capesDir, ec)) return;
    if (!std::filesystem::create_directories(m_capesDir, ec) || ec) return;
    writeSamplePng(m_capesDir + "/Sample Cape.png");
}

void CustomCapesModule::writeSamplePng(const std::string& path) const {
    std::vector<std::uint8_t> src(customcapes::kCapeBackWidth * customcapes::kCapeBackHeight * 4u);
    for (std::uint32_t y = 0; y < customcapes::kCapeBackHeight; ++y) {
        for (std::uint32_t x = 0; x < customcapes::kCapeBackWidth; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * customcapes::kCapeBackWidth + x) * 4u;
            const float t = static_cast<float>(y) / static_cast<float>(customcapes::kCapeBackHeight - 1);
            src[i + 0] = static_cast<std::uint8_t>(70 + 120 * t);
            src[i + 1] = static_cast<std::uint8_t>(30 + 30 * t);
            src[i + 2] = static_cast<std::uint8_t>(160 + 60 * t);
            src[i + 3] = 255;
            const bool border = x < 2 || y < 2 ||
                                x >= customcapes::kCapeBackWidth - 2 ||
                                y >= customcapes::kCapeBackHeight - 2;
            const bool stripe = x == 4 || x == 5;
            if (border || stripe) {
                src[i + 0] = 255; src[i + 1] = 220; src[i + 2] = 60; src[i + 3] = 255;
            }
        }
    }
    const std::vector<std::uint8_t> px = customcapes::resampleToCape(
        src.data(), customcapes::kCapeBackWidth, customcapes::kCapeBackHeight);
    stbi_write_png(path.c_str(), customcapes::kCapeWidth, customcapes::kCapeHeight, 4,
                   px.data(), customcapes::kCapeWidth * 4);
}

void CustomCapesModule::loadConfig(const nlohmann::json& j) {
    // Module::loadConfig() may dispatch into onEnable()/onDisable() through
    // updateEnabledState(), so it must run outside m_patchMutex (those entry
    // points take the lock themselves). Every cape/config switch below runs
    // under m_patchMutex so it cannot race a tick/update frame that is
    // currently applying or restoring the skin patch.
    Module::loadConfig(j);

    std::lock_guard<std::mutex> patchLock(m_patchMutex);

    if (m_capesDir.empty()) m_capesDir = capeDirectoryForConfig();
    const int previousIndex = m_selectedIndex;

    if (j.contains("m_cape")) {
        int parsedIndex = m_selectedIndex;
        std::string parsedName;
        if (j["m_cape"].is_string()) {
            customcapes::parseRadioValue(j["m_cape"].get<std::string>(), parsedIndex, parsedName);
        } else if (j["m_cape"].is_number_integer()) {
            parsedIndex = j["m_cape"].get<int>();
        }
        m_files = customcapes::scanCapeFiles(m_capesDir);
        m_selectedIndex = customcapes::resolveSelectionIndex(parsedIndex, parsedName, m_files);
    }

    if (j.contains("m_capeRenderMode")) {
        int parsedMode = m_renderMode;
        std::string parsedLabel;
        if (j["m_capeRenderMode"].is_string()) {
            customcapes::parseRadioValue(j["m_capeRenderMode"].get<std::string>(),
                                         parsedMode, parsedLabel);
        } else if (j["m_capeRenderMode"].is_number_integer()) {
            parsedMode = j["m_capeRenderMode"].get<int>();
        }
        // The radio labels are the authoritative option list; a plain index
        // read from an older config is clamped instead.
        if (!parsedLabel.empty()) {
            for (int i = 0; i < kRenderModeRadioCount; ++i) {
                if (parsedLabel == kRenderModeRadioLabels[i]) { parsedMode = i; break; }
            }
        }
        if (parsedMode < 0 || parsedMode >= kRenderModeRadioCount) parsedMode = RenderModeEngine;
        m_renderMode = parsedMode;
    }

    if (m_selectedIndex != previousIndex || (m_selectedIndex > 0 && !m_capeLoaded)) {
        releaseLoadedCape();
        if (m_selectedIndex > 0) loadSelectedCape();
        m_needsApply = true;
        publishOverlayCape();
    }
    syncOverlayHook();
}

void CustomCapesModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    if (m_capesDir.empty()) m_capesDir = capeDirectoryForConfig();
    m_files = customcapes::scanCapeFiles(m_capesDir);
    if (m_selectedIndex > static_cast<int>(m_files.size())) m_selectedIndex = 0;
    j["m_cape"] = customcapes::makeRadioValue(m_selectedIndex, m_files);

    // "0,Engine mesh,Overlay,Both" — the launcher renders any comma-separated
    // string value as a radio picker (see ModuleMenu.cpp), and
    // parseRadioValue() maps the chosen label back to the enum.
    std::string renderRadio = std::to_string(m_renderMode);
    for (int i = 0; i < kRenderModeRadioCount; ++i) {
        renderRadio += ',';
        renderRadio += kRenderModeRadioLabels[i];
    }
    j["m_capeRenderMode"] = renderRadio;
}

void CustomCapesModule::releaseLoadedCape() {
    m_pixels.clear();
    m_pixels.shrink_to_fit();
    m_capeLoaded = false;
    m_loadFailed = false;
    m_retryTicks = 0;
}

void CustomCapesModule::loadSelectedCape() {
    releaseLoadedCape();
    if (m_selectedIndex <= 0 || m_selectedIndex > static_cast<int>(m_files.size())) return;

    const std::string path = m_capesDir + "/" + m_files[static_cast<std::size_t>(m_selectedIndex - 1)];

    int width = 0, height = 0, channels = 0;
    stbi_uc* decoded = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0 ||
        width > static_cast<int>(customcapes::kMaxSourceDimension) ||
        height > static_cast<int>(customcapes::kMaxSourceDimension)) {
        if (decoded) stbi_image_free(decoded);
        m_loadFailed = true;
        return;
    }

    m_pixels = customcapes::resampleToCape(decoded, static_cast<std::uint32_t>(width),
                                           static_cast<std::uint32_t>(height));
    stbi_image_free(decoded);
    m_capeLoaded = true;

    ++m_capeIdSerial;
    m_activeCapeId = std::string(kCapeIdBase) + "-" + std::to_string(m_capeIdSerial);
    if (m_activeCapeId.size() > 22) {
        m_activeCapeId = std::string(kCapeIdBase) + "-" + std::to_string(m_capeIdSerial % 1000000);
        if (m_activeCapeId.size() > 22) m_activeCapeId.resize(22);
    }
}

void CustomCapesModule::backupOriginalCape(std::uintptr_t capeImage, std::uintptr_t capeIdAddr) {
    m_backup.format = *reinterpret_cast<uint32_t*>(capeImage + Image::mImageFormat);
    m_backup.width = *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mWidth);
    m_backup.height = *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mHeight);
    m_backup.depth = *reinterpret_cast<uint32_t*>(capeImage + Image::mDepth);
    m_backup.usage = *reinterpret_cast<uint32_t*>(capeImage + Image::mUsage);
    m_backup.blob = *reinterpret_cast<void**>(capeImage + Image::mBytesOffset);
    m_backup.deleter = *reinterpret_cast<void**>(capeImage + Image::mBlobDeleterOffset);
    m_backup.size = *reinterpret_cast<std::size_t*>(capeImage + Image::mBlobSizeOffset);
    std::memcpy(m_backup.capeIdBytes, reinterpret_cast<const void*>(capeIdAddr),
                sizeof(m_backup.capeIdBytes));

    // If the game rebuilt the skin in-place after our previous patch, it can
    // temporarily leave the synthetic cape id behind while replacing the image
    // blob. Treat that id as ours, not as the new skin's vanilla cape id, so
    // disabling the module will not restore an invalid custom id and force the
    // client back to Steve.
    if (shortStdStringHasPrefix(capeIdAddr, kCapeIdBase, kCapeIdBaseLen)) {
        std::memset(m_backup.capeIdBytes, 0, sizeof(m_backup.capeIdBytes));
    }

    m_hasBackup = true;
}

void CustomCapesModule::clearPatchState() {
    m_patchedSkin = nullptr;
    m_injectedBlob = nullptr;
    m_hasBackup = false;
    m_backup = CapeBackup{};
}

bool CustomCapesModule::playerHasLiveLevel(const void* player) {
    if (!player) return false;
    // Actor::mLevel is the first link the engine severs on Leave World,
    // before the player and its skin objects are destroyed. A live level
    // link means the skin object this player owns is still live.
    const void* level = *reinterpret_cast<const void* const*>(
        reinterpret_cast<std::uintptr_t>(player) + Actor::mLevel);
    return level != nullptr;
}

void CustomCapesModule::onWorldExit() {
    std::lock_guard<std::mutex> patchLock(m_patchMutex);
    // The level is gone, so the player and its skin are gone or on their
    // way out. Two hard rules:
    //   1. Never write to the skin here. Restoring the vanilla cape into a
    //      freed SerializedSkinImpl is the use-after-free that crashed the
    //      game on Leave World; restoreOriginalCape() only runs while the
    //      level is live (see onLocalPlayerTick).
    //   2. Never free m_injectedBlob here. The blob was handed to the skin
    //      tagged with freeBlobDeleter, so the engine frees it while it
    //      destroys the skin — freeing it again would double-free.
    // Dropping our references is all that is needed: m_pixels (the cape
    // file) is module-owned and stays loaded, so the cape is re-applied to
    // the fresh skin object when the player joins a world again.
    m_patchedSkin = nullptr;
    m_injectedBlob = nullptr; // ownership: engine (frees via the deleter tag)
    m_hasBackup = false;
    m_backup = CapeBackup{};
    m_needsApply = true; // next live skin must be (re)patched from scratch

    // The player is gone, so the overlay has nothing to anchor to.
    std::lock_guard<std::mutex> lock(g_renderMutex);
    g_pose = CapePose{};
}

void CustomCapesModule::onLocalPlayerTick(void* player) {
    // --- World-exit guard (Leave World crash fix) -----------------------
    // While the engine tears down the world it (1) detaches the player
    // from its level and then (2) destroys the player and its skin. Any
    // tick that reaches us after step 1 must not read or write the skin
    // object: it is freed memory, and writing the vanilla cape back into
    // it — or freeing our injected blob a second time — is the access
    // violation / double-free that crashed the game on Leave World.
    if (!player || !playerHasLiveLevel(player)) {
        onWorldExit();
        return;
    }

    // Serialize the whole patch path: both ClientInstanceUpdateEvent
    // (update thread) and LocalPlayerTickEvent (tick thread) can call into
    // this logic in the same frame. Lock order is m_patchMutex -> g_renderMutex.
    std::lock_guard<std::mutex> patchLock(m_patchMutex);

    // Publish the player pose + cape pixels for the overlay render thread,
    // but ONLY while the overlay is actually drawing (module enabled + a
    // cape selected + Overlay/Both). The old code published the pose on
    // every tick — including with the module disabled — and that
    // unconditional read of the actor's live offsets is what crashed on
    // world entry.
    const bool overlayActive = enabled && m_selectedIndex > 0 && m_capeLoaded &&
                               m_renderMode != RenderModeEngine;
    {
        std::lock_guard<std::mutex> lock(g_renderMutex);
        g_pose = CapePose{};  // readActorPose leaves .valid untouched on failure
        if (overlayActive) readActorPose(player, g_pose);
        if (overlayActive && m_capeIdSerial != g_capeRevision) {
            g_capePixels = m_pixels;
            g_capeRevision = m_capeIdSerial;
            g_hasCapePixels = !g_capePixels.empty();
        }
    }

    if (m_selectedIndex > 0 && !m_capeLoaded && enabled) {
        if (m_retryTicks <= 0) {
            loadSelectedCape();
            if (!m_capeLoaded) m_retryTicks = kLoadRetryTicks;
        } else {
            --m_retryTicks;
        }
    }

    void* skin = resolvePlayerSkin(player);

    // RenderMode Overlay keeps the skin untouched (pure self-rendered cape);
    // any earlier patch from a previous mode is undone first.
    if (!enabled || m_selectedIndex <= 0 || !m_capeLoaded || !skin ||
        m_renderMode == RenderModeOverlay) {
        restoreOriginalCape(skin);
        return;
    }

    applyCustomCape(skin);
}

bool CustomCapesModule::applyCustomCape(void* skin) {
    if (*reinterpret_cast<bool*>(
            reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mIsPersona)) {
        return false;
    }

    // Layout sanity check on the verified skin-image offset: if the live
    // skin texture is not where we expect it, every derived offset
    // (mCapeImage, mCapeId) is unreliable too, so abort the patch.
    const uintptr_t skinImage =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mSkinImage;
    const uint32_t skinW = *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mWidth);
    const uint32_t skinH = *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mHeight);
    void* skinPx = *reinterpret_cast<void**>(skinImage + Image::mBytesOffset);
    const bool layoutOk = skinPx != nullptr &&
                          (skinW == 64 || skinW == 128) && (skinH == 64 || skinH == 128);
    if (!layoutOk) return false;

    // The cape pixels go into mCapeImage and the synthetic id into mCapeId —
    // never into mSkinImage (that is the player's skin texture; touching it
    // makes the game fall back to the default Steve skin).
    const uintptr_t capeImage =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mCapeImage;
    const uintptr_t capeIdAddr =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mCapeId;

    if (!isPlausibleCapeImage(capeImage)) {
        return false;
    }

    // Patch already in place and untouched? Nothing to do this tick.
    const bool idIntact = shortStdStringEquals(capeIdAddr, m_activeCapeId.c_str(),
                                               m_activeCapeId.size());
    void* const liveBlob = *reinterpret_cast<void**>(capeImage + Image::mBytesOffset);
    const bool liveUsesOurBlob = m_injectedBlob != nullptr && liveBlob == m_injectedBlob;

    if (m_patchedSkin == skin && m_hasBackup) {
        if (!m_needsApply && liveUsesOurBlob &&
            *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mWidth) == customcapes::kCapeWidth &&
            idIntact) {
            return true;
        }

        if (m_injectedBlob != nullptr && !liveUsesOurBlob) {
            // Skin changes can rebuild SerializedSkinImpl in-place: the pointer
            // remains equal, but the game has already detached or freed our old
            // blob and installed a new vanilla cape. Do not free the stale
            // pointer, and take a fresh backup of the new skin before patching.
            m_injectedBlob = nullptr;
            backupOriginalCape(capeImage, capeIdAddr);
            m_needsApply = true;
        }
    } else {
        // New skin object: back up its original cape so that "None"/disable
        // can bring the vanilla cape back before we patch anything. The
        // original pixel blob is only detached, never freed, so restoring
        // the raw pointer is safe.
        //
        // The engine owns the blob we injected into the previous skin object
        // and frees it through the deleter tag we set — do not free it here.
        m_patchedSkin = skin;
        m_injectedBlob = nullptr;
        backupOriginalCape(capeImage, capeIdAddr);
        m_needsApply = true;
    }

    const std::size_t bytes = m_pixels.size();
    void* newBlob = std::malloc(bytes);
    if (!newBlob) return false;
    std::memcpy(newBlob, m_pixels.data(), bytes);

    // Point the cape image at the new blob before releasing the previous
    // one so the skin never references freed memory. Only free the old blob
    // when the live skin still points at it; if the game changed skins in
    // place it may already have freed that pointer.
    void* previousBlob = liveUsesOurBlob ? m_injectedBlob : nullptr;

    // Describe a texture the engine can actually upload. It builds the cape
    // texture from this image's own fields, so every one of them has to say
    // "64x32 RGBA8Unorm(3), depth 1": a player without any cape carries a
    // default-constructed mCapeImage whose format/depth/usage are all 0, and
    // a depth-0 image has a computed size of w*h*0*4 = 0 bytes — the texture
    // factory drops it and the cape is silently never drawn. The format must
    // be the enum value 3 (RGBA8Unorm): 4 is past the end of the
    // {0,1,2,3} enum and the factory rejects it just as silently. Depth is
    // always 1 for a 2D texture; the image usage is inherited from the
    // player's own skin texture (an image the engine already renders)
    // whenever the cape image does not carry one of its own.
    const std::uint32_t skinUsage =
        *reinterpret_cast<const uint32_t*>(skinImage + Image::mUsage);
    const std::uint32_t capeUsage =
        *reinterpret_cast<const uint32_t*>(capeImage + Image::mUsage);

    *reinterpret_cast<uint32_t*>(capeImage + Image::mImageFormat) = kCapeImageFormat;
    *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mWidth) = customcapes::kCapeWidth;
    *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mHeight) = customcapes::kCapeHeight;
    *reinterpret_cast<uint32_t*>(capeImage + Image::mDepth) = kCapeImageDepth;
    if (capeUsage == 0 && skinUsage <= 4) {
        *reinterpret_cast<uint32_t*>(capeImage + Image::mUsage) = skinUsage;
    }
    *reinterpret_cast<void**>(capeImage + Image::mBytesOffset) = newBlob;
    *reinterpret_cast<void**>(capeImage + Image::mBlobDeleterOffset) =
        reinterpret_cast<void*>(&freeBlobDeleter);
    *reinterpret_cast<std::size_t*>(capeImage + Image::mBlobSizeOffset) = bytes;
    m_injectedBlob = newBlob;
    if (previousBlob != nullptr) std::free(previousBlob);

    writeShortStdString(capeIdAddr, m_activeCapeId.c_str(), m_activeCapeId.size());

    m_needsApply = false;
    return true;
}

void CustomCapesModule::restoreOriginalCape(void* skin) {
    if (!m_hasBackup || skin == nullptr || skin != m_patchedSkin) {
        // Either nothing to restore or the patched skin object is gone (the
        // engine destroyed it together with our injected blob).
        clearPatchState();
        return;
    }

    const uintptr_t capeImage =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mCapeImage;
    const uintptr_t capeIdAddr =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mCapeId;

    void* const liveBlob = *reinterpret_cast<void**>(capeImage + Image::mBytesOffset);
    if (m_injectedBlob != nullptr && liveBlob != m_injectedBlob) {
        // The game rebuilt the skin in-place before our disable/None restore
        // tick. Our backup belongs to the old appearance and our blob pointer
        // may already have been released by the engine, so restoring/freeing it
        // here would corrupt the new skin or double-free. Just detach state.
        clearPatchState();
        return;
    }

    // Put the vanilla cape back: dimensions, format, blob metadata, the
    // original pixel pointer and the original cape id. The blob pointer is
    // restored before our injected blob is freed, so the skin never
    // references freed memory.
    *reinterpret_cast<uint32_t*>(capeImage + Image::mImageFormat) = m_backup.format;
    *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mWidth) = m_backup.width;
    *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mHeight) = m_backup.height;
    *reinterpret_cast<uint32_t*>(capeImage + Image::mDepth) = m_backup.depth;
    *reinterpret_cast<uint32_t*>(capeImage + Image::mUsage) = m_backup.usage;
    *reinterpret_cast<void**>(capeImage + Image::mBlobDeleterOffset) = m_backup.deleter;
    *reinterpret_cast<std::size_t*>(capeImage + Image::mBlobSizeOffset) = m_backup.size;
    *reinterpret_cast<void**>(capeImage + Image::mBytesOffset) = m_backup.blob;
    std::memcpy(reinterpret_cast<void*>(capeIdAddr), m_backup.capeIdBytes,
                sizeof(m_backup.capeIdBytes));

    if (m_injectedBlob != nullptr) {
        std::free(m_injectedBlob);
        m_injectedBlob = nullptr;
    }

    clearPatchState();
}

void CustomCapesModule::onClientInstanceUpdate(void* clientInstance) {
    if (!clientInstance) return;
    // Only act when a cape is actually selected and the module is on. The
    // per-tick hook alone covers restores/teardown, so this per-frame hook
    // exists purely to land the patch BEFORE the engine builds the cape mesh
    // on the first frame after join. Gating it also keeps it from chasing a
    // stale LocalPlayer pointer while sitting in menus.
    {
        std::lock_guard<std::mutex> patchLock(m_patchMutex);
        if (!enabled || m_selectedIndex <= 0 || !m_capeLoaded) return;
    }

    // Same vtable dispatch shulkerpreview uses on-device:
    // vtable[ClientInstanceGetLocalPlayer] returns the LocalPlayer*. This
    // runs once per frame, before the level render pass, so on the first
    // frame after joining a world the cape patch lands in SerializedSkinImpl
    // before ActorRendererData is built from the skin.
    void** vtable = *reinterpret_cast<void***>(clientInstance);
    if (!vtable) return;
    const auto localPlayerFn = reinterpret_cast<void* (*)(void*)>(
        vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer]);
    if (!localPlayerFn) return;
    void* player = localPlayerFn(clientInstance);
    if (player) onLocalPlayerTick(player);
}

void CustomCapesModule::syncOverlayHook() {
    const bool needsHook = enabled && m_renderMode != RenderModeEngine;
    if (needsHook && !g_renderLevelHook) {
        const std::uintptr_t addr = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::RenderLevel);
        if (!addr) return;
        g_renderLevelHook = bedrocktools::hooks::install(
            reinterpret_cast<void*>(addr),
            reinterpret_cast<void*>(&renderLevelHook),
            reinterpret_cast<void**>(&g_renderLevelOrig));
    } else if (!needsHook && g_renderLevelHook) {
        bedrocktools::hooks::remove(g_renderLevelHook);
        g_renderLevelHook = nullptr;
        g_renderLevelOrig = nullptr;
    }
}

void CustomCapesModule::publishOverlayCape() {
    if (!m_capeLoaded) return;
    std::lock_guard<std::mutex> lock(g_renderMutex);
    g_capePixels = m_pixels;
    g_capeRevision = m_capeIdSerial;
    g_hasCapePixels = !g_capePixels.empty();
}


#include <bedrocktools/modules/visual/wings.hpp>
#include <bedrocktools/modules/visual/wings_default.hpp>

#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"
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
static WingBoneAngles s_boneAngles{};
static void* s_localPlayerPtr = nullptr;
static bool s_hasPlayer = false;

// --- Zero-latency extension: velocity + prediction ---
static float s_velX = 0.0f, s_velY = 0.0f, s_velZ = 0.0f;
static float s_hSpeed = 0.0f, s_vSpeed = 0.0f;
static std::chrono::steady_clock::time_point s_lastTickTime{};
static bool s_hasVelocity = false;
static bool s_lastTickTimeValid = false;

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
// Articulated 3D wings.
//
// The tables below mirror the bone hierarchy embedded in
// wings_default::GeometryJson (resources/wings/wings_geometry.json) one to
// one - same pivots, anchors and cube sizes, in Bedrock pixels (16 px = 1
// block). Bone chain (per side):
//
//   shoulder (root)
//   +-- upper
//   |   +-- feather_1
//   |   +-- feather_2
//   |   +-- tip
//   |       +-- feather_3
//   |       +-- feather_4
//
// Each bone rotates around the Z axis (the player's front-back axis) by the
// per-bone angles the animation controller produces: positive "raise" lifts
// the wing tip, right-side bones apply the negated angle (their span runs
// along -X) and left-side bones apply it directly. Cube z ranges are fixed
// by geometry and never rotated, so they are stored as absolute JSON z.
// ---------------------------------------------------------------------------

constexpr float kPxToBlocks = 1.0f / 16.0f;
constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

// Camera-inside-AABB tolerance for WingsModule::isThirdPersonCamera. In
// first-person the camera is inside the player's head (inside the collision
// box); the small margin absorbs jitter at the box edges. A third-person
// camera is pulled back well outside the box.
constexpr float kFirstPersonMargin = 0.05f;  // blocks

struct WingCornerPose {
    float c, s;    // 2D rotation
    float tx, ty;  // translation to absolute JSON xy (px)
};

struct WingBoneDef {
    int parent;                     // bone table index; -1 for the root (shoulder)
    float anchorX, anchorY;         // pivot offset relative to the parent pivot (px)
    float boxOX, boxOY;             // cube origin relative to the own pivot (px)
    float boxSX, boxSY;             // cube size on the wing plane (px)
    float zMin, zMax;               // cube z range, absolute JSON z (px); the
                                    // back of the body is at +Z, so the wings
                                    // live at z >= 2.5, behind the back
    const unsigned char* colOuter;  // zMax face (faces away from the body)
    const unsigned char* colInner;  // zMin face (faces the body)
    const unsigned char* colEdge;   // x faces and the yMax face
    const unsigned char* colBottom; // yMin face (feather tips get the highlight)
    int angleIndex;                 // which of the 7 animation angles (0=shoulder,
                                    // 1=upper, 2=tip, 3..6=feathers) drives this bone
};

// ---------------------------------------------------------------------------
// Selectable wing styles.
//
// Each style is a right-side bone table (spanning -X from the shoulder root
// pivot at [-3, 21]); the left-side table is generated at runtime by
// mirroring it (negate anchorX, mirror boxOX around the pivot) so the two
// sides always stay in sync. Colors are stored per-style so each shape can
// carry its own palette.
// ---------------------------------------------------------------------------

// ---- Dragon (default) palette - reuses the WingsModule constants so the
// world-space overlay keeps matching the embedded texture. ----
// (see WingsModule::kColorFrame / kColorMembraneOuter / ... )

// ---- Angel: white feathers with soft gold tips ----
static constexpr unsigned char kAngelFrame[3]         = { 244, 240, 232 };
static constexpr unsigned char kAngelMembraneOuter[3] = { 252, 250, 246 };
static constexpr unsigned char kAngelMembraneInner[3] = { 226, 222, 210 };
static constexpr unsigned char kAngelFeatherTip[3]    = { 255, 236, 190 };
static constexpr unsigned char kAngelJointInner[3]    = { 214, 205, 188 };

// ---- Demon: deep red membrane on a near-black frame ----
static constexpr unsigned char kDemonFrame[3]         = { 42, 12, 12 };
static constexpr unsigned char kDemonMembraneOuter[3] = { 96, 16, 22 };
static constexpr unsigned char kDemonMembraneInner[3] = { 150, 26, 32 };
static constexpr unsigned char kDemonFeatherTip[3]    = { 208, 44, 48 };
static constexpr unsigned char kDemonJointInner[3]    = { 64, 14, 16 };

// ---- Bat: small, very dark membrane ----
static constexpr unsigned char kBatFrame[3]         = { 34, 34, 40 };
static constexpr unsigned char kBatMembraneOuter[3] = { 14, 14, 18 };
static constexpr unsigned char kBatMembraneInner[3] = { 26, 26, 32 };
static constexpr unsigned char kBatFeatherTip[3]    = { 20, 20, 26 };
static constexpr unsigned char kBatJointInner[3]    = { 24, 24, 30 };

// ---- Butterfly: pink/orange panels with blue accents ----
static constexpr unsigned char kButterflyFrame[3]         = { 96, 44, 148 };
static constexpr unsigned char kButterflyMembraneOuter[3] = { 250, 118, 178 };
static constexpr unsigned char kButterflyMembraneInner[3] = { 255, 196, 92 };
static constexpr unsigned char kButterflyFeatherTip[3]    = { 118, 200, 255 };
static constexpr unsigned char kButterflyJointInner[3]    = { 150, 82, 192 };

// ---- Phoenix: fiery orange/red with gold highlights ----
static constexpr unsigned char kPhoenixFrame[3]         = { 176, 92, 22 };
static constexpr unsigned char kPhoenixMembraneOuter[3] = { 255, 62, 22 };
static constexpr unsigned char kPhoenixMembraneInner[3] = { 255, 168, 30 };
static constexpr unsigned char kPhoenixFeatherTip[3]    = { 255, 232, 120 };
static constexpr unsigned char kPhoenixJointInner[3]    = { 140, 60, 16 };

// ---- Fairy: small translucent cyan/pink wings ----
static constexpr unsigned char kFairyFrame[3]         = { 120, 222, 210 };
static constexpr unsigned char kFairyMembraneOuter[3] = { 186, 240, 255 };
static constexpr unsigned char kFairyMembraneInner[3] = { 244, 202, 255 };
static constexpr unsigned char kFairyFeatherTip[3]    = { 255, 255, 242 };
static constexpr unsigned char kFairyJointInner[3]    = { 140, 232, 222 };

// Z coordinates mirror resources/wings/wings_geometry.json: the body spans
// z in [-2, +2] (Bedrock model space faces -Z, so the cape/back side is
// +Z). Wing boxes sit at z = [2.5, 4.5] (shoulder joint) and [3.0, 4.0]
// (membranes/feathers) - fixed directly behind the back with a 0.5 px
// standoff from the back surface so nothing clips the torso or pokes
// through the chest/shoulders when the player is seen from the front.
static const WingBoneDef kDragonRightBones[7] = {
    { -1,  0.0f,  0.0f,  -1.5f, -1.5f, 3.0f, 3.0f,  2.5f,  4.5f, WingsModule::kColorFrame,         WingsModule::kColorJointInner,    WingsModule::kColorFrame, WingsModule::kColorFrame, 0 },
    {  0, -2.0f,  0.0f,  -6.0f, -1.5f, 6.0f, 3.0f,  3.0f,  4.0f, WingsModule::kColorMembraneOuter, WingsModule::kColorMembraneInner, WingsModule::kColorFrame, WingsModule::kColorFrame, 1 },
    {  1, -6.0f,  0.0f,  -5.0f, -1.0f, 5.0f, 2.0f,  3.0f,  4.0f, WingsModule::kColorMembraneOuter, WingsModule::kColorMembraneInner, WingsModule::kColorFrame, WingsModule::kColorFrame, 2 },
    {  1, -2.0f, -1.5f,  -1.0f, -6.0f, 2.0f, 6.0f,  3.0f,  4.0f, WingsModule::kColorMembraneOuter, WingsModule::kColorMembraneInner, WingsModule::kColorFrame, WingsModule::kColorFeatherTip, 3 },
    {  1, -4.5f, -1.5f,  -1.0f, -6.0f, 2.0f, 6.0f,  3.0f,  4.0f, WingsModule::kColorMembraneOuter, WingsModule::kColorMembraneInner, WingsModule::kColorFrame, WingsModule::kColorFeatherTip, 4 },
    {  2, -1.5f, -1.0f,  -1.0f, -6.0f, 2.0f, 6.0f,  3.0f,  4.0f, WingsModule::kColorMembraneOuter, WingsModule::kColorMembraneInner, WingsModule::kColorFrame, WingsModule::kColorFeatherTip, 5 },
    {  2, -4.0f, -1.0f,  -1.0f, -5.0f, 2.0f, 5.0f,  3.0f,  4.0f, WingsModule::kColorMembraneOuter, WingsModule::kColorMembraneInner, WingsModule::kColorFrame, WingsModule::kColorFeatherTip, 6 },
};

static const WingBoneDef kAngelRightBones[7] = {
    { -1,  0.0f,  0.0f,  -1.5f, -1.5f, 3.0f, 3.0f,  2.5f, 4.5f, kAngelFrame,         kAngelJointInner,    kAngelFrame, kAngelFrame, 0 },
    {  0, -2.0f,  0.0f,  -7.0f, -0.5f, 7.0f, 1.0f,  3.0f, 4.0f, kAngelFrame,         kAngelFrame,         kAngelFrame, kAngelFrame, 1 },
    {  1, -7.0f,  0.0f,  -1.0f, -5.0f, 2.0f, 6.0f,  3.0f, 4.0f, kAngelMembraneOuter, kAngelMembraneInner, kAngelFrame, kAngelFeatherTip, 2 },
    {  1, -6.0f, -1.0f,  -1.0f, -6.0f, 2.0f, 7.0f,  3.0f, 4.0f, kAngelMembraneOuter, kAngelMembraneInner, kAngelFrame, kAngelFeatherTip, 3 },
    {  1, -5.0f, -2.0f,  -1.0f, -7.0f, 2.0f, 8.0f,  3.0f, 4.0f, kAngelMembraneOuter, kAngelMembraneInner, kAngelFrame, kAngelFeatherTip, 4 },
    {  1, -4.0f, -2.5f,  -1.0f, -7.5f, 2.0f, 8.5f,  3.0f, 4.0f, kAngelMembraneOuter, kAngelMembraneInner, kAngelFrame, kAngelFeatherTip, 5 },
    {  1, -2.5f, -2.5f,  -1.0f, -7.0f, 2.0f, 8.0f,  3.0f, 4.0f, kAngelMembraneOuter, kAngelMembraneInner, kAngelFrame, kAngelFeatherTip, 6 },
};

static const WingBoneDef kDemonRightBones[7] = {
    { -1,  0.0f,  0.0f,  -1.5f, -1.5f, 3.0f, 3.0f,  2.5f, 4.5f, kDemonFrame,         kDemonJointInner,    kDemonFrame, kDemonFrame, 0 },
    {  0, -2.0f,  0.0f,  -7.0f, -1.5f, 7.0f, 3.0f,  3.0f, 4.0f, kDemonFrame,         kDemonFrame,         kDemonFrame, kDemonFrame, 1 },
    {  1, -5.0f, -1.5f,  -4.0f, -6.0f, 8.0f, 7.0f,  3.0f, 4.0f, kDemonMembraneOuter, kDemonMembraneInner, kDemonFrame, kDemonFrame, 2 },
    {  1, -2.0f, -1.5f,  -1.0f, -6.0f, 2.0f, 7.0f,  3.0f, 4.0f, kDemonMembraneOuter, kDemonMembraneInner, kDemonFrame, kDemonFeatherTip, 3 },
    {  1, -5.0f, -2.0f,  -1.0f, -5.0f, 2.0f, 6.0f,  3.0f, 4.0f, kDemonMembraneOuter, kDemonMembraneInner, kDemonFrame, kDemonFeatherTip, 4 },
    {  1, -3.0f,  1.5f,  -1.0f, -1.0f, 2.0f, 4.0f,  3.0f, 4.0f, kDemonMembraneOuter, kDemonMembraneInner, kDemonFrame, kDemonFeatherTip, 5 },
    {  1, -7.0f,  0.0f,  -3.0f, -0.5f, 3.0f, 1.5f,  3.0f, 4.0f, kDemonMembraneOuter, kDemonMembraneInner, kDemonFrame, kDemonFeatherTip, 6 },
};

static const WingBoneDef kBatRightBones[7] = {
    { -1,  0.0f,  0.0f,  -1.5f, -1.5f, 3.0f, 3.0f,  2.5f, 4.5f, kBatFrame,         kBatJointInner,    kBatFrame, kBatFrame, 0 },
    {  0, -2.0f,  0.0f,  -5.0f, -1.0f, 5.0f, 2.0f,  3.0f, 4.0f, kBatFrame,         kBatFrame,         kBatFrame, kBatFrame, 1 },
    {  1, -4.0f, -1.0f,  -3.0f, -4.0f, 7.0f, 5.0f,  3.0f, 4.0f, kBatMembraneOuter, kBatMembraneInner, kBatFrame, kBatFrame, 2 },
    {  2, -3.0f, -4.0f,  -1.0f, -2.0f, 2.0f, 3.0f,  3.0f, 4.0f, kBatMembraneOuter, kBatMembraneInner, kBatFrame, kBatFeatherTip, 3 },
    {  2, -5.0f, -3.0f,  -1.0f, -2.0f, 2.0f, 3.0f,  3.0f, 4.0f, kBatMembraneOuter, kBatMembraneInner, kBatFrame, kBatFeatherTip, 4 },
    {  2, -4.0f,  1.0f,  -1.0f, -1.0f, 2.0f, 3.0f,  3.0f, 4.0f, kBatMembraneOuter, kBatMembraneInner, kBatFrame, kBatFeatherTip, 5 },
    {  2, -6.0f,  0.0f,  -2.0f, -0.5f, 2.0f, 1.0f,  3.0f, 4.0f, kBatMembraneOuter, kBatMembraneInner, kBatFrame, kBatFrame, 6 },
};

static const WingBoneDef kButterflyRightBones[7] = {
    { -1,  0.0f,  0.0f,  -1.5f, -1.5f, 3.0f, 3.0f,  2.5f, 4.5f, kButterflyFrame,         kButterflyJointInner,    kButterflyFrame, kButterflyFrame, 0 },
    {  0, -2.0f,  0.0f,  -5.0f, -0.5f, 5.0f, 1.0f,  3.0f, 4.0f, kButterflyFrame,         kButterflyFrame,         kButterflyFrame, kButterflyFrame, 1 },
    {  1, -3.0f,  0.0f,  -3.0f,  0.0f, 6.0f, 6.0f,  3.0f, 4.0f, kButterflyMembraneOuter, kButterflyMembraneInner, kButterflyFrame, kButterflyFrame, 2 },
    {  1, -3.0f,  0.0f,  -2.0f, -6.0f, 5.0f, 6.0f,  3.0f, 4.0f, kButterflyMembraneOuter, kButterflyMembraneInner, kButterflyFrame, kButterflyFeatherTip, 3 },
    {  2, -3.0f,  5.0f,  -2.0f, -1.0f, 5.0f, 2.0f,  3.0f, 4.0f, kButterflyFeatherTip,   kButterflyMembraneInner, kButterflyFrame, kButterflyFeatherTip, 4 },
    {  3, -3.0f, -5.0f,  -2.0f, -1.0f, 5.0f, 2.0f,  3.0f, 4.0f, kButterflyFeatherTip,   kButterflyMembraneInner, kButterflyFrame, kButterflyFeatherTip, 5 },
    {  1, -2.0f, -0.5f,  -1.0f, -3.0f, 2.0f, 6.0f,  3.0f, 4.0f, kButterflyFrame,         kButterflyFrame,         kButterflyFrame, kButterflyFrame, 6 },
};

static const WingBoneDef kPhoenixRightBones[7] = {
    { -1,  0.0f,  0.0f,  -1.5f, -1.5f, 3.0f, 3.0f,  2.5f, 4.5f, kPhoenixFrame,         kPhoenixJointInner,    kPhoenixFrame, kPhoenixFrame, 0 },
    {  0, -2.0f,  0.0f,  -7.0f, -1.5f, 7.0f, 3.0f,  3.0f, 4.0f, kPhoenixFrame,         kPhoenixFrame,         kPhoenixFrame, kPhoenixFrame, 1 },
    {  1, -6.0f, -1.5f,  -3.0f, -6.0f, 6.0f, 8.0f,  3.0f, 4.0f, kPhoenixMembraneOuter, kPhoenixMembraneInner, kPhoenixFrame, kPhoenixFrame, 2 },
    {  1, -3.0f, -2.0f,  -1.0f, -8.0f, 2.0f, 9.0f,  3.0f, 4.0f, kPhoenixMembraneOuter, kPhoenixMembraneInner, kPhoenixFrame, kPhoenixFeatherTip, 3 },
    {  1, -5.0f, -2.5f,  -1.0f, -7.0f, 2.0f, 8.0f,  3.0f, 4.0f, kPhoenixMembraneOuter, kPhoenixMembraneInner, kPhoenixFrame, kPhoenixFeatherTip, 4 },
    {  1, -4.0f,  1.5f,  -1.0f, -1.0f, 2.0f, 5.0f,  3.0f, 4.0f, kPhoenixMembraneOuter, kPhoenixMembraneInner, kPhoenixFrame, kPhoenixFeatherTip, 5 },
    {  1, -7.0f,  0.0f,  -3.0f, -0.5f, 3.0f, 1.5f,  3.0f, 4.0f, kPhoenixMembraneOuter, kPhoenixMembraneInner, kPhoenixFrame, kPhoenixFrame, 6 },
};

static const WingBoneDef kFairyRightBones[7] = {
    { -1,  0.0f,  0.0f,  -1.5f, -1.5f, 3.0f, 3.0f,  2.5f, 4.5f, kFairyFrame,         kFairyJointInner,    kFairyFrame, kFairyFrame, 0 },
    {  0, -2.0f,  0.0f,  -4.0f, -0.5f, 4.0f, 1.0f,  3.0f, 4.0f, kFairyFrame,         kFairyFrame,         kFairyFrame, kFairyFrame, 1 },
    {  1, -3.0f,  0.0f,  -2.0f,  0.0f, 5.0f, 4.0f,  3.0f, 4.0f, kFairyMembraneOuter, kFairyMembraneInner, kFairyFrame, kFairyFrame, 2 },
    {  1, -2.5f,  0.0f,  -2.0f, -5.0f, 4.0f, 5.0f,  3.0f, 4.0f, kFairyMembraneOuter, kFairyMembraneInner, kFairyFrame, kFairyFeatherTip, 3 },
    {  2, -3.0f,  3.0f,  -1.0f, -1.0f, 2.0f, 2.0f,  3.0f, 4.0f, kFairyFeatherTip,   kFairyMembraneInner, kFairyFrame, kFairyFeatherTip, 4 },
    {  3, -3.0f, -4.0f,  -1.0f, -1.0f, 2.0f, 2.0f,  3.0f, 4.0f, kFairyFeatherTip,   kFairyMembraneInner, kFairyFrame, kFairyFeatherTip, 5 },
    {  1, -3.5f,  0.5f,  -1.0f, -1.0f, 2.0f, 2.0f,  3.0f, 4.0f, kFairyFeatherTip,   kFairyMembraneInner, kFairyFrame, kFairyFeatherTip, 6 },
};

struct WingStyle {
    const char* id;          // config id (radio option)
    const char* label;       // human-readable name (kept for future UI)
    int boneCount;           // number of bones per side
    const WingBoneDef* rightBones; // spans -X
};

static const WingStyle kWingStyleDefs[] = {
    { "dragon",    "Dragon",    7, kDragonRightBones },
    { "angel",     "Angel",     7, kAngelRightBones },
    { "demon",     "Demon",     7, kDemonRightBones },
    { "bat",       "Bat",       7, kBatRightBones },
    { "butterfly", "Butterfly", 7, kButterflyRightBones },
    { "phoenix",   "Phoenix",   7, kPhoenixRightBones },
    { "fairy",     "Fairy",     7, kFairyRightBones },
};

static constexpr int kWingStyleCount = static_cast<int>(sizeof(kWingStyleDefs) / sizeof(kWingStyleDefs[0]));
static constexpr int kMaxWingBones = 16;

// Left-side (spans +X) mirrors of each style's right-side table, generated
// once at runtime so the two sides can never drift apart.
static WingBoneDef s_leftBones[kWingStyleCount][kMaxWingBones];
static bool s_leftBonesReady = false;

static void mirrorWingBones(const WingBoneDef* right, int n, WingBoneDef* out) {
    for (int i = 0; i < n; ++i) {
        out[i].parent = right[i].parent;
        out[i].anchorX = -right[i].anchorX;
        out[i].anchorY = right[i].anchorY;
        out[i].boxOX = -(right[i].boxOX + right[i].boxSX);
        out[i].boxOY = right[i].boxOY;
        out[i].boxSX = right[i].boxSX;
        out[i].boxSY = right[i].boxSY;
        out[i].zMin = right[i].zMin;
        out[i].zMax = right[i].zMax;
        out[i].colOuter = right[i].colOuter;
        out[i].colInner = right[i].colInner;
        out[i].colEdge = right[i].colEdge;
        out[i].colBottom = right[i].colBottom;
        out[i].angleIndex = right[i].angleIndex;
    }
}

static void ensureWingStyleBones() {
    if (s_leftBonesReady) return;
    for (int s = 0; s < kWingStyleCount; ++s)
        mirrorWingBones(kWingStyleDefs[s].rightBones, kWingStyleDefs[s].boneCount, s_leftBones[s]);
    s_leftBonesReady = true;
}

static const WingBoneDef* leftBonesFor(int styleIndex) {
    if (styleIndex < 0 || styleIndex >= kWingStyleCount) styleIndex = 0;
    ensureWingStyleBones();
    return s_leftBones[styleIndex];
}

static int wingStyleIndexForId(const std::string& id) {
    for (int i = 0; i < kWingStyleCount; ++i)
        if (id == kWingStyleDefs[i].id) return i;
    return 0; // dragon default
}

// Serializes the picker value in the launcher's radio format:
// "<selectedIndex>,<id1>,<id2>,..." (same convention as Custom Capes).
static std::string wingStyleRadioValue(int index) {
    if (index < 0 || index >= kWingStyleCount) index = 0;
    std::string value = std::to_string(index);
    for (int i = 0; i < kWingStyleCount; ++i) {
        value += ',';
        value += kWingStyleDefs[i].id;
    }
    return value;
}

// Parses a value coming from the config file (full radio value) or from the
// launcher (just the numeric index when the selection changes) or a bare
// style id, and returns a valid style index.
static int resolveWingStyleIndex(const std::string& value) {
    if (value.empty()) return 0;

    const std::size_t comma = value.find(',');
    std::string head = value.substr(0, comma);

    bool numeric = !head.empty();
    for (char c : head) {
        if (!std::isdigit(static_cast<unsigned char>(c))) { numeric = false; break; }
    }
    if (numeric) {
        try {
            const int idx = std::stoi(head);
            if (idx >= 0 && idx < kWingStyleCount) return idx;
        } catch (...) {}
        return 0;
    }
    return wingStyleIndexForId(head);
}

static constexpr float kRightRootPivotX = -3.0f;
static constexpr float kLeftRootPivotX = 3.0f;
static constexpr float kRootPivotY = 21.0f;
static constexpr int kWingBoxFaces = 6;

static WingCornerPose composePose(const WingCornerPose& parent, float anchorX, float anchorY, float angleRad) {
    const float rc = std::cos(angleRad);
    const float rs = std::sin(angleRad);
    WingCornerPose out;
    out.c = parent.c * rc - parent.s * rs;
    out.s = parent.s * rc + parent.c * rs;
    out.tx = parent.c * anchorX - parent.s * anchorY + parent.tx;
    out.ty = parent.s * anchorX + parent.c * anchorY + parent.ty;
    return out;
}

static void emitFace(void* tess, const float corners[8][3], const int face[4], const unsigned char* color) {
    s_tessColor(tess, color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, 1.0f);
    for (int i = 0; i < 4; ++i) {
        const float* v = corners[face[i]];
        s_tessVertex(tess, v[0], v[1], v[2]);
    }
    // Double-sided: also emit the reversed winding.
    for (int i = 3; i >= 0; --i) {
        const float* v = corners[face[i]];
        s_tessVertex(tess, v[0], v[1], v[2]);
    }
}

static void emitWingBox(void* tess, const WingBoneDef& def, const WingCornerPose& pose,
                        float feetX, float feetY, float feetZ,
                        float rightX, float rightZ, float fwdX, float fwdZ,
                        float camX, float camY, float camZ) {
    // 2D cube corners on the wing plane (local -> absolute JSON xy). The 2D
    // rotation couples x and y, so the four plane corners are transformed as
    // points (bit 0 = x side, bit 1 = y side).
    const float lx[2] = { def.boxOX, def.boxOX + def.boxSX };
    const float ly[2] = { def.boxOY, def.boxOY + def.boxSY };
    float planeX[4], planeY[4];
    for (int i = 0; i < 4; ++i) {
        const float x = lx[i & 1];
        const float y = ly[(i >> 1) & 1];
        planeX[i] = pose.c * x - pose.s * y + pose.tx;
        planeY[i] = pose.s * x + pose.c * y + pose.ty;
    }

    // 8 world-space corners (camera relative). Bedrock model space faces
    // -Z (north), so the back/cape side is +Z: JSON axes map as x -> -right
    // (right side spans negative X), y -> up, z -> -forward (back is +Z).
    // Wing boxes therefore carry POSITIVE z (2.5..4.5 px, see the bone
    // tables) and land behind the player, never inside the chest.
    float corners[8][3];
    for (int i = 0; i < 8; ++i) {
        const float px = planeX[i & 3];
        const float py = planeY[i & 3];
        const float pz = ((i >> 2) & 1) ? def.zMax : def.zMin;
        corners[i][0] = feetX + rightX * (-px * kPxToBlocks) - fwdX * (pz * kPxToBlocks) - camX;
        corners[i][1] = feetY + py * kPxToBlocks - camY;
        corners[i][2] = feetZ + rightZ * (-px * kPxToBlocks) - fwdZ * (pz * kPxToBlocks) - camZ;
    }

    static const int kFaces[6][4] = {
        {0, 1, 2, 3}, // zMin (inner, faces the body)
        {4, 5, 6, 7}, // zMax (outer, faces away)
        {0, 3, 7, 4}, // xMin
        {1, 2, 6, 5}, // xMax
        {0, 1, 5, 4}, // yMin (bottom)
        {3, 2, 6, 7}, // yMax (top)
    };
    const unsigned char* faceColors[6] = {
        def.colInner, def.colOuter, def.colEdge, def.colEdge, def.colBottom, def.colEdge,
    };
    for (int f = 0; f < 6; ++f) emitFace(tess, corners, kFaces[f], faceColors[f]);
}

static void emitWing(void* tess, const WingBoneDef* bones, int boneCount, float rootPivotX,
                     const float anglesDeg[7], float angleSign,
                     float feetX, float feetY, float feetZ,
                     float rightX, float rightZ, float fwdX, float fwdZ,
                     float camX, float camY, float camZ) {
    WingCornerPose poses[kMaxWingBones];
    for (int i = 0; i < boneCount; ++i) {
        const WingBoneDef& def = bones[i];
        const int ai = (def.angleIndex >= 0 && def.angleIndex < 7) ? def.angleIndex : 0;
        const float angleRad = angleSign * anglesDeg[ai] * kDegToRad;
        if (def.parent < 0) {
            poses[i] = { std::cos(angleRad), std::sin(angleRad), rootPivotX, kRootPivotY };
        } else {
            poses[i] = composePose(poses[def.parent], def.anchorX, def.anchorY, angleRad);
        }
        emitWingBox(tess, def, poses[i], feetX, feetY, feetZ, rightX, rightZ, fwdX, fwdZ, camX, camY, camZ);
    }
}

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
    float velX = 0.0f, velY = 0.0f, velZ = 0.0f;
    std::chrono::steady_clock::time_point lastTick{};
    bool lastTickValid = false;
    bool hasVelocity = false;
    bool hasPlayer = false;

    {
        std::lock_guard<std::mutex> lock(s_stateMutex);
        if (!s_hasPlayer) return;
        hasPlayer = s_hasPlayer;
        aabb = s_playerAABB;
        rot = s_playerRot;
        angles = s_boneAngles;
        playerPtr = s_localPlayerPtr;
        velX = s_velX; velY = s_velY; velZ = s_velZ;
        lastTick = s_lastTickTime;
        lastTickValid = s_lastTickTimeValid;
        hasVelocity = s_hasVelocity;
    }

    if (!hasPlayer) return;

    // --- ULTRA-LOW LATENCY: fetch the actor anchor directly at render time ---
    // The AABB shape component is the same authoritative world-space anchor used
    // by the tick callback.  Once this read succeeds it is already the freshest
    // position available to the overlay.  Do not add velocity to it below: doing
    // so extrapolates an already-live sample and makes the wings jump ahead of
    // the body, most noticeably during a vertical jump.
    bool liveAABBValid = false;
    if (playerPtr && (std::uintptr_t)playerPtr >= 0x1000) {
        AABB liveAABB = getActorAABB(playerPtr);
        const float width = liveAABB.max.x - liveAABB.min.x;
        const float height = liveAABB.max.y - liveAABB.min.y;
        const float depth = liveAABB.max.z - liveAABB.min.z;
        const bool finite =
            std::isfinite(liveAABB.min.x) && std::isfinite(liveAABB.min.y) && std::isfinite(liveAABB.min.z) &&
            std::isfinite(liveAABB.max.x) && std::isfinite(liveAABB.max.y) && std::isfinite(liveAABB.max.z);
        liveAABBValid = finite && width > 0.0f && width < 16.0f &&
                        height > 0.0f && height < 16.0f &&
                        depth > 0.0f && depth < 16.0f;
        if (liveAABBValid) {
            aabb = liveAABB;
            bedrocktools::sdk::Vec2 liveRot = getActorRotation(playerPtr);
            if (std::isfinite(liveRot.x) && std::isfinite(liveRot.y)) {
                rot = liveRot;
            }
        }
    }

    // A live sample is preferred.  Velocity extrapolation is only a fallback
    // for the short window in which the actor component is unavailable; this
    // keeps the old last-tick fallback smooth without ever double-predicting a
    // valid sample (which used to detach the wings while jumping).
    const bool fallbackAABBValid =
        std::isfinite(aabb.min.x) && std::isfinite(aabb.min.y) && std::isfinite(aabb.min.z) &&
        std::isfinite(aabb.max.x) && std::isfinite(aabb.max.y) && std::isfinite(aabb.max.z) &&
        aabb.max.x > aabb.min.x && aabb.max.y > aabb.min.y && aabb.max.z > aabb.min.z;
    if (!liveAABBValid && !fallbackAABBValid) return;

    if (!liveAABBValid && lastTickValid && hasVelocity) {
        auto now = std::chrono::steady_clock::now();
        float dtSince = std::chrono::duration<float>(now - lastTick).count();
        if (dtSince < 0.0f) dtSince = 0.0f;
        if (dtSince > 0.12f) dtSince = 0.12f;
        if (dtSince > 0.001f) {
            // Only apply if velocity is reasonable (not teleport).
            const float velSq = velX * velX + velY * velY + velZ * velZ;
            if (velSq < 2500.0f) { // <50 blocks/s
                aabb.min.x += velX * dtSince;
                aabb.min.y += velY * dtSince;
                aabb.min.z += velZ * dtSince;
                aabb.max.x += velX * dtSince;
                aabb.max.y += velY * dtSince;
                aabb.max.z += velZ * dtSince;
            }
        }
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

    // Resolve the selected wing style (index is kept in sync with m_wingStyle
    // on the tick/config thread; reading an int here is safe for the render
    // thread).
    int styleIdx = 0;
    if (g_wings) styleIdx = g_wings->m_wingStyleIndex;
    if (styleIdx < 0 || styleIdx >= kWingStyleCount) styleIdx = 0;
    const WingStyle& style = kWingStyleDefs[styleIdx];

    // Per-bone, raise-positive angles in animation order:
    // [shoulder, upper, tip, feather_1, feather_2, feather_3, feather_4]
    const float wingAngles[7] = {
        angles.shoulderDeg, angles.upperDeg, angles.tipDeg,
        angles.featherDeg[0], angles.featherDeg[1], angles.featherDeg[2], angles.featherDeg[3],
    };

    char pad[0x58];
    std::memset(pad, 0, sizeof(pad));

    const int vertexCount = 2 * style.boneCount * kWingBoxFaces * 4 * 2; // both windings
    s_tessBegin(tess, nullptr, 1, vertexCount, 0); // 1 = quad

    // Right wing (span along -X, so a positive lift angle is a negative Z
    // rotation); left wing mirrors it.
    emitWing(tess, style.rightBones, style.boneCount, kRightRootPivotX, wingAngles, -1.0f,
             feetX, feetY, feetZ, rightX, rightZ, fwdX, fwdZ, camX, camY, camZ);
    emitWing(tess, leftBonesFor(styleIdx), style.boneCount, kLeftRootPivotX, wingAngles, 1.0f,
             feetX, feetY, feetZ, rightX, rightZ, fwdX, fwdZ, camX, camY, camZ);

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
    : Module("Wings", "Renders 3D articulated wings attached to your back, animated by your movement speed. Pick a shape in the Wing Style selector: Dragon, Angel, Demon, Bat, Butterfly, Phoenix or Fairy. Does not modify your skin.") {
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
    ensureWingStyleBones();

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
        s_velX = s_velY = s_velZ = 0.0f;
        s_hSpeed = s_vSpeed = 0.0f;
        s_hasVelocity = false;
        s_lastTickTimeValid = false;
    }
}

void WingsModule::onDisable() {
    // No skin to restore; just clear tracked player so overlay disappears immediately
    std::lock_guard<std::mutex> lock(s_stateMutex);
    s_hasPlayer = false;
    s_localPlayerPtr = nullptr;
    s_velX = s_velY = s_velZ = 0.0f;
    s_hasVelocity = false;
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
    return currentFlapAngleDegrees() * kDegToRad;
}

WingBoneAngles WingsModule::currentBoneAngles() const {
    WingBoneAngles out;
    const float phase = m_flapTime * kFlapBaseRate * m_flapSpeed;
    const float idlePh = m_flapTime * kIdleRate * m_flapSpeed;
    const float glidePh = m_flapTime * kGlideRate * m_flapSpeed;
    const float w = m_intensity;
    const float g = m_glide;

    // idle <-> flap blended pose (a wave travelling from the shoulder out to
    // the feathers, matching animation.wings.idle / animation.wings.flap).
    const float shoulderFlap = lerpFloat(kIdleBaseDegrees, kFlightBaseDegrees, w)
        + lerpFloat(kIdleAmplitudeDegrees * std::sin(idlePh),
                    kFlapAmplitudeDegrees * std::sin(phase), w);
    const float upperFlap = lerpFloat(6.0f, 0.0f, w)
        + lerpFloat(4.0f * std::sin(idlePh - kIdleUpperLag),
                    kFlightUpperAmplitudeDegrees * std::sin(phase - kUpperLag), w);
    const float tipFlap = lerpFloat(8.0f, 0.0f, w)
        + lerpFloat(3.0f * std::sin(idlePh - kIdleTipLag),
                    kFlightTipAmplitudeDegrees * std::sin(phase - kTipLag), w);

    // glide pose (animation.wings.glide): wings spread high, segments
    // straightened outward. Signs match the "raise positive" value the JSON
    // animations produce for the right-side bones.
    const float shoulderGlide = kGlideBaseDegrees + 3.0f * std::sin(glidePh);
    const float upperGlide = -15.0f + 3.0f * std::sin(glidePh - 0.5235988f); // -30 deg
    const float tipGlide = -15.0f + 2.0f * std::sin(glidePh - 1.0471976f);   // -60 deg

    out.shoulderDeg = lerpFloat(shoulderFlap, shoulderGlide, g);
    out.upperDeg = lerpFloat(upperFlap, upperGlide, g);
    out.tipDeg = lerpFloat(tipFlap, tipGlide, g);
    for (int i = 0; i < 4; ++i) {
        const float fi = static_cast<float>(i);
        const float featherFlap =
            lerpFloat(2.5f * std::sin(idlePh - kIdleFeatherLagBase - fi * kIdleFeatherLagStep),
                      kFlightFeatherAmplitudeDegrees * std::sin(phase - kFeatherLagBase - fi * kFeatherLagStep), w);
        const float featherGlide = 4.0f + 2.0f * std::sin(glidePh - 1.5707963f - fi * kIdleFeatherLagStep);
        out.featherDeg[i] = lerpFloat(featherFlap, featherGlide, g);
    }
    out.flapPhase = phase;
    out.intensity = w;
    out.glide = g;
    return out;
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

    WingBoneAngles out;
    const float phase = effectiveFlapTime * kFlapBaseRate * m_flapSpeed;
    const float idlePh = effectiveFlapTime * kIdleRate * m_flapSpeed;
    const float glidePh = effectiveFlapTime * kGlideRate * m_flapSpeed;
    const float w = m_intensity;
    const float g = m_glide;

    const float shoulderFlap = lerpFloat(kIdleBaseDegrees, kFlightBaseDegrees, w)
        + lerpFloat(kIdleAmplitudeDegrees * std::sin(idlePh),
                    kFlapAmplitudeDegrees * std::sin(phase), w);
    const float upperFlap = lerpFloat(6.0f, 0.0f, w)
        + lerpFloat(4.0f * std::sin(idlePh - kIdleUpperLag),
                    kFlightUpperAmplitudeDegrees * std::sin(phase - kUpperLag), w);
    const float tipFlap = lerpFloat(8.0f, 0.0f, w)
        + lerpFloat(3.0f * std::sin(idlePh - kIdleTipLag),
                    kFlightTipAmplitudeDegrees * std::sin(phase - kTipLag), w);

    const float shoulderGlide = kGlideBaseDegrees + 3.0f * std::sin(glidePh);
    const float upperGlide = -15.0f + 3.0f * std::sin(glidePh - 0.5235988f);
    const float tipGlide = -15.0f + 2.0f * std::sin(glidePh - 1.0471976f);

    out.shoulderDeg = lerpFloat(shoulderFlap, shoulderGlide, g);
    out.upperDeg = lerpFloat(upperFlap, upperGlide, g);
    out.tipDeg = lerpFloat(tipFlap, tipGlide, g);
    for (int i = 0; i < 4; ++i) {
        const float fi = static_cast<float>(i);
        const float featherFlap =
            lerpFloat(2.5f * std::sin(idlePh - kIdleFeatherLagBase - fi * kIdleFeatherLagStep),
                      kFlightFeatherAmplitudeDegrees * std::sin(phase - kFeatherLagBase - fi * kFeatherLagStep), w);
        const float featherGlide = 4.0f + 2.0f * std::sin(glidePh - 1.5707963f - fi * kIdleFeatherLagStep);
        out.featherDeg[i] = lerpFloat(featherFlap, featherGlide, g);
    }
    out.flapPhase = phase;
    out.intensity = w;
    out.glide = g;
    return out;
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
        WingBoneAngles angles = currentBoneAngles();
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
        s_velX = s_velY = s_velZ = 0.0f;
        s_hasVelocity = false;
        s_lastTickTimeValid = false;
        return;
    }

    if (!enabled) {
        std::lock_guard<std::mutex> lock(s_stateMutex);
        s_hasPlayer = false;
        s_localPlayerPtr = nullptr;
        s_velX = s_velY = s_velZ = 0.0f;
        s_hasVelocity = false;
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
    bedrocktools::sdk::Vec2 rot = getActorRotation(player);

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
        s_playerAABB = aabb;
        s_playerRot = rot;
        s_localPlayerPtr = player;
        s_hasPlayer = true;
        // Store velocity for prediction in render thread
        if (hasVel) {
            s_velX = velX; s_velY = velY; s_velZ = velZ;
            s_hSpeed = horizontalSpeed; s_vSpeed = verticalSpeed;
            s_hasVelocity = true;
        } else if (dt > 0.0f) {
            // If no velocity this tick, decay quickly to zero to avoid stale prediction
            s_velX *= 0.5f; s_velY *= 0.5f; s_velZ *= 0.5f;
            if (std::abs(s_velX) < 0.01f && std::abs(s_velY) < 0.01f && std::abs(s_velZ) < 0.01f) {
                s_hasVelocity = false;
            }
        }
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
        m_wingStyleIndex = resolveWingStyleIndex(j["m_wingStyle"].get<std::string>());
        m_wingStyle = kWingStyleDefs[m_wingStyleIndex].id;
    } else if (j.contains("wingStyle")) {
        m_wingStyleIndex = resolveWingStyleIndex(j["wingStyle"].get<std::string>());
        m_wingStyle = kWingStyleDefs[m_wingStyleIndex].id;
    }
}

void WingsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    float flapSpeed = std::clamp(m_flapSpeed, 0.1f, 10.0f);
    j["m_flapSpeed"] = flapSpeed;
    j["flapSpeed"] = flapSpeed; // keep both keys for compatibility
    m_wingStyleIndex = wingStyleIndexForId(m_wingStyle);
    j["m_wingStyle"] = wingStyleRadioValue(m_wingStyleIndex);
}

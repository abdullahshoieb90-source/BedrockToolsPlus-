#include "hitbox.hpp"
#include "hitbox_camera.hpp"
#include "hitbox_projection.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include "core/GameHooks.hpp"
#include <pl/ModMenu.hpp>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <cmath>
#include <span>
#include <string>
#include <cstring>
#include <vector>
#include <utility>

// The module logs one line per state change so "it does nothing" can be told
// apart from "nothing is nearby": which of the required game functions are
// missing, how many actors each source handed back, and how many boxes were
// drawn. Host tests compile this out.
#if defined(__ANDROID__)
#include <android/log.h>
#define HITBOX_LOG(...) __android_log_print(ANDROID_LOG_INFO, "BedrockToolsPlus", __VA_ARGS__)
#else
#define HITBOX_LOG(...) ((void)0)
#endif

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

// ActorManager::getRuntimeActorList() -- every actor in the level. Tablist and
// the Debug Menu already enumerate entities this way.
typedef std::vector<void*> (*ActorManager_getRuntimeActorList_t)(void* actorManager);

// Level::getHitResult() + HitResult::getEntity() -- what the crosshair is
// actually pointing at. The same pair the Crosshair module's indicator uses.
typedef void* (*Level_getHitResult_t)(void* level);
typedef void* (*HitResult_getEntity_t)(void* hitResult);

// BlockSource::isSolidBlockingBlock(BlockPos const&) -- true for full opaque
// blocks that block movement and sight (stone, dirt, planks...), false for
// transparent or partial blocks (glass, water, leaves, fences, slabs...).
struct BlockPosI {
    int x, y, z;
};
typedef bool (*BlockSource_isSolidBlockingBlock_t)(void* region, const BlockPosI& pos);

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

static uintptr_t resolveADRP(uint32_t* insns, size_t count, uint32_t targetReg) {
    for (size_t i = 0; i < count; i++) {
        uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;

        if ((insn & 0x9F000000) == 0x90000000) {
            uintptr_t page = ((uintptr_t)&insns[i] & ~0xFFFULL)
                           + ((int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29) & 3) << 43) >> 31);

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
            int64_t imm = (int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29)) << 43) >> 43;
            return (uintptr_t)&insns[i] + imm;
        }
    }
    return 0;
}

static HitboxModule* g_hitboxMod = nullptr;

static Tessellator_begin_t                s_tessBegin = nullptr;
static Tessellator_color_t                s_tessColor = nullptr;
static Tessellator_vertex_t               s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;

static Actor_isPlayer_t                   s_actorIsPlayer = nullptr;
static Actor_isInvisible_t                s_actorIsInvisible = nullptr;
static Actor_fetchNearbyActorsSorted_t    s_actorFetchNearby = nullptr;
static ActorManager_getRuntimeActorList_t s_getRuntimeActorList = nullptr;
static Level_getHitResult_t               s_levelGetHitResult = nullptr;
static HitResult_getEntity_t              s_hitResultGetEntity = nullptr;
static BlockSource_isSolidBlockingBlock_t s_isSolidBlockingBlock = nullptr;

static MaterialPtr s_matSelection;
static MaterialPtr s_matFill;
static uintptr_t    s_renderMaterialGroup = 0;

// Timestamp of the last frame the world-space pass actually drew something.
// The HUD fallback only takes over while this is stale: an overlay that is
// alive should not be drawn twice.
static int64_t s_lastWorldDrawUs = 0;

// The launcher validates a whole batch before drawing any of it and rejects one
// that is too long (kMaxDrawCommandCount = 4096 in its ModMenuBridge), so the
// fallback keeps a margin under that instead of losing every box at once.
static constexpr size_t kMaxSurfaceCommands = 3072;
// True while the module's HUD layer currently has commands on it, so a frame
// with nothing to draw submits an empty batch once (instead of every frame) to
// clear the last one.
static bool    s_surfaceDrawn = false;

static int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

static uint32_t forceOpaqueColor(uint32_t color) {
    return color | 0xFF000000u;
}

// Radius (blocks) around the camera a box is drawn for. The nearby-actor scan
// uses it as its extent, and the whole-level actor list is filtered by it so a
// busy world does not tessellate hundreds of distant boxes.
static constexpr float kActorRadius = 30.0f;

// A color with no RGB at all (pure black) draws an invisible box. That is what
// a launcher color picker leaves behind when it cannot parse the value it was
// given, and the module then looks dead. Such a value is treated as unset.
static uint32_t drawableColor(uint32_t color) {
    return (color & 0x00FFFFFFu) == 0 ? 0xFFFFFFFFu : forceOpaqueColor(color);
}

static void (*_renderLevel_orig)(void* _this, void* screenContext, void* a3);

static void* g_localPlayerPtr = nullptr;

// Options::getPlayerViewPerspective(): 0 = first person, 1 = third person
// back, 2 = third person front. Jumping in first person interpolates the
// camera above the tick AABB, so the geometric test alone used to flash
// the local player's own box. The game value is authoritative; hooks chain
// with ViewModel's detour on the same target.
static int (*_getPerspective_orig)(void*) = nullptr;
static int s_perspective = 0;
static bool s_perspectiveHooked = false;
static bool s_perspectiveKnown = false;

static int _getPerspective_hook(void* _this) {
    int result = 0;
    if (_getPerspective_orig)
        result = _getPerspective_orig(_this);
    s_perspective = result;
    s_perspectiveKnown = true;
    return result;
}

struct AABB {
    bedrocktools::sdk::Vec3 min;
    bedrocktools::sdk::Vec3 max;
};

// One actor as the HUD fallback needs it. Filled on the client tick, where the
// game is not mid-render, so a frame only has to project cached numbers.
struct CachedActor {
    AABB box{};
    bool isPlayer = false;
    bool isMob = false;
};

// What the fallback needs to draw a frame: where the eyes are, which way they
// look and one box per actor that passed the module's filters.
//
// The tick thread builds it and the render thread (onFrame, driven by the
// swap-buffers hook) reads it, so it is published under a lock: the writer only
// swaps two already-built objects in (O(1) - it never holds the lock while
// walking the game's actor list), and the reader copies the small result out
// and iterates its own copy. Nothing the game owns is touched while the lock
// is held.
struct FrameInputs {
    bool valid = false;      // a local player was found
    bool fromClient = false; // ... through ClientInstance rather than the tick
    bedrocktools::sdk::Vec3 eye{0.0f, 0.0f, 0.0f};
    bedrocktools::sdk::Vec2 rotation{0.0f, 0.0f};
    size_t nearbyCount = 0;
    size_t managerCount = 0;
    std::vector<CachedActor> actors;
};

static std::mutex s_frameInputsMutex;
static FrameInputs s_publishedInputs; // guarded by s_frameInputsMutex
static FrameInputs s_tickInputs;      // tick-thread scratch
static std::vector<hitboxhud::Segment> s_cachedSegments; // frame thread only

// Publishes what the tick thread just built and hands the previous list back
// for reuse.
static void publishFrameInputs(FrameInputs& built) {
    std::lock_guard<std::mutex> lock(s_frameInputsMutex);
    std::swap(s_publishedInputs, built);
}

static FrameInputs frameInputs() {
    FrameInputs copy;
    std::lock_guard<std::mutex> lock(s_frameInputsMutex);
    copy.valid = s_publishedInputs.valid;
    copy.fromClient = s_publishedInputs.fromClient;
    copy.eye = s_publishedInputs.eye;
    copy.rotation = s_publishedInputs.rotation;
    copy.nearbyCount = s_publishedInputs.nearbyCount;
    copy.managerCount = s_publishedInputs.managerCount;
    copy.actors = s_publishedInputs.actors;
    return copy;
}

// Only the published copy is dropped: the tick thread's own scratch is
// harmless (it is never read until a later tick publishes it) and is not
// touched here without the lock.
static void clearFrameInputs() {
    std::lock_guard<std::mutex> lock(s_frameInputsMutex);
    s_publishedInputs = FrameInputs{};
}

static bool hasCategory(void* actor, uint32_t categoryBit);

static void cacheFrameInputs(void* localPlayer, bool fromClient);

// ClientInstance::getLocalPlayer() through the vtable.
//
// The actor tick is the primary source of the player pointer, but it comes from
// a signature - and if that one does not resolve on a build, the fallback would
// have nothing to draw from. This route is version-independent (a vtable slot,
// not an offset or a pattern) and is what the HUD modules already use to find
// the player, so it is tried whenever no tick has delivered one.
static void* localPlayerFromClient() {
    void* client = bedrocktools::core::gamehooks::clientInstance();
    if (!client || (uintptr_t)client < 0x1000) return nullptr;

    void** vtable = *reinterpret_cast<void***>(client);
    if (!vtable) return nullptr;

    void* getLocalPlayer =
        vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer];
    if (!getLocalPlayer) return nullptr;

    void* player = reinterpret_cast<void* (*)(void*)>(getLocalPlayer)(client);
    if (!player || (uintptr_t)player < 0x1000) return nullptr;
    return player;
}

static void s_hitboxTickCallback(void* _this) {
    if (!g_hitboxMod || !g_hitboxMod->enabled) return;
    g_localPlayerPtr = _this;
    cacheFrameInputs(_this, false);
}

static MaterialPtr getMaterial(const char* name) {
    if (!s_renderMaterialGroup) return {};

    HashedString hs(name);

    void** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);
    if (!vtable || !vtable[2]) return {};

    using getMat_t = MaterialPtr(*)(void*, const HashedString*);
    return reinterpret_cast<getMat_t>(vtable[2])((void*)s_renderMaterialGroup, &hs);
}

// The game's own selection-overlay material, embedded in the player renderer.
// It is the fallback for builds where the material-group lookup returns
// nothing, and its offset differs between game builds - the header here says
// 0x1048 while upstream's says 0x1030. A wrong offset hands the renderer a
// pointer into unrelated memory, which draws nothing at all, so every known
// offset is probed and only a slot that looks like the two-pointer MaterialPtr
// of a shared_ptr (data + control block whose first word is a code pointer) is
// accepted. Guessing instead can hand the renderer garbage.
static void* embeddedOverlayMaterial(uintptr_t lrpPtr) {
    static const size_t kCandidates[] = {
        bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial,
        0x1030,
    };

    for (size_t offset : kCandidates) {
        void** slot = (void**)(lrpPtr + offset);
        uintptr_t data = (uintptr_t)slot[0];
        uintptr_t control = (uintptr_t)slot[1];
        if (data < 0x1000 || control < 0x1000) continue;
        if ((data & 0xF) != 0 || (control & 0xF) != 0) continue;
        const uintptr_t vtable = *(uintptr_t*)control;
        if (vtable < 0x1000 || (vtable & 0xF) != 0) continue;
        return (void*)slot;
    }
    return nullptr;
}

static void ensureMaterials() {
    if (!s_renderMaterialGroup) return;

    if (!s_matSelection) s_matSelection = getMaterial("selection_box");

    // Thick geometry is drawn as filled quads. The selection overlay
    // material is built for a translucent block highlight, so using it
    // makes the hitbox color look washed-out as soon as line thickness
    // goes above the hairline. Prefer a vertex-color fill instead.
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

static bool rayHitsAABB(float ox, float oy, float oz,
                        float dx, float dy, float dz,
                        const AABB& aabb,
                        float maxDist,
                        float& outDist) {
    float tmin = 0.0f;
    float tmax = maxDist;

    auto slab = [&](float origin, float dir, float mn, float mx) -> bool {
        if (fabsf(dir) < 1e-8f) {
            return origin >= mn && origin <= mx;
        }
        float inv = 1.0f / dir;
        float t1 = (mn - origin) * inv;
        float t2 = (mx - origin) * inv;
        if (t1 > t2) {
            float tmp = t1;
            t1 = t2;
            t2 = tmp;
        }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        return tmin <= tmax;
    };

    if (!slab(ox, dx, aabb.min.x, aabb.max.x)) return false;
    if (!slab(oy, dy, aabb.min.y, aabb.max.y)) return false;
    if (!slab(oz, dz, aabb.min.z, aabb.max.z)) return false;
    if (tmax < 0.0f) return false;
    outDist = tmin > 0.0f ? tmin : 0.0f;
    return true;
}

// Amanatides & Woo voxel traversal: walks every voxel the segment
// camera -> target passes through and returns true as soon as a solid
// blocking block is found. The camera's own voxel is never tested, so
// the check keeps working when the camera clips into geometry.
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

    // t is measured in units of the whole segment (the camera is t = 0 and the
    // sampled point is t = 1), so the walk below must stop at t = 1 and not at
    // the Euclidean length of the ray - the two only agree for a unit vector.
    constexpr float kSegmentEnd = 1.0f;

    if (dx > 0.0f)      { stepX = 1;  tDeltaX = 1.0f / dx;      tMaxX = (x + 1 - ox) * tDeltaX; }
    else if (dx < 0.0f) { stepX = -1; tDeltaX = -1.0f / dx;     tMaxX = (ox - x) * tDeltaX; }
    else                { stepX = 0;  tDeltaX = kInf;           tMaxX = kInf; }

    if (dy > 0.0f)      { stepY = 1;  tDeltaY = 1.0f / dy;      tMaxY = (y + 1 - oy) * tDeltaY; }
    else if (dy < 0.0f) { stepY = -1; tDeltaY = -1.0f / dy;     tMaxY = (oy - y) * tDeltaY; }
    else                { stepY = 0;  tDeltaY = kInf;           tMaxY = kInf; }

    if (dz > 0.0f)      { stepZ = 1;  tDeltaZ = 1.0f / dz;      tMaxZ = (z + 1 - oz) * tDeltaZ; }
    else if (dz < 0.0f) { stepZ = -1; tDeltaZ = -1.0f / dz;     tMaxZ = (oz - z) * tDeltaZ; }
    else                { stepZ = 0;  tDeltaZ = kInf;           tMaxZ = kInf; }

    // A segment crosses at most one voxel per block of length per axis, so this
    // bound is never reached by a legitimate ray. It only exists so a degenerate
    // camera or AABB (NaN, huge coordinate) cannot stall the render thread.
    int stepsRemaining = static_cast<int>(dist * 3.0f) + 8;

    while (stepsRemaining-- > 0) {
        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            x += stepX;
            if (tMaxX > kSegmentEnd) break;
            tMaxX += tDeltaX;
        } else if (tMaxY < tMaxZ) {
            y += stepY;
            if (tMaxY > kSegmentEnd) break;
            tMaxY += tDeltaY;
        } else {
            z += stepZ;
            if (tMaxZ > kSegmentEnd) break;
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

// True when every sampled point of the actor's box is hidden behind solid
// blocks. Samples the box center and the top-center, so a tall mob whose
// head pokes over a low wall stays visible while a fully hidden one is culled.
static bool isOccluded(void* region,
                       float camX, float camY, float camZ,
                       const AABB& aabb) {
    if (!region || !s_isSolidBlockingBlock) return false;

    const float cx = (aabb.min.x + aabb.max.x) * 0.5f;
    const float cz = (aabb.min.z + aabb.max.z) * 0.5f;
    const float cy = (aabb.min.y + aabb.max.y) * 0.5f;

    if (!rayHitsSolid(region, camX, camY, camZ, cx, cy, cz)) return false;
    if (!rayHitsSolid(region, camX, camY, camZ, cx, aabb.max.y, cz)) return false;
    return true;
}

static AABB getActorAABB(void* actor) {
    AABB aabb = {{0,0,0},{0,0,0}};
    uintptr_t actorAddr = (uintptr_t)actor;

    uintptr_t builtInPtr = *(uintptr_t*)(actorAddr + bedrocktools::sdk::offsets::Actor::mStateVectorComponent);
    if (builtInPtr) {
        uintptr_t aabbComponentPtr = *(uintptr_t*)(actorAddr + bedrocktools::sdk::offsets::Actor::mStateVectorComponent + bedrocktools::sdk::offsets::BuiltInActorComponents::mAABBShapeComponent);
        if (aabbComponentPtr) {
            aabb = *(AABB*)(aabbComponentPtr + bedrocktools::sdk::offsets::AABBShapeComponent::mAABB);
        }
    }

    return aabb;
}

static bedrocktools::sdk::Vec2 getActorRotation(void* actor) {
    bedrocktools::sdk::Vec2 rot = {0.f, 0.f};
    uintptr_t actorAddr = (uintptr_t)actor;
    uintptr_t rotComp = *(uintptr_t*)(actorAddr + bedrocktools::sdk::offsets::Actor::mActorRotationComponent);
    if (rotComp) {
        rot = *(bedrocktools::sdk::Vec2*)rotComp;
    }
    return rot;
}

static bool hasCategory(void* actor, uint32_t categoryBit) {
    uintptr_t actorAddr = (uintptr_t)actor;
    uint32_t categories = *(uint32_t*)(actorAddr + bedrocktools::sdk::offsets::Actor::mCategories);
    return (categories & categoryBit) != 0;
}

// A box the renderer can actually use. getActorAABB walks two component
// pointers, so a build where that chain is off returns garbage; a single NaN
// vertex poisons the whole tessellated mesh and no box is drawn at all.
static bool isUsableBox(const AABB& box) {
    if (!std::isfinite(box.min.x) || !std::isfinite(box.min.y) || !std::isfinite(box.min.z)) return false;
    if (!std::isfinite(box.max.x) || !std::isfinite(box.max.y) || !std::isfinite(box.max.z)) return false;
    const bool empty = box.min.x == 0.0f && box.min.y == 0.0f && box.min.z == 0.0f &&
                       box.max.x == 0.0f && box.max.y == 0.0f && box.max.z == 0.0f;
    return !empty;
}

static float distanceSqToBox(const AABB& box, float x, float y, float z) {
    const float dx = x < box.min.x ? box.min.x - x : (x > box.max.x ? x - box.max.x : 0.0f);
    const float dy = y < box.min.y ? box.min.y - y : (y > box.max.y ? y - box.max.y : 0.0f);
    const float dz = z < box.min.z ? box.min.z - z : (z > box.max.z ? z - box.max.z : 0.0f);
    return dx * dx + dy * dy + dz * dz;
}

// The entity the game says the crosshair is on, or nullptr. Straight from the
// level's stored hit result, exactly like the Crosshair module's indicator.
static void* aimedEntity(void* localPlayer) {
    if (!localPlayer || !s_levelGetHitResult || !s_hitResultGetEntity) return nullptr;

    uintptr_t level = *(uintptr_t*)((uintptr_t)localPlayer + bedrocktools::sdk::offsets::Actor::mLevel);
    if (level < 0x1000) return nullptr;

    void* hit = s_levelGetHitResult((void*)level);
    if (!hit || (uintptr_t)hit < 0x1000) return nullptr;

    const int type = *(int*)((uintptr_t)hit + bedrocktools::sdk::offsets::HitResult::mType);
    if (type != bedrocktools::sdk::offsets::HitResult::TypeEntity) return nullptr;

    void* entity = s_hitResultGetEntity(hit);
    if (!entity || entity == localPlayer) return nullptr;
    return entity;
}

// Collects the actors to box from every source the current build provides.
//
// The nearby-actor scan used to be the only source, and when that one
// signature does not resolve on a build the whole module draws nothing - the
// failure the module kept being reported with. The level's actor manager is
// queried as well (Tablist and the Debug Menu read the same list), and the
// aimed entity is always kept so the crosshair indicator works even when both
// lists come back empty.
static void collectActors(void* localPlayer, float camX, float camY, float camZ,
                          std::vector<void*>& out, size_t& managerCount, size_t& nearbyCount) {
    out.clear();
    managerCount = 0;
    nearbyCount = 0;

    auto append = [&](void* actor) {
        if (!actor || actor == localPlayer) return;
        if ((uintptr_t)actor < 0x1000) return;
        for (void* existing : out) {
            if (existing == actor) return;
        }
        const AABB box = getActorAABB(actor);
        if (!isUsableBox(box)) return;
        if (distanceSqToBox(box, camX, camY, camZ) > kActorRadius * kActorRadius) return;
        out.push_back(actor);
    };

    // 1) The nearby-actor scan, exactly what the original module does.
    if (s_actorFetchNearby) {
        bedrocktools::sdk::Vec3 extent = {kActorRadius, kActorRadius, kActorRadius};
        ActorVec actors = s_actorFetchNearby(localPlayer, &extent, 1);
        if (actors.begin && actors.end) {
            for (DistanceSortedActor* it = actors.begin; it < actors.end; ++it) {
                if (it->mActor) ++nearbyCount;
                append(it->mActor);
            }
        }
    }

    // 2) Nothing came back? Fall back to the level's actor manager, which is
    // how Tablist and the Debug Menu enumerate entities. This is what keeps
    // the module alive on a build where the nearby-actor signature does not
    // resolve: instead of drawing nothing at all, every actor in the level is
    // walked and the ones inside the radius are boxed.
    if (out.empty() && s_getRuntimeActorList) {
        uintptr_t level = *(uintptr_t*)((uintptr_t)localPlayer + bedrocktools::sdk::offsets::Actor::mLevel);
        if (level >= 0x1000) {
            uintptr_t manager = *(uintptr_t*)(level + bedrocktools::sdk::offsets::Level::mActorManager);
            if (manager >= 0x1000) {
                std::vector<void*> all = s_getRuntimeActorList((void*)manager);
                managerCount = all.size();
                for (void* actor : all) append(actor);
            }
        }
    }
}

// Caches what the HUD fallback needs for the frame it is drawn on: the eye
// position, the look angles and one entry per actor that passes the module's
// filters. Runs on the client tick, so no game state is read while the
// renderer is walking the same structures.
static void cacheFrameInputs(void* localPlayer, bool fromClient = false) {
    FrameInputs& built = s_tickInputs;
    built = FrameInputs{};
    if (!localPlayer || !g_hitboxMod) return;

    const AABB localBox = getActorAABB(localPlayer);
    if (!isUsableBox(localBox)) return;

    built.valid = true;
    built.fromClient = fromClient;

    // Eye height above the feet, the same 1.62 the original module uses.
    built.eye = {localBox.min.x + (localBox.max.x - localBox.min.x) * 0.5f,
                 localBox.min.y + 1.62f,
                 localBox.min.z + (localBox.max.z - localBox.min.z) * 0.5f};
    built.rotation = getActorRotation(localPlayer);

    std::vector<void*> actors;
    collectActors(localPlayer, built.eye.x, built.eye.y, built.eye.z,
                  actors, built.managerCount, built.nearbyCount);

    built.actors.reserve(actors.size());
    for (void* ent : actors) {
        if (!ent || ent == localPlayer) continue;
        if (g_hitboxMod->hideInvisible && s_actorIsInvisible && s_actorIsInvisible(ent)) continue;

        CachedActor cached;
        cached.isPlayer = s_actorIsPlayer && s_actorIsPlayer(ent);
        cached.isMob = hasCategory(ent, bedrocktools::sdk::offsets::ActorCategories::IsMob);

        if (cached.isPlayer) {
            if (!g_hitboxMod->showPlayers) continue;
        } else if (cached.isMob) {
            if (!g_hitboxMod->showEntities) continue;
        } else if (!g_hitboxMod->showItems) {
            continue;
        }

        cached.box = getActorAABB(ent);
        built.actors.push_back(cached);
    }

    // Nearest first: the fallback caps how many edges it submits per frame (the
    // launcher rejects a batch that is too long), so the boxes that survive the
    // cap should be the ones the player is closest to.
    std::sort(built.actors.begin(), built.actors.end(),
              [&](const CachedActor& a, const CachedActor& b) {
                  const float da = distanceSqToBox(a.box, built.eye.x, built.eye.y, built.eye.z);
                  const float db = distanceSqToBox(b.box, built.eye.x, built.eye.y, built.eye.z);
                  return da < db;
              });

    publishFrameInputs(built);
}

static void _renderLevel_hook(void* _this, void* screenContext, void* a3) {
    if (_renderLevel_orig) {
        _renderLevel_orig(_this, screenContext, a3);
    }

    if (!g_hitboxMod || !g_hitboxMod->enabled) return;
    if (!g_localPlayerPtr) return;

    // Reported once per state change: "the module does nothing" is otherwise
    // impossible to tell apart from "nothing is on screen".
    static int s_lastBail = -1;
    auto bail = [](int reason, const char* message) {
        (void)message; // only read by the Android log macro
        if (s_lastBail == reason) return;
        s_lastBail = reason;
        HITBOX_LOG("Hitbox: skipped - %s", message);
    };

    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) {
        bail(1, "tessellator helpers unresolved");
        return;
    }
    if (!screenContext || (uintptr_t)screenContext < 0x1000) return;

    uintptr_t tessellatorPtr = *(uintptr_t*)((uintptr_t)screenContext + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    if (!tessellatorPtr || tessellatorPtr < 0x1000) {
        bail(2, "screen context has no tessellator");
        return;
    }
    void* tessellator = (void*)tessellatorPtr;

    uintptr_t lrpPtr = *(uintptr_t*)((uintptr_t)_this + bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (!lrpPtr || lrpPtr < 0x1000) {
        bail(3, "level renderer has no player renderer");
        return;
    }

    float camX = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);
    float camY = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 4);
    float camZ = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 8);

    // The camera sits at the local player's eyes, or a few blocks behind them
    // in third person. A player-renderer layout that does not match this header
    // yields a camera somewhere else entirely, and every box is then translated
    // out of the world - which looks exactly like the module drawing nothing.
    // Those frames are skipped so the HUD fallback can take over instead of
    // silently painting the overlay into empty space.
    {
        const AABB localBox = getActorAABB(g_localPlayerPtr);
        constexpr float kCameraSlack = 12.0f;
        const bool finite = std::isfinite(camX) && std::isfinite(camY) && std::isfinite(camZ);
        const bool nearPlayer = isUsableBox(localBox) &&
            camX >= localBox.min.x - kCameraSlack && camX <= localBox.max.x + kCameraSlack &&
            camY >= localBox.min.y - kCameraSlack && camY <= localBox.max.y + kCameraSlack &&
            camZ >= localBox.min.z - kCameraSlack && camZ <= localBox.max.z + kCameraSlack;
        if (!finite || !nearPlayer) {
            bail(5, "camera position does not match the player renderer layout");
            return;
        }
    }

    // Wall occlusion: resolve the dimension's BlockSource once per frame. Only
    // done when the setting is on - the cull is opt-in so a build where the
    // isSolidBlockingBlock resolution is wrong cannot silently suppress every
    // hitbox in the game.
    void* region = nullptr;
    if (g_hitboxMod->hideBehindWalls && s_isSolidBlockingBlock) {
        uintptr_t dimension = *(uintptr_t*)((uintptr_t)g_localPlayerPtr + bedrocktools::sdk::offsets::Actor::mDimension);
        if (dimension >= 0x1000) {
            uintptr_t blockSource = *(uintptr_t*)(dimension + bedrocktools::sdk::offsets::Dimension::mBlockSource);
            if (blockSource >= 0x1000) region = (void*)blockSource;
        }
    }

    ensureMaterials();

    void* overlayMaterial = embeddedOverlayMaterial(lrpPtr);
    void* matInner = s_matSelection ? (void*)&s_matSelection : overlayMaterial;

    // Prefer an opaque vertex-color fill so raising line thickness keeps
    // the chosen RGB solid instead of inheriting the overlay's alpha.
    void* matFill = s_matFill ? (void*)&s_matFill : matInner;

    // Nothing usable to draw with: better an empty frame than handing the
    // renderer a pointer that is not a material.
    if (!matInner || !matFill) {
        bail(4, "no material resolved (material group and embedded overlay both unusable)");
        return;
    }

    uintptr_t colorHolderPtr = *(uintptr_t*)((uintptr_t)screenContext + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    if (!colorHolderPtr || colorHolderPtr < 0x1000) return;
    float* colorHolder = (float*)colorHolderPtr;

    float savedColor[4] = { colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3] };
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    // Menu thickness slider -> world-space half width. 1.0 (or lower) keeps
    // the classic hairline box; anything above is drawn as real geometry,
    // because GL line width is ignored by nearly every mobile GLES driver.
    float thicknessSetting = g_hitboxMod->lineThickness;
    if (thicknessSetting < 1.0f) thicknessSetting = 1.0f;
    if (thicknessSetting > 20.0f) thicknessSetting = 20.0f;
    const bool thickLines = thicknessSetting > 1.05f;
    const float halfWidth = thicknessSetting * 0.01f * 0.5f;

    auto drawLines = [&](const std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>>& lines, uint32_t color) {
        if (lines.empty()) return;
        float r = ((color >> 16) & 0xFF) / 255.0f;
        float g = ((color >>  8) & 0xFF) / 255.0f;
        float b = ((color      ) & 0xFF) / 255.0f;
        // Hitbox lines stay fully opaque at every thickness. The menu
        // color picker often stores #RRGGBB (alpha 0) or a low alpha,
        // which used to make thicker geometry look transparent.
        const float a = 1.0f;

        char pad[0x58];

        // Thick pass: every segment becomes a camera-facing quad, so the
        // apparent width follows the thickness setting from any angle.
        if (thickLines) {
            s_tessBegin(tessellator, nullptr, 1, static_cast<int>(lines.size() * 8), 0);
            s_tessColor(tessellator, r, g, b, a);

            for (const auto& line : lines) {
                bedrocktools::sdk::Vec3 p1 = line.first;
                bedrocktools::sdk::Vec3 p2 = line.second;
                p1.x -= camX; p1.y -= camY; p1.z -= camZ;
                p2.x -= camX; p2.y -= camY; p2.z -= camZ;

                float dx = p2.x - p1.x;
                float dy = p2.y - p1.y;
                float dz = p2.z - p1.z;
                float len = sqrtf(dx * dx + dy * dy + dz * dz);
                if (len < 1e-5f) continue;
                dx /= len; dy /= len; dz /= len;

                // The camera sits at the origin of this relative space, so
                // the vector to the segment midpoint is the view direction.
                float mx = (p1.x + p2.x) * 0.5f;
                float my = (p1.y + p2.y) * 0.5f;
                float mz = (p1.z + p2.z) * 0.5f;

                // side = dir x view, i.e. perpendicular to both the segment
                // and the eye ray => the quad always faces the player.
                float sx = dy * mz - dz * my;
                float sy = dz * mx - dx * mz;
                float sz = dx * my - dy * mx;
                float sLen = sqrtf(sx * sx + sy * sy + sz * sz);
                if (sLen < 1e-5f) {
                    // Looking straight down the segment: pick any perpendicular.
                    if (fabsf(dy) < 0.9f) { sx = -dz; sy = 0.0f; sz = dx; }
                    else { sx = 1.0f; sy = 0.0f; sz = 0.0f; }
                    sLen = sqrtf(sx * sx + sy * sy + sz * sz);
                    if (sLen < 1e-5f) continue;
                }
                sx = sx / sLen * halfWidth;
                sy = sy / sLen * halfWidth;
                sz = sz / sLen * halfWidth;

                // Overshoot both ends by half the width so corners stay solid.
                float ex = dx * halfWidth;
                float ey = dy * halfWidth;
                float ez = dz * halfWidth;

                bedrocktools::sdk::Vec3 quad[4] = {
                    {p1.x - ex - sx, p1.y - ey - sy, p1.z - ez - sz},
                    {p2.x + ex - sx, p2.y + ey - sy, p2.z + ez - sz},
                    {p2.x + ex + sx, p2.y + ey + sy, p2.z + ez + sz},
                    {p1.x - ex + sx, p1.y - ey + sy, p1.z - ez + sz}
                };

                // Emitted with both windings so back-face culling never
                // eats a segment.
                for (int i = 0; i < 4; ++i)
                    s_tessVertex(tessellator, quad[i].x, quad[i].y, quad[i].z);
                for (int i = 3; i >= 0; --i)
                    s_tessVertex(tessellator, quad[i].x, quad[i].y, quad[i].z);
            }

            memset(pad, 0, sizeof(pad));
            s_renderMesh(screenContext, tessellator, matFill, pad);
        }

        // Core hairline pass: keeps the edge crisp and visible even when the
        // quads shrink below a pixel at long range.
        s_tessBegin(tessellator, nullptr, 4, static_cast<int>(lines.size() * 2), 0);
        s_tessColor(tessellator, r, g, b, a);

        for (const auto& line : lines) {
            bedrocktools::sdk::Vec3 p1 = line.first;
            bedrocktools::sdk::Vec3 p2 = line.second;
            p1.x -= camX; p1.y -= camY; p1.z -= camZ;
            p2.x -= camX; p2.y -= camY; p2.z -= camZ;
            s_tessVertex(tessellator, p1.x, p1.y, p1.z);
            s_tessVertex(tessellator, p2.x, p2.y, p2.z);
        }

        memset(pad, 0, sizeof(pad));
        s_renderMesh(screenContext, tessellator, matInner, pad);
    };

    auto drawBox = [&](const AABB& aabb, uint32_t color) {
        std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> lines;
        bedrocktools::sdk::Vec3 mn = aabb.min;
        bedrocktools::sdk::Vec3 mx = aabb.max;

        lines.push_back({bedrocktools::sdk::Vec3{mn.x, mn.y, mn.z}, bedrocktools::sdk::Vec3{mx.x, mn.y, mn.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mx.x, mn.y, mn.z}, bedrocktools::sdk::Vec3{mx.x, mn.y, mx.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mx.x, mn.y, mx.z}, bedrocktools::sdk::Vec3{mn.x, mn.y, mx.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mn.x, mn.y, mx.z}, bedrocktools::sdk::Vec3{mn.x, mn.y, mn.z}});

        lines.push_back({bedrocktools::sdk::Vec3{mn.x, mx.y, mn.z}, bedrocktools::sdk::Vec3{mx.x, mx.y, mn.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mx.x, mx.y, mn.z}, bedrocktools::sdk::Vec3{mx.x, mx.y, mx.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mx.x, mx.y, mx.z}, bedrocktools::sdk::Vec3{mn.x, mx.y, mx.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mn.x, mx.y, mx.z}, bedrocktools::sdk::Vec3{mn.x, mx.y, mn.z}});

        lines.push_back({bedrocktools::sdk::Vec3{mn.x, mn.y, mn.z}, bedrocktools::sdk::Vec3{mn.x, mx.y, mn.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mx.x, mn.y, mn.z}, bedrocktools::sdk::Vec3{mx.x, mx.y, mn.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mx.x, mn.y, mx.z}, bedrocktools::sdk::Vec3{mx.x, mx.y, mx.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mn.x, mn.y, mx.z}, bedrocktools::sdk::Vec3{mn.x, mx.y, mx.z}});

        drawLines(lines, color);
    };

    // First-person: never draw the local player's own box (it fills the
    // view, and jumping interpolates the camera above the tick AABB so a
    // tight inside-AABB test used to flash it). Combine the game camera
    // mode with a jump-tolerant geometric test; see hitbox_camera.hpp.
    AABB localAabb = getActorAABB(g_localPlayerPtr);
    const bool cameraLooksThirdPerson = hitbox::isThirdPersonCamera(
        camX, camY, camZ,
        localAabb.min.x, localAabb.min.y, localAabb.min.z,
        localAabb.max.x, localAabb.max.y, localAabb.max.z);
    const bool gameThirdPerson = s_perspectiveKnown ? (s_perspective != 0) : true;

    // Actors to box, from the level's actor manager and from the nearby-actor
    // scan. Either one alone leaves the module empty on a build where its
    // signature does not resolve, so both are used and the results merged.
    static std::vector<void*> actors;
    size_t managerCount = 0;
    size_t nearbyCount = 0;
    collectActors(g_localPlayerPtr, camX, camY, camZ, actors, managerCount, nearbyCount);

    // The game's own answer for "what is under the crosshair" - the source the
    // Crosshair module's indicator already relies on. It also keeps working
    // when both actor lists come back empty.
    void* aimed = aimedEntity(g_localPlayerPtr);
    if (aimed) {
        bool known = false;
        for (void* actor : actors) {
            if (actor == aimed) {
                known = true;
                break;
            }
        }
        if (!known && isUsableBox(getActorAABB(aimed))) actors.push_back(aimed);
    }

    void* selectedEntity = nullptr;
    if (g_hitboxMod->hitboxIndicator) {
        if (aimed) {
            selectedEntity = aimed;
        } else if (!actors.empty()) {
            bedrocktools::sdk::Vec2 lookRot = getActorRotation(g_localPlayerPtr);
            static constexpr float kPi = 3.14159265f;
            static constexpr float kDegToRad = kPi / 180.0f;
            const float yawR = lookRot.y * kDegToRad;
            const float pitchR = lookRot.x * kDegToRad;
            const float lookX = -sinf(yawR) * cosf(pitchR);
            const float lookY = -sinf(pitchR);
            const float lookZ = cosf(yawR) * cosf(pitchR);

            // No hit result: fall back to the nearest actor along the look
            // ray, limited to a short arm's reach so a distant entity the
            // crosshair happens to touch is not highlighted as if it were the
            // one being aimed at.
            constexpr float kSelectionRayLength = 3.0f;

            float bestDist = 1e9f;
            for (void* ent : actors) {
                if (!ent || ent == g_localPlayerPtr) continue;
                AABB aabb = getActorAABB(ent);
                float hitDist = 0.0f;
                if (!rayHitsAABB(camX, camY, camZ, lookX, lookY, lookZ, aabb, kSelectionRayLength, hitDist)) continue;
                if (hitDist < bestDist) {
                    bestDist = hitDist;
                    selectedEntity = ent;
                }
            }
        }
    }

    auto renderActor = [&](void* ent, uint32_t groupColor, bool skipOcclusion = false) {
        AABB aabb = getActorAABB(ent);
        if (aabb.min.x == 0.f && aabb.min.y == 0.f && aabb.min.z == 0.f &&
            aabb.max.x == 0.f && aabb.max.y == 0.f && aabb.max.z == 0.f) return;

        // With Hide Behind Walls on, cull hitboxes fully hidden behind solid
        // blocks instead of drawing them through walls. Skips the eye/look
        // lines too, since they belong to the same box. The local player's own
        // box is never culled: a third-person camera often sits inside or
        // against a wall, which would otherwise hide the player's own hitbox.
        if (!skipOcclusion && region && isOccluded(region, camX, camY, camZ, aabb)) return;

        uint32_t boxColor = drawableColor(groupColor);
        if (g_hitboxMod->hitboxIndicator) {
            // The indicator is active for the entity currently under the
            // crosshair. Every other nearby entity keeps the default
            // indicator color.
            boxColor = drawableColor(g_hitboxMod->indicatorDefaultColor);
            if (ent == selectedEntity) {
                boxColor = drawableColor(g_hitboxMod->indicatorActiveColor);
            }
        }

        drawBox(aabb, boxColor);

        if (g_hitboxMod->showEyeLine) {
            float minX = aabb.min.x;
            float maxX = aabb.max.x;
            float minZ = aabb.min.z;
            float maxZ = aabb.max.z;

            float entityHeight = aabb.max.y - aabb.min.y;
            float eyeHeight = aabb.min.y + entityHeight * 0.85f;

            std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> eyeLines;
            eyeLines.push_back({bedrocktools::sdk::Vec3{minX, eyeHeight, minZ}, bedrocktools::sdk::Vec3{maxX, eyeHeight, minZ}});
            eyeLines.push_back({bedrocktools::sdk::Vec3{maxX, eyeHeight, minZ}, bedrocktools::sdk::Vec3{maxX, eyeHeight, maxZ}});
            eyeLines.push_back({bedrocktools::sdk::Vec3{maxX, eyeHeight, maxZ}, bedrocktools::sdk::Vec3{minX, eyeHeight, maxZ}});
            eyeLines.push_back({bedrocktools::sdk::Vec3{minX, eyeHeight, maxZ}, bedrocktools::sdk::Vec3{minX, eyeHeight, minZ}});
            drawLines(eyeLines, drawableColor(g_hitboxMod->eyeLineColor));
        }

        if (g_hitboxMod->showLookLine) {
            bedrocktools::sdk::Vec2 rot = getActorRotation(ent);
            static constexpr float PI = 3.14159265f;
            static constexpr float DEG_TO_RAD = PI / 180.0f;

            float yawR = rot.y * DEG_TO_RAD;
            float pitchR = rot.x * DEG_TO_RAD;
            float dirX = -sinf(yawR) * cosf(pitchR);
            float dirY = -sinf(pitchR);
            float dirZ = cosf(yawR) * cosf(pitchR);

            float entityHeight = aabb.max.y - aabb.min.y;
            float eyeHeight = aabb.min.y + entityHeight * 0.85f;
            float centerX = (aabb.min.x + aabb.max.x) * 0.5f;
            float centerZ = (aabb.min.z + aabb.max.z) * 0.5f;

            bedrocktools::sdk::Vec3 start = {centerX, eyeHeight, centerZ};
            float lineLen = g_hitboxMod->lookLineLength;
            bedrocktools::sdk::Vec3 end = {start.x + dirX * lineLen, start.y + dirY * lineLen, start.z + dirZ * lineLen};

            std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> lookLines;
            lookLines.push_back({start, end});
            drawLines(lookLines, drawableColor(g_hitboxMod->lookLineColor));
        }
    };

    if (hitbox::shouldDrawLocalHitbox(g_hitboxMod->show3rdPerson, gameThirdPerson, cameraLooksThirdPerson)) {
        renderActor(g_localPlayerPtr, drawableColor(g_hitboxMod->hitboxColor), true);
    }

    size_t drawn = 0;
    for (void* ent : actors) {
        if (!ent || ent == g_localPlayerPtr) continue;

        // Invisible actors are boxed by default: this is a hitbox overlay, and
        // on a build where the invisibility query mis-resolves, hiding them
        // hides every box in the game.
        if (g_hitboxMod->hideInvisible && s_actorIsInvisible && s_actorIsInvisible(ent)) continue;

        bool isPlayer = false;
        if (s_actorIsPlayer) {
            isPlayer = s_actorIsPlayer(ent);
        }

        uint32_t groupColor = g_hitboxMod->hitboxColor;
        if (isPlayer) {
            if (!g_hitboxMod->showPlayers) continue;
        } else if (hasCategory(ent, bedrocktools::sdk::offsets::ActorCategories::IsMob)) {
            if (!g_hitboxMod->showEntities) continue;
        } else {
            if (!g_hitboxMod->showItems) continue;
            groupColor = g_hitboxMod->showItemsColor;
        }

        renderActor(ent, groupColor);
        ++drawn;
    }

    // Tell the HUD fallback the world pass is alive: it stays out of the way
    // while this keeps running.
    if (drawn > 0 || aimed) {
        s_lastWorldDrawUs = nowUs();
    }

    // One line per frame while the state is unchanged, so a log tells whether
    // actors were found at all and whether they survived the filters.
    static size_t s_lastManager = size_t(-1);
    static size_t s_lastNearby = size_t(-1);
    static size_t s_lastDrawn = size_t(-1);
    if (managerCount != s_lastManager || nearbyCount != s_lastNearby || drawn != s_lastDrawn) {
        s_lastManager = managerCount;
        s_lastNearby = nearbyCount;
        s_lastDrawn = drawn;
        HITBOX_LOG("Hitbox: actor manager %zu (fallback), nearby %zu, boxes %zu (aimed %s)",
                   managerCount, nearbyCount, drawn, aimed ? "yes" : "no");
    }

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

HitboxModule::HitboxModule()
    : Module("Hitbox", "Displays hitboxes of entities.") {

    showInMenu = true;

    // World overlay, not a HUD element.
    hideInHudEditor = true;

    m_patched = false;
    m_patchTarget = nullptr;
    m_tessBeginAddr = nullptr;
    m_tessColorAddr = nullptr;
    m_tessVertexAddr = nullptr;
    m_renderMaterialGroupAddr = nullptr;
    g_hitboxMod = this;
}

HitboxModule::~HitboxModule() {
    if (g_hitboxMod == this) g_hitboxMod = nullptr;
}

void HitboxModule::onInit() {
    uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    if (addr != 0) {
        m_patchTarget = (void*)addr;
    }

    uintptr_t tb = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin);
    if (tb) { m_tessBeginAddr = (void*)tb; s_tessBegin = (Tessellator_begin_t)tb; }

    uintptr_t tc = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor);
    if (tc) { m_tessColorAddr = (void*)tc; s_tessColor = (Tessellator_color_t)tc; }

    uintptr_t tv = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex);
    if (tv) { m_tessVertexAddr = (void*)tv; s_tessVertex = (Tessellator_vertex_t)tv; }

    uintptr_t rm = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if (rm) {
        s_renderMesh = (MeshHelpers_renderMeshImmediately_t)rm;
    } else {
        uintptr_t rm5 = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);
        if (rm5) s_renderMesh = (MeshHelpers_renderMeshImmediately_t)rm5;
    }

    uintptr_t rmg = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
    if (rmg) {
        m_renderMaterialGroupAddr = (void*)rmg;
        uintptr_t groupAddr = resolveADRP(reinterpret_cast<uint32_t*>(rmg), 2, 0);
        if (groupAddr) {
            s_renderMaterialGroup = groupAddr + bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
        }
    }

    uintptr_t aip = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer);
    if (aip) s_actorIsPlayer = (Actor_isPlayer_t)aip;

    uintptr_t aii = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsInvisible);
    if (aii) s_actorIsInvisible = (Actor_isInvisible_t)aii;

    uintptr_t afn = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted);
    if (afn) s_actorFetchNearby = (Actor_fetchNearbyActorsSorted_t)afn;

    // Actor enumeration and the crosshair hit result. Kept optional: the
    // module uses every source that resolves and logs what it has.
    uintptr_t aml = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorManagerList);
    if (aml) s_getRuntimeActorList = (ActorManager_getRuntimeActorList_t)aml;

    uintptr_t lhr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelGetHitResult);
    if (lhr) s_levelGetHitResult = (Level_getHitResult_t)lhr;

    uintptr_t hrge = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::HitResultGetEntity);
    if (hrge) s_hitResultGetEntity = (HitResult_getEntity_t)hrge;

    HITBOX_LOG("Hitbox: init - tess %d, material group %d, actor manager %d, nearby %d, hit result %d",
               s_tessVertex != nullptr, s_renderMaterialGroup != 0,
               s_getRuntimeActorList != nullptr, s_actorFetchNearby != nullptr,
               s_levelGetHitResult != nullptr && s_hitResultGetEntity != nullptr);

    uintptr_t isb = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock);
    if (isb) s_isSolidBlockingBlock = (BlockSource_isSolidBlockingBlock_t)isb;

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) { s_hitboxTickCallback(event.player); });

    if (!s_perspectiveHooked) {
        uintptr_t perspective = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::GetPerspective);
        if (perspective != 0 &&
            bedrocktools::hooks::install((void*)perspective, (void*)_getPerspective_hook, (void**)&_getPerspective_orig)) {
            s_perspectiveHooked = true;
        }
    }
}

void HitboxModule::applyPatch() {
    if (m_patched) return;
    if (!m_patchTarget) {
        // The signature table is resolved before the modules are initialized,
        // but re-check here so a module that was enabled before the game
        // library was scanned still hooks on a later toggle instead of staying
        // silently dead for the rest of the session.
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
        if (addr != 0) m_patchTarget = (void*)addr;
    }
    if (!m_patchTarget) return;
    auto handle = bedrocktools::hooks::install(m_patchTarget, (void*)_renderLevel_hook, (void**)&_renderLevel_orig);
    // Only claim the patch when the hook really is in. Marking it patched on
    // failure would stop every later onEnable() from retrying, which is how a
    // failed install used to leave the overlay permanently off.
    m_patched = handle != nullptr;
}

void HitboxModule::onEnable() {
    applyPatch();
}

void HitboxModule::onDisable() {
    // Drop the cached player. It points into the world that is going away, and
    // the render hook dereferences it (AABB, dimension, rotation) - keeping it
    // would read freed memory after leaving a world.
    g_localPlayerPtr = nullptr;
    clearFrameInputs();
    s_surfaceDrawn = false;
    // Leave no stale overlay behind: onFrame stops running for a disabled
    // module, so this is the only chance to clear the HUD layer.
    pl::modmenu::submitDrawCommands(moduleId, std::span<const pl::modmenu::DrawCommand>{});
}

// The on-screen status readout (hudDiagnostics). Two short lines on the top
// left of the HUD surface: which pass is drawing, and what the actor sources
// and the resolved game functions look like. Written for a device with no
// adb available - it turns "the module does nothing" into a sentence that can
// be read straight off the screen.
static void appendDiagnostics(std::vector<pl::modmenu::DrawCommand>& commands, bool worldAlive,
                              bool fallbackActive, bool capped, size_t drawnBoxes,
                              const FrameInputs& inputs) {
    std::string headline;
    if (worldAlive) {
        headline = "Hitbox: world pass live";
    } else if (fallbackActive) {
        headline = "Hitbox: HUD fallback, " + std::to_string(drawnBoxes) + " box(es)";
        if (capped) headline += " (capped, nearest first)";
    } else if (!g_hitboxMod || !g_hitboxMod->hudFallback) {
        headline = "Hitbox: world pass silent, HUD fallback off";
    } else {
        headline = "Hitbox: nothing drawn";
    }

    std::string detail = "actors: scan ";
    detail += s_actorFetchNearby ? "ok" : "n/a";
    detail += ", level list " + std::to_string(inputs.managerCount);
    detail += ", boxed " + std::to_string(inputs.actors.size());
    if (!inputs.valid) {
        detail += " (no player yet: tick and client instance both silent)";
    } else {
        detail += inputs.fromClient ? " (player via client)" : " (player via tick)";
        if (inputs.actors.empty()) detail += " - nothing in range";
    }
    detail += " | fns: box ";
    detail += s_hitResultGetEntity ? "ok" : "n/a";
    detail += ", list ";
    detail += s_getRuntimeActorList ? "ok" : "n/a";

    auto text = [](float y, uint32_t color, std::string message) {
        pl::modmenu::DrawCommand command{};
        command.type = pl::modmenu::DrawCommandType::Text;
        command.x = 8.0f;
        command.y = y;
        command.w = -1.0f; // natural width, left-aligned
        command.color = color;
        command.size = 18.0f;
        command.text = std::move(message);
        return command;
    };

    commands.push_back(text(8.0f, 0xFFFFFFFFu, std::move(headline)));
    commands.push_back(text(30.0f, 0xFFB0BEC5u, std::move(detail)));
}

// The HUD fallback: the same boxes, projected onto the launcher's overlay.
//
// Used while the world-space pass has not drawn for a while - the state a
// build lands in when the level-renderer signatures or the player-renderer
// layout do not match the ones this header was written for. The launcher HUD
// layer does not go through that pass, so the overlay stays usable; the boxes
// are drawn over terrain, since the HUD has no depth buffer.
//
// Everything is built into one command list and submitted once, so turning the
// fallback off (or having nothing to show) removes the previous frame's boxes
// instead of leaving them on screen.
void HitboxModule::onFrame() {
    constexpr int64_t kWorldGraceUs = 2 * 1000 * 1000; // 2 s, ~120 frames
    // This frame's own copy of what the tick thread cached; iterating a copy is
    // what keeps the render thread from walking a list the tick thread is
    // rebuilding.
    FrameInputs inputs = frameInputs();
    const bool worldAlive =
        s_lastWorldDrawUs != 0 && (nowUs() - s_lastWorldDrawUs) <= kWorldGraceUs;

    // No tick ever handed us a player? Ask the client instance instead. Walking
    // the level is not free, so this runs every few frames, and only while the
    // world pass is not drawing either.
    if (hudFallback && !worldAlive && !inputs.valid) {
        constexpr int kClientProbeFrames = 15; // ~4 probes a second at 60 fps
        static int s_framesSinceProbe = kClientProbeFrames;
        if (s_framesSinceProbe >= kClientProbeFrames) {
            s_framesSinceProbe = 0;
            if (void* player = localPlayerFromClient()) {
                cacheFrameInputs(player, true);
                inputs = frameInputs();
            }
        } else {
            ++s_framesSinceProbe;
        }
    }

    const bool fallbackActive =
        hudFallback && !worldAlive && inputs.valid && !inputs.actors.empty();

    std::vector<pl::modmenu::DrawCommand> commands;
    size_t drawnBoxes = 0;
    bool capped = false;

    if (fallbackActive) {
        const pl::modmenu::HudSurfaceSize surface = pl::modmenu::getHudSurfaceSize();
        if (surface.width <= 1.0f || surface.height <= 1.0f) return;

        const hitboxhud::Camera camera = hitboxhud::computeCamera(inputs.eye, inputs.rotation);
        const hitboxhud::Projection projection =
            hitboxhud::makeProjection(surface.width, surface.height, hudFov);
        // Keeps an entity straddling the camera plane from producing
        // coordinates in the tens of thousands.
        const float limit =
            (surface.width > surface.height ? surface.width : surface.height) * 4.0f;

        const float thickness = lineThickness > 1.05f ? lineThickness : 1.5f;

        commands.reserve(std::min<size_t>(inputs.actors.size() * hitboxhud::kBoxEdgeCount,
                                          kMaxSurfaceCommands));

        for (const CachedActor& actor : inputs.actors) {
            if (commands.size() >= kMaxSurfaceCommands) {
                capped = true;
                break;
            }

            const uint32_t color = drawableColor(actor.isPlayer || actor.isMob
                                                     ? hitboxColor
                                                     : showItemsColor);

            s_cachedSegments.clear();
            hitboxhud::projectBox(camera, projection, actor.box.min, actor.box.max,
                                  s_cachedSegments, limit);

            size_t edges = 0;
            for (const hitboxhud::Segment& segment : s_cachedSegments) {
                if (commands.size() >= kMaxSurfaceCommands) {
                    capped = true;
                    break;
                }
                // Off-screen edges still cost a command, so they are dropped.
                if ((segment.x1 < 0.0f && segment.x2 < 0.0f) ||
                    (segment.x1 > surface.width && segment.x2 > surface.width) ||
                    (segment.y1 < 0.0f && segment.y2 < 0.0f) ||
                    (segment.y1 > surface.height && segment.y2 > surface.height)) {
                    continue;
                }

                const float w = segment.x2 - segment.x1; // the launcher reads w/h as the end delta
                const float h = segment.y2 - segment.y1;
                // One bad coordinate makes the launcher drop the whole batch
                // (it validates every command before drawing any), so a
                // segment that is not fully finite never leaves the module.
                if (!std::isfinite(segment.x1) || !std::isfinite(segment.y1) ||
                    !std::isfinite(w) || !std::isfinite(h)) {
                    continue;
                }

                pl::modmenu::DrawCommand command{};
                command.type = pl::modmenu::DrawCommandType::Line;
                command.x = segment.x1;
                command.y = segment.y1;
                command.w = w;
                command.h = h;
                command.size = thickness;
                command.color = color;
                commands.push_back(std::move(command));
                ++edges;
            }
            if (edges != 0) ++drawnBoxes;
        }
    }

    if (hudDiagnostics) {
        appendDiagnostics(commands, worldAlive, fallbackActive, capped, drawnBoxes, inputs);
    }

    // Drawing nothing on a layer that already shows nothing: leave it alone.
    if (commands.empty() && !s_surfaceDrawn) return;

    pl::modmenu::submitDrawCommands(moduleId, commands);
    s_surfaceDrawn = !commands.empty();
}

void HitboxModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    showEntities = j.value("showEntities", showEntities);
    showPlayers = j.value("showPlayers", showPlayers);
    showItems = j.value("showItems", showItems);
    // Prefer the current key; fall back to the old "showSelf" name so
    // existing configs keep working after the rename.
    if (j.contains("show3rdPerson")) {
        show3rdPerson = j.value("show3rdPerson", show3rdPerson);
    } else if (j.contains("showSelf")) {
        show3rdPerson = j.value("showSelf", show3rdPerson);
    }
    showEyeLine = j.value("showEyeLine", showEyeLine);
    showLookLine = j.value("showLookLine", showLookLine);
    lookLineLength = j.value("lookLineLength", lookLineLength);

    if (j.contains("lineThickness")) {
        try { lineThickness = j["lineThickness"].get<float>(); } catch (...) {}
    }
    if (lineThickness < 1.0f) lineThickness = 1.0f;
    if (lineThickness > 20.0f) lineThickness = 20.0f;

    if (j.contains("hitboxIndicator")) {
        hitboxIndicator = j["hitboxIndicator"].get<bool>();
    }
    // Older configs have no such key; the cull used to be hardcoded on, but
    // the default is now off so the overlay is guaranteed to draw.
    hideBehindWalls = j.value("hideBehindWalls", hideBehindWalls);
    hideInvisible = j.value("hideInvisible", hideInvisible);
    hudFallback = j.value("hudFallback", hudFallback);
    hudDiagnostics = j.value("hudDiagnostics", hudDiagnostics);
    hudFov = j.value("hudFov", hudFov);
    if (hudFov < 30.0f) hudFov = 30.0f;
    if (hudFov > 120.0f) hudFov = 120.0f;
    auto parseColor = [&](const std::string& key, uint32_t& outColor) {
        if (!j.contains(key) || !j[key].is_string()) return;
        std::string hexStr = j[key].get<std::string>();
        if (hexStr.empty()) return;
        if (hexStr[0] == '#') hexStr = hexStr.substr(1);
        else if (hexStr.size() > 1 && hexStr[0] == '0' && (hexStr[1] == 'x' || hexStr[1] == 'X')) hexStr = hexStr.substr(2);
        try {
            unsigned long parsed = std::stoul(hexStr, nullptr, 16);
            if (hexStr.size() <= 6) {
                // #RRGGBB from the color picker has no alpha byte.
                outColor = 0xFF000000u | static_cast<uint32_t>(parsed);
            } else {
                // Keep RGB, drop any stored transparency.
                outColor = forceOpaqueColor(static_cast<uint32_t>(parsed));
            }
        } catch (...) {}
    };

    parseColor("hitboxColor", hitboxColor);
    parseColor("showItemsColor", showItemsColor);
    parseColor("eyeLineColor", eyeLineColor);
    parseColor("lookLineColor", lookLineColor);
    parseColor("indicatorDefaultColor", indicatorDefaultColor);
    parseColor("indicatorActiveColor", indicatorActiveColor);
}

void HitboxModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["showEntities"] = showEntities;
    j["showPlayers"] = showPlayers;
    j["showItems"] = showItems;
    j["show3rdPerson"] = show3rdPerson;
    j["showEyeLine"] = showEyeLine;
    j["showLookLine"] = showLookLine;
    j["lookLineLength"] = lookLineLength;
    j["lineThickness"] = lineThickness;
    j["hitboxIndicator"] = hitboxIndicator;
    j["hideBehindWalls"] = hideBehindWalls;
    j["hideInvisible"] = hideInvisible;
    j["hudFallback"] = hudFallback;
    j["hudDiagnostics"] = hudDiagnostics;
    j["hudFov"] = hudFov;

    // Colors are written the way every other module in the mod writes them:
    // "#RRGGBB". The launcher builds its color picker straight from this
    // string, and the old "#AARRGGBB" form was the odd one out - the picker
    // could not show the current value. Alpha is forced opaque at draw time
    // anyway, so nothing is lost by dropping the byte here.
    char hexH[12], hexI[12], hexE[12], hexL[12], hexD[12], hexA[12];
    snprintf(hexH, sizeof(hexH), "#%06X", hitboxColor & 0x00FFFFFFu);
    snprintf(hexI, sizeof(hexI), "#%06X", showItemsColor & 0x00FFFFFFu);
    snprintf(hexE, sizeof(hexE), "#%06X", eyeLineColor & 0x00FFFFFFu);
    snprintf(hexL, sizeof(hexL), "#%06X", lookLineColor & 0x00FFFFFFu);
    snprintf(hexD, sizeof(hexD), "#%06X", indicatorDefaultColor & 0x00FFFFFFu);
    snprintf(hexA, sizeof(hexA), "#%06X", indicatorActiveColor & 0x00FFFFFFu);

    j["hitboxColor"] = std::string(hexH);
    // Without this the item color was never written out, so the menu had no
    // entry for it and every unrelated settings change silently reset it to
    // white (the menu round-trips the whole config through save -> load).
    j["showItemsColor"] = std::string(hexI);
    j["eyeLineColor"] = std::string(hexE);
    j["lookLineColor"] = std::string(hexL);
    j["indicatorDefaultColor"] = std::string(hexD);
    j["indicatorActiveColor"] = std::string(hexA);
}

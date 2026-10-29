#include "killaura.hpp"
#include "killaura_logic.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using bedrocktools::sdk::AABB;
using bedrocktools::sdk::Vec2;
using bedrocktools::sdk::Vec3;
namespace offsets = bedrocktools::sdk::offsets;
using SignatureId = bedrocktools::memory::SignatureId;

// GameMode::attack(Actor&, Vec3 const&). On the arm64 ABI the reference
// parameters arrive as plain pointers.
using AttackVirtualFn = bool (*)(void* gameMode, void* target, const Vec3* hitPosition);
// LocalPlayer::swing(ActorSwingSource, HandSlot). HandSlot rides in its own
// register, so passing it is harmless if a build's swing takes only the
// source (the older 2-argument form this SDK's signature was taken from).
using PlayerSwingFn = bool (*)(void* player, std::uint8_t source, std::uint32_t handSlot);
using ActorIsPlayerFn = bool (*)(void* actor);
using ActorIsInvisibleFn = bool (*)(void* actor);

// Same fetch ABI the Hitbox overlay uses (Actor::fetchNearbyActorsSorted).
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
using ActorFetchNearbyFn = ActorVec (*)(void* actor, void* extent, int actorType);

struct BlockPosI {
    int x, y, z;
};
using IsSolidBlockingFn = bool (*)(void* region, const BlockPosI& pos);

ActorIsPlayerFn s_actorIsPlayer = nullptr;
ActorIsInvisibleFn s_actorIsInvisible = nullptr;
ActorFetchNearbyFn s_fetchNearby = nullptr;
IsSolidBlockingFn s_isSolidBlockingBlock = nullptr;
PlayerSwingFn s_playerSwing = nullptr;

// Resolved addresses of the two attack implementations. They are the
// fingerprints used to recognize a real GameMode object below.
std::uintptr_t s_gameModeAttackAddr = 0;
std::uintptr_t s_survivalModeAttackAddr = 0;

// Last GameMode seen in the game's own action events, and the local player
// it belonged to (attack/use/place all refresh this, even while the module
// is disabled, so the fallback path is ready the moment it is enabled).
void* s_localPlayer = nullptr;
void* s_capturedGameMode = nullptr;
void* s_capturedGameModePlayer = nullptr;

// Container/chat screens currently open. Attacking is paused while the
// player is in a menu; the GameMode object also tends to be recreated
// around screen changes, so this doubles as a stale-pointer guard.
int s_openScreens = 0;

// Slot of GameMode::attack in the vtable (counted with the two destructor
// slots first; the core GameHooks code uses the same slot to tell a
// SurvivalMode instance from a plain GameMode).
constexpr std::size_t kAttackVtableSlot = 16;

// ActorSwingSource::Attack, so the swing is reported exactly like a manual
// attack swing (keystrokes/Hit Sound observers see no difference).
constexpr std::uint8_t kSwingSourceAttack = 4;

// Player::mGameMode, derived from the verified mSkin offset. In the game's
// Player layout the members `mGameMode`, `mSpawnRandomizer` and `mSkin`
// are three consecutive std::unique_ptrs, so the GameMode slot is two
// pointers before mSkin - independent of any STL size differences between
// platforms. The value read here is never trusted blindly: it must pass
// the vtable fingerprint in looksLikeGameMode() before any call, and when
// it does not, the event-captured pointer above is used instead.
constexpr std::size_t kPlayerGameModeOffset = offsets::Player::mSkin - 2 * sizeof(void*);

// A Multi Attack burst cap: beyond a dense mob farm's worth of targets the
// remaining ones wait for the next interval instead of stacking tens of
// transactions into a single tick.
constexpr std::size_t kMaxMultiTargets = 16;

bool isPlausiblePointer(const void* pointer) {
    // Strip an arm64 top-byte tag (MTE/TBI) before range/alignment checks;
    // the original value is still what gets dereferenced.
    const auto address = reinterpret_cast<std::uintptr_t>(pointer) & 0x00FFFFFFFFFFFFFFULL;
    return address >= 0x1000 && (address & 0x7) == 0;
}

AABB getActorAABB(void* actor) {
    AABB aabb{{0, 0, 0}, {0, 0, 0}};
    const auto actorAddr = reinterpret_cast<std::uintptr_t>(actor);
    const auto builtIn = *reinterpret_cast<const std::uintptr_t*>(
        actorAddr + offsets::Actor::mStateVectorComponent);
    if (!builtIn) return aabb;
    const auto shape = *reinterpret_cast<const std::uintptr_t*>(
        actorAddr + offsets::Actor::mStateVectorComponent +
        offsets::BuiltInActorComponents::mAABBShapeComponent);
    if (shape) {
        aabb = *reinterpret_cast<const AABB*>(shape + offsets::AABBShapeComponent::mAABB);
    }
    return aabb;
}

bool hasCategory(void* actor, std::uint32_t categoryBit) {
    const auto categories = *reinterpret_cast<const std::uint32_t*>(
        reinterpret_cast<std::uintptr_t>(actor) + offsets::Actor::mCategories);
    return (categories & categoryBit) != 0;
}

// The rotation component's first Vec2 is the {pitch, yaw} pair the Hitbox
// look line reads. Writing it is the visible "client" rotation: the next
// auth-input packet reports it to the server like a real mouse flick.
bool getActorRotation(void* actor, Vec2& out) {
    const auto component = *reinterpret_cast<const std::uintptr_t*>(
        reinterpret_cast<std::uintptr_t>(actor) + offsets::Actor::mActorRotationComponent);
    if (!isPlausiblePointer(reinterpret_cast<const void*>(component))) return false;
    out = *reinterpret_cast<const Vec2*>(component);
    return std::isfinite(out.x) && std::isfinite(out.y);
}

void setActorRotation(void* actor, float pitch, float yaw) {
    const auto component = *reinterpret_cast<const std::uintptr_t*>(
        reinterpret_cast<std::uintptr_t>(actor) + offsets::Actor::mActorRotationComponent);
    if (!isPlausiblePointer(reinterpret_cast<const void*>(component))) return;
    *reinterpret_cast<Vec2*>(component) = Vec2{pitch, yaw};
}

// True when the object at `gameMode` really is a GameMode/SurvivalMode:
// its attack virtual (slot 16) must be one of the two signature-resolved
// attack implementations. A wrong offset reading some other member of
// Player (a vector, a Vec3, another unique_ptr) essentially never survives
// this check, which is what makes the derived offset safe to try.
bool looksLikeGameMode(void* gameMode, AttackVirtualFn& outAttack) {
    outAttack = nullptr;
    if (!isPlausiblePointer(gameMode)) return false;
    if (!s_gameModeAttackAddr && !s_survivalModeAttackAddr) return false;

    auto** vtable = *reinterpret_cast<void***>(gameMode);
    if (!isPlausiblePointer(vtable)) return false;

    const auto attack = reinterpret_cast<std::uintptr_t>(vtable[kAttackVtableSlot]);
    if (attack != s_gameModeAttackAddr && attack != s_survivalModeAttackAddr) return false;
    if (!attack) return false;

    outAttack = reinterpret_cast<AttackVirtualFn>(attack);
    return true;
}

struct ResolvedGameMode {
    void* instance = nullptr;
    AttackVirtualFn attack = nullptr;
};

ResolvedGameMode resolveGameMode(void* player) {
    ResolvedGameMode resolved;

    // Primary: the derived unique_ptr slot is always current - it follows
    // gamemode switches (survival <-> creative recreates the object).
    void* derived = *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(player) + kPlayerGameModeOffset);
    AttackVirtualFn attack = nullptr;
    if (looksLikeGameMode(derived, attack)) {
        resolved.instance = derived;
        resolved.attack = attack;
        return resolved;
    }

    // Fallback: the pointer captured from this player's own game actions.
    if (s_capturedGameModePlayer == player &&
        looksLikeGameMode(s_capturedGameMode, attack)) {
        resolved.instance = s_capturedGameMode;
        resolved.attack = attack;
    }
    return resolved;
}

// --- Wall occlusion (Only Visible Targets) ----------------------------------
// Amanatides & Woo voxel traversal, same as the Hitbox overlay's culling:
// walks every voxel between the eyes and the sample point and reports the
// first solid blocking block.
bool rayHitsSolid(void* region, const Vec3& from, const Vec3& to) {
    if (!region || !s_isSolidBlockingBlock) return false;

    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float dz = to.z - from.z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 0.01f) return false;

    int x = static_cast<int>(std::floor(from.x));
    int y = static_cast<int>(std::floor(from.y));
    int z = static_cast<int>(std::floor(from.z));
    const int ex = static_cast<int>(std::floor(to.x));
    const int ey = static_cast<int>(std::floor(to.y));
    const int ez = static_cast<int>(std::floor(to.z));
    if (x == ex && y == ey && z == ez) return false;

    constexpr float kInf = 1e30f;
    int stepX = 0, stepY = 0, stepZ = 0;
    float tMaxX = kInf, tMaxY = kInf, tMaxZ = kInf;
    float tDeltaX = kInf, tDeltaY = kInf, tDeltaZ = kInf;

    if (dx > 0.0f)      { stepX = 1;  tDeltaX = 1.0f / dx;  tMaxX = (x + 1 - from.x) * tDeltaX; }
    else if (dx < 0.0f) { stepX = -1; tDeltaX = -1.0f / dx; tMaxX = (from.x - x) * tDeltaX; }
    if (dy > 0.0f)      { stepY = 1;  tDeltaY = 1.0f / dy;  tMaxY = (y + 1 - from.y) * tDeltaY; }
    else if (dy < 0.0f) { stepY = -1; tDeltaY = -1.0f / dy; tMaxY = (from.y - y) * tDeltaY; }
    if (dz > 0.0f)      { stepZ = 1;  tDeltaZ = 1.0f / dz;  tMaxZ = (z + 1 - from.z) * tDeltaZ; }
    else if (dz < 0.0f) { stepZ = -1; tDeltaZ = -1.0f / dz; tMaxZ = (from.z - z) * tDeltaZ; }

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
        // The target's own voxel never counts as blocking.
        if (x == ex && y == ey && z == ez) break;
        if (s_isSolidBlockingBlock(region, BlockPosI{x, y, z})) return true;
    }
    return false;
}

// Occluded when both the box center and its top-center are hidden, so a
// target whose head pokes over a low wall is still attackable.
bool isOccluded(void* region, const Vec3& eye, const AABB& box) {
    if (!region || !s_isSolidBlockingBlock) return false;
    const Vec3 center = killaura::boxCenter(box);
    if (!rayHitsSolid(region, eye, center)) return false;
    const Vec3 top{center.x, box.max.y, center.z};
    return rayHitsSolid(region, eye, top);
}

void* blockSourceFor(void* player) {
    const auto dimension = *reinterpret_cast<const std::uintptr_t*>(
        reinterpret_cast<std::uintptr_t>(player) + offsets::Actor::mDimension);
    if (!isPlausiblePointer(reinterpret_cast<const void*>(dimension))) return nullptr;
    const auto source = *reinterpret_cast<const std::uintptr_t*>(
        dimension + offsets::Dimension::mBlockSource);
    return isPlausiblePointer(reinterpret_cast<const void*>(source))
               ? reinterpret_cast<void*>(source)
               : nullptr;
}

struct Target {
    void* actor;
    AABB box;
    float reach; // eyes -> closest point of the box
};

} // namespace

KillAuraModule* g_killAura = nullptr;

KillAuraModule::KillAuraModule()
    : Module("Kill Aura",
             "Automatically attacks nearby players and mobs. Adjustable range, CPS and rotation.") {
    showInMenu = true;
    hideInHudEditor = true; // game-logic module, not a HUD element
    g_killAura = this;
}

KillAuraModule::~KillAuraModule() {
    if (g_killAura == this) g_killAura = nullptr;
}

void KillAuraModule::onInit() {
    const auto isPlayer = bedrocktools::memory::resolve(SignatureId::ActorIsPlayer);
    if (isPlayer) s_actorIsPlayer = reinterpret_cast<ActorIsPlayerFn>(isPlayer);

    const auto isInvisible = bedrocktools::memory::resolve(SignatureId::ActorIsInvisible);
    if (isInvisible) s_actorIsInvisible = reinterpret_cast<ActorIsInvisibleFn>(isInvisible);

    const auto fetch = bedrocktools::memory::resolve(SignatureId::ActorFetchNearbyActorsSorted);
    if (fetch) s_fetchNearby = reinterpret_cast<ActorFetchNearbyFn>(fetch);

    const auto solid = bedrocktools::memory::resolve(SignatureId::BlockSourceIsSolidBlockingBlock);
    if (solid) s_isSolidBlockingBlock = reinterpret_cast<IsSolidBlockingFn>(solid);

    const auto swingFn = bedrocktools::memory::resolve(SignatureId::LocalPlayerSwing);
    if (swingFn) s_playerSwing = reinterpret_cast<PlayerSwingFn>(swingFn);

    s_gameModeAttackAddr = bedrocktools::memory::resolve(SignatureId::GameModeAttack);
    s_survivalModeAttackAddr = bedrocktools::memory::resolve(SignatureId::SurvivalModeAttack);

    auto& bus = bedrocktools::events::bus();

    bus.subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) {
        s_localPlayer = event.player;
        if (g_killAura && g_killAura->enabled) {
            g_killAura->onLocalPlayerTick(event.player);
        }
    });

    // Every game-mode action (attack, use, build, destroy, ...) carries the
    // live GameMode pointer; keep the newest one as the fallback for builds
    // where the derived mGameMode offset no longer validates.
    bus.subscribe<bedrocktools::events::GameModeActionEvent>([](auto& event) {
        if (event.gameMode) {
            s_capturedGameMode = event.gameMode;
            s_capturedGameModePlayer = s_localPlayer;
        }
    });

    bus.subscribe<bedrocktools::events::ScreenStateEvent>([](auto& event) {
        using bedrocktools::events::ScreenPhase;
        if (event.phase == ScreenPhase::Opened) {
            ++s_openScreens;
        } else if (s_openScreens > 0) {
            --s_openScreens;
        }
    });
}

void KillAuraModule::onEnable() {
    // Attack as soon as a target is in range; the interval only ever gates
    // *between* attacks.
    m_nextAttack = {};
}

void KillAuraModule::onDisable() {
    m_nextAttack = {};
}

void KillAuraModule::onLocalPlayerTick(void* player) {
    if (!enabled || !isPlausiblePointer(player)) return;
    if (s_openScreens > 0) return; // inventory/chat open: hands off
    if (!attackPlayers && !attackMobs) return;
    if (!s_fetchNearby) return;

    const auto now = std::chrono::steady_clock::now();
    if (now < m_nextAttack) return;

    const AABB localBox = getActorAABB(player);
    if (!killaura::isAttackableBox(localBox)) return;
    const Vec3 eye = killaura::eyePosition(localBox);

    const ResolvedGameMode gameMode = resolveGameMode(player);
    if (!gameMode.instance || !gameMode.attack) return;

    // The fetch extent is measured from the actor, so over-fetch a little
    // and let the exact eye->box reach below decide.
    const float range = killaura::clampedRange(attackRange);
    const float extentValue = range + 4.0f;
    Vec3 extent{extentValue, extentValue, extentValue};
    const ActorVec actors = s_fetchNearby(player, &extent, 1);
    if (!actors.begin || !actors.end) return;

    void* region = onlyVisibleTargets ? blockSourceFor(player) : nullptr;

    std::vector<Target> targets;
    for (DistanceSortedActor* it = actors.begin; it < actors.end; ++it) {
        void* actor = it->mActor;
        if (!actor || actor == player) continue;

        // Players also carry the mob category bit, so classify them first
        // (same order as the Hitbox overlay).
        const bool isPlayer = s_actorIsPlayer ? s_actorIsPlayer(actor)
                                              : hasCategory(actor, offsets::ActorCategories::IsPlayer);
        if (isPlayer) {
            if (!attackPlayers) continue;
        } else if (hasCategory(actor, offsets::ActorCategories::IsMob)) {
            if (!attackMobs) continue;
        } else {
            continue; // items, projectiles, xp orbs, ...
        }

        if (!attackInvisible && s_actorIsInvisible && s_actorIsInvisible(actor)) continue;

        const AABB box = getActorAABB(actor);
        if (!killaura::isAttackableBox(box)) continue;

        const float reach = killaura::distancePointToAabb(eye, box);
        if (!(reach <= range)) continue; // also rejects NaN

        if (onlyVisibleTargets && isOccluded(region, eye, box)) continue;

        targets.push_back(Target{actor, box, reach});
    }
    if (targets.empty()) return;

    std::sort(targets.begin(), targets.end(), [](const Target& a, const Target& b) {
        return a.reach < b.reach;
    });
    if (!multiAttack) {
        targets.resize(1);
    } else if (targets.size() > kMaxMultiTargets) {
        targets.resize(kMaxMultiTargets);
    }

    if (rotateToTarget) {
        Vec2 current{0.0f, 0.0f};
        getActorRotation(player, current);
        const Vec3 aimPoint = killaura::closestPointOnAabb(eye, targets.front().box);
        const killaura::Angles angles = killaura::lookAngles(
            eye, aimPoint, killaura::Angles{current.x, current.y});
        setActorRotation(player, angles.pitch, angles.yaw);
    }

    bool attacked = false;
    for (const Target& target : targets) {
        const Vec3 hitPosition = killaura::closestPointOnAabb(eye, target.box);
        if (gameMode.attack(gameMode.instance, target.actor, &hitPosition)) attacked = true;
    }

    if (attacked && swing && s_playerSwing) {
        s_playerSwing(player, kSwingSourceAttack, 0);
    }

    m_nextAttack = now + std::chrono::milliseconds(killaura::attackIntervalMs(cps));
}

void KillAuraModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    attackRange = j.value("attackRange", attackRange);
    cps = j.value("cps", cps);
    attackPlayers = j.value("attackPlayers", attackPlayers);
    attackMobs = j.value("attackMobs", attackMobs);
    attackInvisible = j.value("attackInvisible", attackInvisible);
    multiAttack = j.value("multiAttack", multiAttack);
    rotateToTarget = j.value("rotateToTarget", rotateToTarget);
    swing = j.value("swing", swing);
    onlyVisibleTargets = j.value("onlyVisibleTargets", onlyVisibleTargets);

    attackRange = killaura::clampedRange(attackRange);
    cps = killaura::clampedCps(cps);
}

void KillAuraModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["attackRange"] = killaura::clampedRange(attackRange);
    j["cps"] = killaura::clampedCps(cps);
    j["attackPlayers"] = attackPlayers;
    j["attackMobs"] = attackMobs;
    j["attackInvisible"] = attackInvisible;
    j["multiAttack"] = multiAttack;
    j["rotateToTarget"] = rotateToTarget;
    j["swing"] = swing;
    j["onlyVisibleTargets"] = onlyVisibleTargets;
}

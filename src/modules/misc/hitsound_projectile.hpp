#pragma once

#include <bedrocktools/sdk/offsets/World.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

// Projectile ("arrow") hit detection for the Hit Sound module.
//
// The melee path can watch a specific victim because the attack hook hands it
// that victim. A bow shot has no equivalent hook: nothing in the game tells
// the client mod "your arrow landed". This header implements the client-side
// alternative. It is deliberately pure logic over one tick's worth of nearby
// actors so it can be unit tested without Minecraft (see
// tests/hitsound_test.cpp); the module only feeds it actor snapshots.
//
// Two facts about the game make it work:
//
//   1. A projectile the local player fires spawns at the player's eyes, so on
//      the tick it first becomes visible it is still within a couple of blocks
//      of the player and its motion carries it *away* from the player. An
//      arrow shot at us by a skeleton spawns at the skeleton and moves
//      *towards* us, so the same test rejects it. Seeing a projectile that
//      passes this test arms a short watch window.
//
//   2. Confirmed damage from any source shows up as a jump upwards in the
//      victim's hurt-time — the very same field the melee path confirms
//      against. A jump while the window is armed is our arrow landing, and the
//      sound plays.
//
// It is heuristic on purpose and it fails silent: when no shot is detected
// nothing plays, and a jump outside the window is ignored, so unrelated damage
// (fall damage, another player's hit, a mob fighting back) stays quiet.
//
// The module feeds this from the nearby-actor fetch, so everything here is
// bounded by that radius: a target further away than the fetch reaches is
// never watched and its hit stays silent.

namespace hitsound {

struct Point3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline float dot(Point3 a, Point3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline float squaredLength(Point3 v) { return dot(v, v); }

inline float squaredDistance(Point3 a, Point3 b) {
    const Point3 d{a.x - b.x, a.y - b.y, a.z - b.z};
    return squaredLength(d);
}

// hurtTime counts down from the maximum hurt flash (a handful of ticks) after
// each hit; values outside this range mean the pointer is not a live actor, so
// never read or trust them. Shared with the melee path in hitsound.cpp.
inline constexpr int kHurtTimeMin = 0;
inline constexpr int kHurtTimeMax = 100;

inline bool hurtTimeIsPlausible(int hurtTime) {
    return hurtTime >= kHurtTimeMin && hurtTime <= kHurtTimeMax;
}

// Actor category bits this module reads (see bedrocktools/sdk/offsets/World.hpp
// for the full enum). Mobs and players are the only actors an arrow of ours can
// hurt, so they are the only ones worth watching; everything else — item
// entities, the projectiles themselves, minecarts, TNT — is ignored.
inline constexpr std::uint32_t kProjectileCategories =
    bedrocktools::sdk::offsets::ActorCategories::IsProjectile;
inline constexpr std::uint32_t kVictimCategories =
    bedrocktools::sdk::offsets::ActorCategories::IsMob |
    bedrocktools::sdk::offsets::ActorCategories::IsPlayer;

// One nearby actor as seen on a single tick. `id` is the actor pointer, used
// purely as an identity: the tracker never dereferences it.
struct ActorSnapshot {
    void* id = nullptr;
    Point3 position{};
    Point3 delta{}; // position - previousPosition over this tick
    int hurtTime = 0;
    std::uint32_t categories = 0;

    bool isProjectile() const { return (categories & kProjectileCategories) != 0; }

    bool isPossibleVictim() const { return (categories & kVictimCategories) != 0; }
};

struct ProjectileConfig {
    // How close to the player a projectile has to be the first time it is seen
    // for it to count as our own shot. A bow spawns its arrow at the player,
    // and the shot is recognised on the first tick that shows it moving away
    // (an arrow covers ~3 blocks a tick), so this covers a tick or two of
    // flight — plus slack for a dropped tick — while still rejecting arrows
    // that were fired from further away.
    float spawnRadius = 6.0f;

    // Per-tick motion bounds. A fully charged arrow covers roughly 3 blocks a
    // tick; anything still is an arrow stuck in a block and anything much
    // faster than an arrow means the previous-position read was garbage.
    float minDeltaLength = 0.05f;
    float maxDeltaLength = 6.0f;

    // How long a shot keeps the hurt-time watch alive. Long enough
    // for a long-range arrow to land, short enough that unrelated damage soon
    // afterwards is not mistaken for the shot.
    int armWindowMs = 3000;

    // How many ticks a close-by projectile keeps being re-examined as a
    // possible shot. Its very first tick can still show no motion at all (the
    // game has not recorded a previous position for it yet), so it gets a few
    // ticks to show which way it is going before being written off — few
    // enough that an arrow flying *past* the player is long gone from the
    // spawn radius by then.
    int spawnResolveTicks = 3;

    // Cap on the watched victim list; the nearby-actor fetch is bounded
    // anyway, this just keeps the bookkeeping flat.
    std::size_t maxWatched = 48;
};

// True when a projectile that just appeared next to the player looks like the
// player's own shot: it starts inside `config.spawnRadius` of the player and
// its motion carries it further away. Exported because it is the whole
// attribution rule and worth testing on its own.
inline bool looksLikeOwnProjectile(Point3 playerPosition, const ActorSnapshot& projectile,
                                   const ProjectileConfig& config) {
    const float speedSquared = squaredLength(projectile.delta);
    const float minSquared = config.minDeltaLength * config.minDeltaLength;
    const float maxSquared = config.maxDeltaLength * config.maxDeltaLength;
    if (speedSquared < minSquared || speedSquared > maxSquared) return false;

    const float radiusSquared = config.spawnRadius * config.spawnRadius;
    if (squaredDistance(playerPosition, projectile.position) > radiusSquared) return false;

    const Point3 awayFromPlayer{projectile.position.x - playerPosition.x,
                                projectile.position.y - playerPosition.y,
                                projectile.position.z - playerPosition.z};
    return dot(projectile.delta, awayFromPlayer) > 0.0f;
}

// Tracks the local player's shots across ticks and reports which actor was
// confirmed to have taken damage from one of them.
//
// Only ever touched on the game thread (LocalPlayerTickEvent), so no locking.
class ProjectileTracker {
public:
    explicit ProjectileTracker(ProjectileConfig config = ProjectileConfig{}) : m_config(config) {}

    // Feeds one tick's nearby actors. Returns the actor whose hurt-time jumped
    // up while one of our projectiles was in flight, or nullptr when nothing
    // was confirmed this tick. `nowMs` is a monotonic clock in milliseconds
    // and `suppressHit` updates the state without reporting a hit — the module
    // passes it when the melee path already played the sound for this tick, so
    // a melee kill never fires the sound a second time.
    void* update(void* localPlayer, Point3 playerPosition, const ActorSnapshot* actors,
                 std::size_t count, long long nowMs, bool suppressHit = false) {
        std::vector<SeenProjectile> projectiles;
        std::vector<const ActorSnapshot*> victims;
        projectiles.reserve(count);
        bool armedNow = false;

        for (std::size_t i = 0; i < count; ++i) {
            const ActorSnapshot& actor = actors[i];
            if (!actor.id || actor.id == localPlayer) continue;

            if (actor.isProjectile()) {
                SeenProjectile entry{actor.id, 0};
                if (const SeenProjectile* previous = findProjectile(actor.id)) {
                    entry.ticks = previous->ticks + 1;
                }
                projectiles.push_back(entry);

                // A projectile that just appeared next to the player and is
                // moving away is our shot. Ones we have already watched for a
                // few ticks are settled: they are either flying past or stuck
                // in a block.
                if (entry.ticks <= m_config.spawnResolveTicks &&
                    looksLikeOwnProjectile(playerPosition, actor, m_config)) {
                    m_armedUntilMs = nowMs + m_config.armWindowMs;
                    armedNow = true;
                }
                continue;
            }

            if (actor.isPossibleVictim()) victims.push_back(&actor);
        }

        m_seenProjectiles.swap(projectiles);
        if (armedNow) m_watched.clear(); // fresh baselines for the new shot

        if (nowMs >= m_armedUntilMs) {
            m_watched.clear();
            return nullptr;
        }

        void* hit = nullptr;
        std::vector<Watched> next;
        next.reserve(victims.size());

        for (const ActorSnapshot* victim : victims) {
            // Implausible value: the actor object is gone and its memory has
            // been recycled. Never trust the read.
            if (!hurtTimeIsPlausible(victim->hurtTime)) continue;

            const Watched* previous = findWatched(victim->id);
            if (!previous) {
                // First sight of this victim during the window: record its
                // current hurt-time as the baseline and wait for a jump.
                if (next.size() < m_config.maxWatched) next.push_back(Watched{victim->id, victim->hurtTime});
                continue;
            }

            // hurtTime counts down after a hit, so new damage only ever shows
            // up as a jump upwards.
            if (victim->hurtTime > previous->baselineHurtTime && !hit && !suppressHit) {
                hit = victim->id;
                break;
            }

            next.push_back(Watched{victim->id, victim->hurtTime});
        }

        if (hit) {
            // One shot, one sound: drop the window so the same arrow cannot
            // fire again and the next shot has to be detected afresh.
            m_armedUntilMs = nowMs;
            m_watched.clear();
        } else {
            // Victims that left the fetched area are gone from `next`, so the
            // list prunes itself every tick.
            m_watched.swap(next);
        }

        return hit;
    }

    // Drops the armed shot and every watched victim. Called when the module is
    // (re)enabled or disabled so a stale window can never outlive the module
    // state. The projectile list is deliberately kept: it makes the projectiles
    // already flying around "known", so re-enabling the module mid-fight cannot
    // mistake an arrow in the air for a fresh shot.
    void reset() {
        m_armedUntilMs = 0;
        m_watched.clear();
    }

    bool shotArmed(long long nowMs) const { return nowMs < m_armedUntilMs; }
    std::size_t watchedCount() const { return m_watched.size(); }
    const ProjectileConfig& config() const { return m_config; }

private:
    struct Watched {
        void* id;
        int baselineHurtTime;
    };

    // A projectile in the fetched area and how many ticks we have seen it for.
    struct SeenProjectile {
        void* id;
        int ticks;
    };

    const SeenProjectile* findProjectile(void* id) const {
        for (const SeenProjectile& seen : m_seenProjectiles) {
            if (seen.id == id) return &seen;
        }
        return nullptr;
    }

    const Watched* findWatched(void* id) const {
        for (const Watched& watched : m_watched) {
            if (watched.id == id) return &watched;
        }
        return nullptr;
    }

    ProjectileConfig m_config;
    // The projectiles that were in the fetched area on the previous tick, and
    // for how long; the list rebuilds itself every tick.
    std::vector<SeenProjectile> m_seenProjectiles;
    // Victims with a recorded hurt-time baseline, alive only while a shot is
    // armed.
    std::vector<Watched> m_watched;
    long long m_armedUntilMs = 0;
};

} // namespace hitsound

#pragma once

#include <cstddef>

// Projectile hit tracking for the Hit Sound module (bow / crossbow / trident /
// snowball / egg hits).
//
// The melee lane of the module is event-driven: the GameMode::attack hook fires
// for every melee swing, so the module knows exactly who was attacked. Arrows
// never pass through that hook — the damage is dealt by the projectile actor's
// own hit logic long after the bow was released — so projectile hits have to
// be recognised from the world state alone. This header holds that decision
// logic as pure, game-free code (same idea as hitbox_camera.hpp) so it can be
// unit-tested on the host; hitsound.cpp only samples the world and feeds it
// here once per local-player tick.
//
// How a projectile hit is recognised, in three steps:
//
// 1. Classifying "our" projectiles. Every tick the module lists the actors
//    near the player. An actor carrying the IsProjectile category that is
//    first seen within a few blocks of the player while the player is holding
//    a projectile weapon can only be something the player just fired — arrows
//    spawn at the shooter's eye and fly three blocks per tick at most, so a
//    projectile is near us at birth if and only if we (or someone standing
//    point-blank next to us) shot it. Projectiles first seen far away belong
//    to somebody else and are ignored forever.
//
// 2. Watching plausible victims. While one of our projectiles is in flight,
//    every mob/player close to it has their hurt-time recorded as a baseline
//    on every tick. The watch radius is deliberately larger than the
//    distance an arrow covers in one tick plus the depth it sticks into a
//    victim, so whatever the arrow can hit this tick was already baselined
//    last tick with its pre-impact value.
//
// 3. Confirming the hit. A hit is the victim's hurt-time jumping ABOVE its
//    baseline (exactly the same confirmation the melee lane uses) while one
//    of our tracked projectiles sits at/inside the victim's box. The jump
//    alone is not enough — mobs brawl among themselves, so the spatial check
//    is what ties the damage to our arrow. Both the flight and the spatial
//    eligibility end when a projectile stops moving for a few ticks (it stuck
//    into a block), which keeps a long-idle stuck arrow from "confirming"
//    unrelated fights happening next to it.
//
// A projectile that disappears from the world (server removed it right after
// an entity hit) is kept as a "ghost" position for a short window, because in
// multiplayer the victim's hurt-time often arrives a few ticks after the
// impact, when the arrow actor is already gone.
//
// Deliberate limitation: without a verified owner offset there is no way to
// read who owns a projectile, so a projectile fired by someone standing
// point-blank next to the player can occasionally be classified as ours. The
// held-weapon check (the module only classifies while a bow/crossbow/trident/
// snowball/egg is in the hotbar) makes that scenario rare.
namespace hitsound {

// Tracked projectiles at any moment: a bow fires roughly one arrow per second
// (a rapid crossbow a few more), so the cap is generous; the oldest entry is
// recycled when it is exceeded.
inline constexpr int kMaxTrackedProjectiles = 8;

// Victims watched while a projectile is in flight. Only mob/player actors
// within a couple of blocks of the projectile enter the list, so this is far
// more than a fast-paced fight needs.
inline constexpr int kMaxWatchedVictims = 16;

// A projectile first seen within this distance of the player is treated as
// one the player just fired. Arrows spawn at the shooter's eye (about 1.6
// blocks up, measured from feet positions) and cover at most ~3.2 blocks in
// their first tick, so freshly fired arrows are always inside this radius;
// projectiles of other players usually are not, because they are already
// mid-flight when they first come this close.
inline constexpr float kProjectileSpawnRadius = 5.0f;

// Mob/player actors within this distance of an in-flight projectile get their
// hurt-time baselined every tick. An arrow covers at most ~3.2 blocks per
// tick and ends up at most ~1.3 from the victim's feet position when it
// sticks, so 5.0 guarantees every possible victim of this tick's impact was
// baselined on the tick before (with its pre-impact hurt-time).
inline constexpr float kVictimWatchRadius = 5.0f;

// Containment for confirming a hit: the tracked projectile's position must be
// inside the victim's box expanded by this much. An arrow that just connected
// rests inside the victim's body, so 1.25 comfortably covers every vanilla
// hitbox while an arrow merely flying past (or a brawl one block away) stays
// outside.
inline constexpr float kImpactExpansion = 1.25f;

// Tighter containment for the point-blank special case below (a projectile
// that was classified this very tick and is already touching a freshly hurt
// victim). Narrow on purpose: the only thing proving the hit here is contact.
inline constexpr float kSpawnTickImpactExpansion = 0.6f;

// Per-tick displacement (blocks) below which a projectile counts as stalled.
// A real arrow never flies slower than this; a stuck one does not move at
// all. Positions are feet positions read once per tick, so walking-pace
// riders and gravity alone never dip a live arrow below the threshold for
// more than a tick or two.
inline constexpr float kMinFlightSpeed = 0.5f;

// How many ticks a projectile stays confirm-eligible after its last observed
// movement. Covers the multiplayer case where the projectile is removed (or
// stops) at the impact tick but the victim's hurt-time only rises a few ticks
// later once the server confirms the damage. After the grace lapses the
// projectile is treated as a piece of world decoration (a stuck arrow) and
// can neither watch nor confirm anything.
inline constexpr int kFlightGraceTicks = 6;

// How many ticks a vanished projectile's last position is kept as a ghost.
// The server usually removes an arrow right after an entity hit while the
// victim's hurt-time sync is still in flight, so the ghost bridges that gap.
inline constexpr int kGhostTicks = 12;

// How many ticks a watched victim stays pending without being near a live
// projectile. Multiplayer hurt-time confirmations arrive within a handful of
// ticks; anything later is not this arrow's kill (and a fresh hit re-arms a
// new baseline anyway).
inline constexpr int kVictimWatchTicks = 12;

// Total tracked lifetime, generous enough for any realistic arrow flight
// (~1 second) plus a long stuck period that the grace alone would have
// ended much earlier.
inline constexpr int kProjectileMaxAgeTicks = 100;

// Plausibility bounds for hurt-time reads, identical to the melee lane:
// values outside this range mean the actor pointer is stale and its memory
// has been recycled, so the read must not be trusted.
inline constexpr int kHurtTimeMin = 0;
inline constexpr int kHurtTimeMax = 100;

// One actor's per-tick observation, filled in by the module from game memory.
struct ProjectileObservation {
    const void* actor = nullptr;
    // Carries the IsProjectile category (arrows, tridents, thrown items...).
    bool isProjectile = false;
    // Is a mob or a player (a plausible hurt-time victim). Items, boats and
    // other non-living actors never enter the watch list.
    bool isVictimCandidate = false;
    // Feet position and world-space AABB.
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float minX = 0.0f, minY = 0.0f, minZ = 0.0f;
    float maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
    int hurtTime = 0;
};

struct TrackedProjectile {
    const void* actor = nullptr;
    // Last seen position (kept as a ghost while the actor is missing).
    float x = 0.0f, y = 0.0f, z = 0.0f;
    // Ticks since the actor was last seen in the world (0 = seen this tick).
    int unseenTicks = 0;
    // Confirm-eligibility left, in ticks. Refreshed to kFlightGraceTicks on
    // every observed movement; a projectile that stalls (stuck in a block)
    // loses it tick by tick and is then dropped.
    int flightGrace = 0;
    int age = 0;
};

struct WatchedVictim {
    const void* actor = nullptr;
    // Last hurt-time seen for this victim; a hit is a jump above it.
    int baselineHurtTime = 0;
    // Ticks since the victim was last near a live projectile.
    int unseenTicks = 0;
};

inline float distanceSquared(float ax, float ay, float az, float bx, float by, float bz) {
    const float dx = ax - bx;
    const float dy = ay - by;
    const float dz = az - bz;
    return dx * dx + dy * dy + dz * dz;
}

inline bool pointInExpandedBox(float px, float py, float pz,
                               float minX, float minY, float minZ,
                               float maxX, float maxY, float maxZ,
                               float expand) {
    return px >= minX - expand && px <= maxX + expand &&
           py >= minY - expand && py <= maxY + expand &&
           pz >= minZ - expand && pz <= maxZ + expand;
}

// Accumulates projectile/victim state across ticks. Feed one tick's
// observations per call (exactly one call per local-player tick); the return
// value says a projectile hit was confirmed this tick and the sound should
// play. All state is bounded and reset() clears everything (module toggled,
// world changed).
class ProjectileHitTracker {
public:
    void reset() {
        m_projectileCount = 0;
        m_victimCount = 0;
    }

    int trackedCount() const { return m_projectileCount; }
    int watchedCount() const { return m_victimCount; }

    bool update(const ProjectileObservation* samples, std::size_t count,
                const void* localPlayer,
                float playerX, float playerY, float playerZ,
                bool holdingProjectileWeapon) {
        if (!samples && count > 0) return false;

        // ---- pass A: age the tracked projectiles ----
        // One update() call is one game tick, so the counters are ticks.
        // Projectiles die of old age, after too long unseen (their actor was
        // removed and the ghost window lapsed) or once they have been stalled
        // longer than the flight grace (stuck in a block or floating).
        for (int i = 0; i < m_projectileCount;) {
            TrackedProjectile& p = m_projectiles[i];
            ++p.unseenTicks;
            --p.flightGrace;
            ++p.age;
            if (p.age > kProjectileMaxAgeTicks || p.unseenTicks > kGhostTicks ||
                p.flightGrace < 0) {
                m_projectiles[i] = m_projectiles[--m_projectileCount];
                continue;
            }
            ++i;
        }

        // ---- pass B: classify freshly fired projectiles as ours ----
        bool classifiedThisTick = false;
        if (holdingProjectileWeapon) {
            for (std::size_t s = 0; s < count; ++s) {
                const ProjectileObservation& o = samples[s];
                if (!o.isProjectile || !o.actor) continue;
                if (findProjectile(o.actor) >= 0) continue;
                if (distanceSquared(o.x, o.y, o.z, playerX, playerY, playerZ) >
                    kProjectileSpawnRadius * kProjectileSpawnRadius) {
                    continue;
                }
                if (m_projectileCount >= kMaxTrackedProjectiles) {
                    // Recycle the oldest slot; the oldest projectile is the
                    // least likely to still be flying.
                    int oldest = 0;
                    for (int i = 1; i < m_projectileCount; ++i) {
                        if (m_projectiles[i].age > m_projectiles[oldest].age) oldest = i;
                    }
                    m_projectiles[oldest] = m_projectiles[--m_projectileCount];
                }
                TrackedProjectile& p = m_projectiles[m_projectileCount++];
                p.actor = o.actor;
                p.x = o.x;
                p.y = o.y;
                p.z = o.z;
                p.unseenTicks = 0;
                p.flightGrace = kFlightGraceTicks;
                p.age = 0;
                classifiedThisTick = true;
            }
        }

        // ---- pass C: refresh projectiles seen this tick ----
        // Movement re-arms the flight grace; a projectile that keeps moving is
        // alive and dangerous, one that froze is already stuck somewhere.
        for (int i = 0; i < m_projectileCount; ++i) {
            TrackedProjectile& p = m_projectiles[i];
            const ProjectileObservation* o = findSample(samples, count, p.actor);
            if (!o) continue;
            const float movedSquared = distanceSquared(p.x, p.y, p.z, o->x, o->y, o->z);
            p.x = o->x;
            p.y = o->y;
            p.z = o->z;
            p.unseenTicks = 0;
            if (movedSquared >= kMinFlightSpeed * kMinFlightSpeed) {
                p.flightGrace = kFlightGraceTicks;
            }
        }

        // ---- pass D: confirm hits against baselines from earlier ticks ----
        // This pass must run BEFORE the watch refresh below: on the impact
        // tick the victim is standing right next to the arrow, so refreshing
        // first would fold the fresh hurt-time into the baseline and swallow
        // the very jump this pass is looking for.
        bool played = false;
        for (int v = 0; v < m_victimCount; ++v) {
            WatchedVictim& w = m_victims[v];
            const ProjectileObservation* o = findSample(samples, count, w.actor);
            if (!o) continue;
            if (o->hurtTime < kHurtTimeMin || o->hurtTime > kHurtTimeMax) {
                // Implausible read: the actor object is gone and its memory
                // has been recycled. Expire the entry instead of trusting it.
                w.unseenTicks = kVictimWatchTicks + 1;
                continue;
            }
            if (o->hurtTime > w.baselineHurtTime && projectileTouches(*o, kImpactExpansion)) {
                played = true;
                break;
            }
            // Remember the latest value: it ticks down while a previous hit's
            // flash fades, and following it keeps the next jump unambiguous.
            w.baselineHurtTime = o->hurtTime;
        }
        if (played) {
            // Consume all pending victims: one sound per impact tick, and the
            // watch refresh below re-arms fresh baselines for further hits.
            m_victimCount = 0;
        }

        // ---- pass E: refresh the victim watch list ----
        bool touched[kMaxWatchedVictims] = {};
        for (std::size_t s = 0; s < count; ++s) {
            const ProjectileObservation& o = samples[s];
            if (!o.isVictimCandidate || !o.actor || o.actor == localPlayer) continue;
            if (o.hurtTime < kHurtTimeMin || o.hurtTime > kHurtTimeMax) continue;
            // Only victims near a projectile that was actually seen this tick
            // stay armed; ghosts only ever confirm (pass D), they do not open
            // new watches.
            if (!liveProjectileNear(o)) continue;

            const int existing = findVictim(o.actor);
            if (existing >= 0) {
                WatchedVictim& w = m_victims[existing];
                w.baselineHurtTime = o.hurtTime;
                w.unseenTicks = 0;
                touched[existing] = true;
            } else {
                if (m_victimCount >= kMaxWatchedVictims) {
                    // Drop the stalest entry (the one closest to expiring).
                    int oldest = 0;
                    for (int i = 1; i < m_victimCount; ++i) {
                        if (m_victims[i].unseenTicks > m_victims[oldest].unseenTicks) oldest = i;
                    }
                    m_victims[oldest] = m_victims[--m_victimCount];
                }
                const int index = m_victimCount++;
                m_victims[index].actor = o.actor;
                m_victims[index].baselineHurtTime = o.hurtTime;
                m_victims[index].unseenTicks = 0;
                touched[index] = true;

                // Point-blank special case: a projectile classified this very
                // tick that is already touching a freshly hurt victim. The
                // normal pass D path cannot see this hit (there was no earlier
                // tick to baseline the victim), and at contact distance with a
                // projectile that can only be ours the evidence is solid.
                if (!played && classifiedThisTick && o.hurtTime > kHurtTimeMin &&
                    projectileTouches(o, kSpawnTickImpactExpansion, /*freshOnly=*/true)) {
                    played = true;
                    m_victimCount = 0;
                }
            }
        }

        // ---- pass F: age victims that were not near a projectile this tick ----
        for (int i = 0; i < m_victimCount;) {
            if (touched[i]) {
                ++i;
                continue;
            }
            WatchedVictim& w = m_victims[i];
            ++w.unseenTicks;
            if (w.unseenTicks > kVictimWatchTicks) {
                m_victims[i] = m_victims[--m_victimCount];
                continue;
            }
            ++i;
        }

        return played;
    }

private:
    int findProjectile(const void* actor) const {
        for (int i = 0; i < m_projectileCount; ++i) {
            if (m_projectiles[i].actor == actor) return i;
        }
        return -1;
    }

    int findVictim(const void* actor) const {
        for (int i = 0; i < m_victimCount; ++i) {
            if (m_victims[i].actor == actor) return i;
        }
        return -1;
    }

    static const ProjectileObservation* findSample(const ProjectileObservation* samples,
                                                   std::size_t count, const void* actor) {
        for (std::size_t i = 0; i < count; ++i) {
            if (samples[i].actor == actor) return &samples[i];
        }
        return nullptr;
    }

    // True when any tracked projectile's position (including ghosts of
    // already-removed arrows) sits inside the victim's box expanded by
    // `expand`. With freshOnly, only projectiles classified this very tick
    // (age 0) count.
    bool projectileTouches(const ProjectileObservation& victim, float expand,
                           bool freshOnly = false) const {
        for (int i = 0; i < m_projectileCount; ++i) {
            const TrackedProjectile& p = m_projectiles[i];
            if (freshOnly && p.age != 0) continue;
            if (pointInExpandedBox(p.x, p.y, p.z,
                                   victim.minX, victim.minY, victim.minZ,
                                   victim.maxX, victim.maxY, victim.maxZ,
                                   expand)) {
                return true;
            }
        }
        return false;
    }

    // True when a projectile seen this tick and still within its flight grace
    // is close enough that the victim could plausibly be its next target.
    bool liveProjectileNear(const ProjectileObservation& victim) const {
        for (int i = 0; i < m_projectileCount; ++i) {
            const TrackedProjectile& p = m_projectiles[i];
            if (p.unseenTicks != 0 || p.flightGrace < 0) continue;
            if (distanceSquared(p.x, p.y, p.z, victim.x, victim.y, victim.z) <=
                kVictimWatchRadius * kVictimWatchRadius) {
                return true;
            }
        }
        return false;
    }

    TrackedProjectile m_projectiles[kMaxTrackedProjectiles];
    int m_projectileCount = 0;
    WatchedVictim m_victims[kMaxWatchedVictims];
    int m_victimCount = 0;
};

} // namespace hitsound

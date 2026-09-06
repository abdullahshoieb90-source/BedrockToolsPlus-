// Unit tests for the Hit Sound module's projectile hit tracker
// (hitsound_projectile.hpp) — the pure decision logic that recognises when the
// player's bow/crossbow/trident projectile connects with a mob or player.
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src tests/hitsound_projectile_test.cpp -o /tmp/hsp_test
//     /tmp/hsp_test
//
// The harness simulates one actor list per tick and feeds it to the tracker
// exactly like the module does, so every test reads as a scripted gameplay
// situation: launch position, per-tick flight, impact tick and hurt-time.

#include "modules/misc/hitsound_projectile.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

// A fake world actor. Positions are feet positions, matching what the module
// samples from the game; `id` stands in for the actor pointer.
struct Ent {
    const void* id = nullptr;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float halfW = 0.6f;   // horizontal half extent of the box
    float height = 1.9f;  // box height
    int hurt = 0;         // hurt-time value observed this tick
    bool projectile = false;
    bool victim = true;   // mob/player candidates only
    bool present = true;  // false = removed from the world (despawned)
};

// One simulated local player + world, ticked like the module ticks it.
struct Sim {
    std::vector<Ent> ents;
    float px = 0.0f, py = 0.0f, pz = 0.0f;  // player feet position
    bool holding = true;                    // projectile weapon in the hotbar
    hitsound::ProjectileHitTracker tracker;

    bool tick() {
        hitsound::ProjectileObservation samples[64];
        std::size_t count = 0;
        for (const Ent& e : ents) {
            if (!e.present || count >= 64) continue;
            hitsound::ProjectileObservation& o = samples[count++];
            o.actor = e.id;
            o.isProjectile = e.projectile;
            o.isVictimCandidate = e.victim && !e.projectile;
            o.x = e.x;
            o.y = e.y;
            o.z = e.z;
            o.minX = e.x - e.halfW;
            o.minY = e.y;
            o.minZ = e.z - e.halfW;
            o.maxX = e.x + e.halfW;
            o.maxY = e.y + e.height;
            o.maxZ = e.z + e.halfW;
            o.hurtTime = e.hurt;
        }
        return tracker.update(samples, count, reinterpret_cast<const void*>(0x1),
                              px, py, pz, holding);
    }

    Ent* find(const void* id) {
        for (Ent& e : ents) {
            if (e.id == id) return &e;
        }
        return nullptr;
    }
};

const void* pid(int i) { return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(i)); }

// Bow shot from the player towards a zombie 8 blocks away. The arrow spawns
// about 2 blocks out, flies 3 blocks per tick and lands inside the zombie's
// box on the third tick, where the zombie's hurt-time jumps.
void testArrowFlightPlaysOnce() {
    std::printf("arrow flight -> single play\n");
    Sim sim;
    Ent arrow{pid(100), 0, 1, 2, 0.1f, 0.2f, 0, true, false};
    Ent zombie{pid(200), 0, 0, 8};
    sim.ents = {arrow, zombie};

    check(!sim.tick(), "tick 0 (arrow at 2, classified): silent");
    check(sim.tracker.trackedCount() == 1, "tick 0: projectile tracked as ours");

    sim.find(pid(100))->z = 5;
    check(!sim.tick(), "tick 1 (arrow at 5, victim baselined): silent");

    sim.find(pid(100))->z = 8;
    sim.find(pid(200))->hurt = 10;
    check(sim.tick(), "tick 2 (arrow lands, hurt-time jumps): plays");
    check(!sim.tick(), "tick 3: does not refire");
    sim.find(pid(200))->hurt = 9;
    check(!sim.tick(), "tick 4: does not refire while the flash decays");
}

// Without a projectile weapon in the hotbar a fresh projectile near the player
// is never classified as ours (it must be someone else's), so their hits stay
// silent.
void testNoWeaponNoSound() {
    std::printf("no projectile weapon held -> silent\n");
    Sim sim;
    sim.holding = false;
    Ent arrow{pid(100), 0, 1, 2, 0.1f, 0.2f, 0, true, false};
    Ent zombie{pid(200), 0, 0, 8};
    sim.ents = {arrow, zombie};

    bool played = false;
    for (int t = 0; t < 4; ++t) {
        if (t == 1) sim.find(pid(100))->z = 5;
        if (t == 2) {
            sim.find(pid(100))->z = 8;
            sim.find(pid(200))->hurt = 10;
        }
        played = sim.tick() || played;
    }
    check(!played, "foreign arrow never plays the sound");
    check(sim.tracker.trackedCount() == 0, "foreign arrow never tracked");
}

// A projectile first seen far away is mid-flight from somebody else and is
// ignored even when it later lands next to a mob.
void testStrayProjectileIgnored() {
    std::printf("stray projectile (first seen far) -> silent\n");
    Sim sim;
    Ent arrow{pid(100), 0, 1, 20, 0.1f, 0.2f, 0, true, false};
    Ent zombie{pid(200), 0, 0, 12};
    sim.ents = {arrow, zombie};

    bool played = false;
    for (int t = 0; t < 5; ++t) {
        sim.find(pid(100))->z -= 2.0f;  // 20 -> 12 over four ticks
        if (t == 3) sim.find(pid(200))->hurt = 10;
        played = sim.tick() || played;
    }
    check(!played, "arrow fired from far away never plays");
}

// The victim was hurt shortly before the arrow arrives: the baseline follows
// the decaying hurt-time and the fresh impact still jumps above it.
void testBaselineTracksDecay() {
    std::printf("victim already hurt (decaying) -> still plays on impact\n");
    Sim sim;
    Ent arrow{pid(100), 0, 1, 0.5f, 0.1f, 0.2f, 0, true, false};
    Ent zombie{pid(200), 0, 0, 3};
    zombie.hurt = 8;  // a zombie brawl hit one tick before the shot
    sim.ents = {arrow, zombie};

    check(!sim.tick(), "tick 0 (hurt 8, arrow launched): silent");
    sim.find(pid(100))->z = 1.5f;
    sim.find(pid(200))->hurt = 7;
    check(!sim.tick(), "tick 1 (hurt 7): silent");
    sim.find(pid(100))->z = 2.5f;
    sim.find(pid(200))->hurt = 6;
    check(!sim.tick(), "tick 2 (hurt 6): silent");
    sim.find(pid(100))->z = 3.5f;
    sim.find(pid(200))->hurt = 10;
    check(sim.tick(), "tick 3 (arrow impact resets hurt to 10): plays");
}

// The arrow sticks into the ground next to a brawl: the hurt-time of the
// brawling mob jumps while our (stalled) arrow is nearby but NOT touching,
// and after the flight grace lapses not even contact would confirm.
void testStuckArrowDoesNotConfirmBrawl() {
    std::printf("stuck arrow near a brawl -> silent\n");
    Sim sim;
    Ent arrow{pid(100), 0, 1, 2, 0.1f, 0.2f, 0, true, false};
    Ent zombie{pid(200), 0, 0, 5};
    sim.ents = {arrow, zombie};

    sim.tick();                                       // arrow classified
    sim.find(pid(100))->x = 1.4f;                     // t1: flying
    sim.find(pid(100))->y = 0.5f;
    sim.find(pid(100))->z = 3.6f;
    sim.tick();
    sim.find(pid(100))->x = 2.2f;                     // t2: lands in the ground
    sim.find(pid(100))->y = 0.2f;                     //     two blocks from the mob
    sim.find(pid(100))->z = 5.0f;
    sim.tick();
    check(sim.tracker.trackedCount() == 1, "stuck arrow still tracked right after landing");

    for (int t = 3; t <= 4; ++t) sim.tick();          // arrow stalls, grace decays
    sim.find(pid(200))->hurt = 10;                    // t5: a brawl hit lands
    check(!sim.tick(), "hurt jump next to (not under) the arrow: silent");

    for (int t = 6; t <= 11; ++t) sim.tick();         // grace lapses, arrow dropped
    sim.find(pid(200))->hurt = 5;
    sim.tick();
    sim.find(pid(200))->hurt = 10;                    // another brawl hit much later
    check(!sim.tick(), "hurt jump after the grace lapsed: silent");
    check(sim.tracker.trackedCount() == 0, "stalled arrow dropped after the grace");
}

// Multiplayer: the server removes the arrow at the impact tick and the
// victim's hurt-time only rises a couple of ticks later. The ghost position
// of the vanished arrow bridges that gap.
void testDespawnedArrowStillConfirms() {
    std::printf("arrow removed at impact, hurt-time arrives late -> plays\n");
    Sim sim;
    Ent arrow{pid(100), 0, 1, 2, 0.1f, 0.2f, 0, true, false};
    Ent zombie{pid(200), 0, 0, 8};
    sim.ents = {arrow, zombie};

    sim.tick();                                       // t0: classified
    sim.find(pid(100))->z = 5;
    sim.tick();                                       // t1: zombie baselined
    sim.find(pid(100))->z = 7.8f;
    sim.tick();                                       // t2: arrow 0.2 from the box
    sim.find(pid(100))->present = false;              // t3: server removes the arrow
    check(!sim.tick(), "impact tick without hurt-time: silent");
    sim.find(pid(200))->hurt = 10;                    // t4: confirmation arrives
    check(sim.tick(), "late hurt-time jump confirmed via the ghost position");
}

// ...but the patience is finite: a jump many ticks after the arrow vanished
// is not ours and does not play.
void testGhostWindowExpires() {
    std::printf("hurt-time long after the arrow vanished -> silent\n");
    Sim sim;
    Ent arrow{pid(100), 0, 1, 2, 0.1f, 0.2f, 0, true, false};
    Ent zombie{pid(200), 0, 0, 8};
    sim.ents = {arrow, zombie};

    sim.tick();
    sim.find(pid(100))->z = 5;
    sim.tick();
    sim.find(pid(100))->z = 7.8f;
    sim.tick();
    sim.find(pid(100))->present = false;
    for (int t = 0; t < 14; ++t) sim.tick();          // far beyond the ghost window
    sim.find(pid(200))->hurt = 10;
    check(!sim.tick(), "very late jump: silent");
}

// A multishot volley (or any second arrow) hitting the same victim on the
// same tick plays exactly one sound.
void testMultishotPlaysOnce() {
    std::printf("multishot volley -> one sound\n");
    Sim sim;
    Ent arrowA{pid(100), 0, 1, 2, 0.1f, 0.2f, 0, true, false};
    Ent arrowB{pid(101), 0.2f, 1, 2, 0.1f, 0.2f, 0, true, false};
    Ent zombie{pid(200), 0, 0, 8};
    sim.ents = {arrowA, arrowB, zombie};

    sim.tick();
    sim.find(pid(100))->z = 5;
    sim.find(pid(101))->z = 5;
    sim.tick();

    sim.find(pid(100))->z = 8;
    sim.find(pid(101))->z = 8;
    sim.find(pid(200))->hurt = 10;
    check(sim.tick(), "both arrows land on the same tick: plays");
    check(!sim.tick(), "and only once");
    sim.find(pid(200))->hurt = 9;
    check(!sim.tick(), "no refire while the flash decays");
}

// Two victims confirmed on the same tick share the one sound (fresh baselines
// are armed again afterwards, so a real second hit still plays).
void testTwoVictimsSameTick() {
    std::printf("two victims on one tick -> one sound\n");
    Sim sim;
    Ent arrow{pid(100), 0, 1, 2, 0.1f, 0.2f, 0, true, false};
    Ent zombieA{pid(200), 0, 0, 8};
    Ent zombieB{pid(201), 1.0f, 0, 8.5f};
    sim.ents = {arrow, zombieA, zombieB};

    sim.tick();
    sim.find(pid(100))->z = 5;
    sim.tick();
    sim.find(pid(100))->z = 8;
    sim.find(pid(200))->hurt = 10;
    sim.find(pid(201))->hurt = 10;
    check(sim.tick(), "both jump on the impact tick: plays once");
    check(sim.tracker.watchedCount() == 2, "baselines re-armed for both victims");
}

// The local player is never a victim: a skeleton shooting the player from
// point-blank range must not trigger the sound even if its arrow gets
// misclassified as ours.
void testLocalPlayerNeverAVictim() {
    std::printf("local player hit -> silent\n");
    Sim sim;
    Ent arrow{pid(100), 0, 1, 2, 0.1f, 0.2f, 0, true, false};
    Ent self{pid(1), 0, 0, 8};  // 0x1 is the local player's pointer
    self.hurt = 10;
    sim.ents = {arrow, self};

    sim.tick();
    sim.find(pid(100))->z = 5;
    sim.tick();
    sim.find(pid(100))->z = 7.8f;
    check(!sim.tick(), "the player's own hurt-time never plays");
}

// Implausible hurt-time reads (stale/recycled actor memory) are never
// trusted.
void testImplausibleHurtIgnored() {
    std::printf("implausible hurt-time -> ignored\n");
    Sim sim;
    Ent arrow{pid(100), 0, 1, 2, 0.1f, 0.2f, 0, true, false};
    Ent zombie{pid(200), 0, 0, 8};
    zombie.hurt = 150;  // garbage outside the plausible flash range
    sim.ents = {arrow, zombie};

    sim.tick();
    sim.find(pid(100))->z = 5;
    sim.tick();
    sim.find(pid(100))->z = 8;
    sim.find(pid(200))->hurt = 10;
    check(!sim.tick(), "victim with garbage hurt-time was never watched");
}

// Point-blank face shot: the arrow is classified and already touching a
// freshly hurt victim on its very first tick, so there was no earlier tick to
// baseline from. The tight-contact special case covers it.
void testPointBlankSpawnTouch() {
    std::printf("point-blank shot on the spawn tick -> plays\n");
    Sim sim;
    Ent zombie{pid(200), 0, 0, 0.8f};  // hugging the player
    Ent arrow{pid(100), 0, 1.5f, 0.6f, 0.1f, 0.2f, 0, true, false};
    sim.ents = {zombie, arrow};

    sim.find(pid(200))->hurt = 10;  // hurt and impact on the same tick
    check(sim.tick(), "spawn-tick contact with a fresh hurt: plays");

    // Negative control: same geometry, but the victim is not hurt.
    Sim calm;
    Ent z2{pid(200), 0, 0, 0.8f};
    Ent a2{pid(100), 0, 1.5f, 0.6f, 0.1f, 0.2f, 0, true, false};
    calm.ents = {z2, a2};
    check(!calm.tick(), "spawn-tick contact without a hurt: silent");
}

// An actor that is neither a projectile nor a mob/player candidate (dropped
// item, boat, ...) never enters the watch list.
void testNonCandidateIgnored() {
    std::printf("non-victim actor hurt change -> silent\n");
    Sim sim;
    Ent arrow{pid(100), 0, 1, 2, 0.1f, 0.2f, 0, true, false};
    Ent item{pid(300), 0, 0, 8};
    item.victim = false;  // the module only flags mobs/players
    sim.ents = {arrow, item};

    sim.tick();
    sim.find(pid(100))->z = 5;
    sim.tick();
    sim.find(pid(100))->z = 8;
    sim.find(pid(300))->hurt = 10;
    check(!sim.tick(), "non-candidate actor is not watched");
}

// With no projectile in flight at all, mob fights around the player stay
// silent.
void testNoProjectileNoWatch() {
    std::printf("mob brawl without any projectile -> silent\n");
    Sim sim;
    Ent zombie{pid(200), 0, 0, 3};
    sim.ents = {zombie};

    check(!sim.tick(), "tick 0: silent");
    zombie.hurt = 10;
    check(!sim.tick(), "brawl hit: silent");
    check(sim.tracker.watchedCount() == 0, "nothing was watched");
}

// The internal lists stay bounded no matter how busy the world is.
void testCaps() {
    std::printf("caps\n");
    Sim sim;
    for (int i = 0; i < 12; ++i) {
        Ent arrow{pid(1000 + i), i * 0.1f, 1, 2, 0.1f, 0.2f, 0, true, false};
        sim.ents.push_back(arrow);
    }
    sim.tick();
    check(sim.tracker.trackedCount() == hitsound::kMaxTrackedProjectiles,
          "projectile list capped");

    Sim busy;
    Ent arrow{pid(100), 0, 1, 2, 0.1f, 0.2f, 0, true, false};
    busy.ents.push_back(arrow);
    for (int i = 0; i < 40; ++i) {
        const float angle = i * 0.7f;
        Ent mob{pid(2000 + i), std::cos(angle) * 2.0f, 0, 2 + std::sin(angle) * 2.0f};
        busy.ents.push_back(mob);
    }
    busy.tick();
    busy.find(pid(100))->z = 3;
    busy.tick();
    check(busy.tracker.watchedCount() <= hitsound::kMaxWatchedVictims,
          "victim list capped");
}

// reset() drops all state (module toggle / world change).
void testReset() {
    std::printf("reset\n");
    Sim sim;
    Ent arrow{pid(100), 0, 1, 2, 0.1f, 0.2f, 0, true, false};
    Ent zombie{pid(200), 0, 0, 8};
    sim.ents = {arrow, zombie};
    sim.tick();
    sim.find(pid(100))->z = 5;
    sim.tick();
    sim.tracker.reset();
    check(sim.tracker.trackedCount() == 0, "reset clears tracked projectiles");
    check(sim.tracker.watchedCount() == 0, "reset clears watched victims");
    sim.find(pid(100))->z = 8;
    sim.find(pid(200))->hurt = 10;
    check(!sim.tick(), "state from before the reset is gone");
}

// Degenerate input must not crash or report hits.
void testNullSamples() {
    std::printf("null samples\n");
    hitsound::ProjectileHitTracker tracker;
    check(!tracker.update(nullptr, 0, nullptr, 0, 0, 0, true), "null sample list: no hit");
    check(!tracker.update(nullptr, 4, nullptr, 0, 0, 0, true), "null samples with count: no hit");
}

// A second arrow landing on the same victim shortly after the first still
// plays: baselines re-arm after every confirmed hit.
void testSecondArrowPlays() {
    std::printf("second arrow shortly after the first -> plays again\n");
    Sim sim;
    Ent arrow{pid(100), 0, 1, 2, 0.1f, 0.2f, 0, true, false};
    Ent zombie{pid(200), 0, 0, 8};
    sim.ents = {arrow, zombie};

    sim.tick();
    sim.find(pid(100))->z = 5;
    sim.tick();
    sim.find(pid(100))->z = 8;
    sim.find(pid(200))->hurt = 10;
    check(sim.tick(), "first arrow plays");

    // The arrow despawns; 0.5s later a fresh shot flies the same path.
    sim.find(pid(100))->present = false;
    for (int t = 0; t < 8; ++t) {
        sim.find(pid(200))->hurt = 10 - t - 1 > 0 ? 10 - t - 1 : 0;
        sim.tick();
    }
    Ent arrow2{pid(101), 0, 1, 2, 0.1f, 0.2f, 0, true, false};
    sim.ents.push_back(arrow2);
    sim.tick();
    sim.find(pid(101))->z = 5;
    sim.tick();
    sim.find(pid(101))->z = 8;
    sim.find(pid(200))->hurt = 10;
    check(sim.tick(), "second arrow plays too");
}

} // namespace

int main() {
    testArrowFlightPlaysOnce();
    testNoWeaponNoSound();
    testStrayProjectileIgnored();
    testBaselineTracksDecay();
    testStuckArrowDoesNotConfirmBrawl();
    testDespawnedArrowStillConfirms();
    testGhostWindowExpires();
    testMultishotPlaysOnce();
    testTwoVictimsSameTick();
    testLocalPlayerNeverAVictim();
    testImplausibleHurtIgnored();
    testPointBlankSpawnTouch();
    testNonCandidateIgnored();
    testNoProjectileNoWatch();
    testCaps();
    testReset();
    testNullSamples();
    testSecondArrowPlays();

    if (g_failures == 0) {
        std::printf("\nall projectile tracker tests passed\n");
        return 0;
    }
    std::printf("\n%d check(s) failed\n", g_failures);
    return 1;
}

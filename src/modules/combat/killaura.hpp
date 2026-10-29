#pragma once

#include "../Module.hpp"

#include <chrono>

// Kill Aura
//
// Automatically attacks players and mobs around the local player through
// the game's own GameMode attack path - the exact virtual the vanilla
// attack input calls - so damage, cooldowns, knockback, item use and the
// server transaction are all produced by the game itself. Nothing is
// forged at the packet level.
//
// Targets come from the same sorted nearby-actor fetch the Hitbox overlay
// uses; the pure selection/aim rules live in killaura_logic.hpp and are
// host-tested. Reach is measured the vanilla way (eyes to the closest
// point of the target's collision box), and that closest point is also the
// hit position handed to the attack.
//
// Getting the GameMode instance is the one version-specific piece: there is
// no signature for it, so the module derives the Player::mGameMode slot
// from the verified mSkin offset (in the game's Player layout the members
// are `mGameMode`, `mSpawnRandomizer`, `mSkin` - three consecutive
// unique_ptrs) and *validates* whatever it reads against the resolved
// GameMode/SurvivalMode attack functions (vtable slot 16) before calling
// anything through it. If a future build moves the member, validation
// fails and the module falls back to the GameMode pointer captured from
// the game's own action events (any manual attack/use/place refreshes
// it), instead of calling through a wrong pointer.
//
// Attacks run on the local-player tick (game thread). CPS above 20
// therefore attacks once per tick at most; Multi Attack strikes every
// valid target in range once per interval instead of only the nearest.
class KillAuraModule : public Module {
public:
    KillAuraModule();
    ~KillAuraModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from the LocalPlayerTickEvent subscription with the local
    // player actor. Public like HitSoundModule::onAttack so host tests can
    // drive it with fake actor memory.
    void onLocalPlayerTick(void* player);

    // Menu settings (see loadConfig for the clamps).
    float attackRange = 3.0f;        // blocks, eyes -> target box
    int cps = 10;                    // attacks per second, 1..30
    bool attackPlayers = true;
    bool attackMobs = true;
    bool attackInvisible = false;    // also strike invisible actors
    bool multiAttack = false;        // every target in range per interval
    bool rotateToTarget = false;     // snap the view onto the target first
    bool swing = true;               // play the arm swing with each attack
    bool onlyVisibleTargets = false; // skip targets fully behind solid blocks

private:
    // Next moment an attack may fire; only advanced by actual attacks, so
    // entering range of a target is answered on the very next tick.
    std::chrono::steady_clock::time_point m_nextAttack{};
};

extern KillAuraModule* g_killAura;

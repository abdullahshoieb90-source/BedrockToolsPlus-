#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/world/Actor.hpp>
#include <chrono>
#include <string>
#include <vector>

// Hit Sound
//
// Lets the player pick a custom sound that plays only when a melee attack
// actually deals damage to a mob or another player — swings that whiff, hit
// during the attack cooldown, get blocked or are rejected by the server stay
// silent. The attack hook fires before the damage is applied (and a few ticks
// before the server confirms it in multiplayer), so the module records each
// swing as a pending hit and watches the victim's hurt-time field on the
// player ticks afterwards: it plays only once the field rises, proving the
// target took the hit.
//
// The module owns a "hitsounds" directory next to config.json
// (`<configDir>/hitsounds`, created on first launch together with a generated
// sample WAV); every audio file in it (.wav, .ogg, .mp3, .m4a or .flac) shows
// up as an option of the module's radio picker in the launcher mod menu, and a
// "Volume" slider controls playback loudness.
//
// The sound is a purely client-side overlay on top of the vanilla audio —
// the victim's own hurt sound is not cancelled or replaced, so picking "None"
// simply leaves the default behaviour. Audio is decoded and played by the
// Android framework (android.media.SoundPool) through JNI, so every format
// the platform decodes works with no extra decoder library inside the mod.
// SoundPool is Android's low-latency API for short game sound effects: the
// selected file is decoded once, asynchronously and off the game thread, when
// it is picked or the module is enabled, and each hit then costs only a
// single cheap play() call — so landing a hit never stalls the game thread
// with file I/O or codec setup (the lag the old per-hit MediaPlayer
// create/prepare/start path caused). SoundPool itself caps simultaneous
// streams and steals the oldest one, so no manual player tracking or
// per-tick cleanup is needed. All SoundPool calls are serialized behind a
// mutex so the attack hook and the config path can safely share the engine
// even if the engine dispatches them from different threads. On non-Android
// builds the audio engine compiles to a no-op.
class HitSoundModule : public Module {
public:
    HitSoundModule();
    ~HitSoundModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from the AttackEvent subscription when the local player attacks
    // an actor. `target` is the resolved hit target (null when the attack was
    // not aimed at an entity, e.g. creative instant mining of blocks). The
    // swing is only recorded here — no sound plays until the target's
    // hurt-time rises on a later tick, proving the damage landed.
    void onAttack(bedrocktools::sdk::Actor* target);

    // Called on every local-player tick to verify pending hits against the
    // victims' current hurt-time.
    void onTickCheck();

    // Directory the module watches; exposed for the menu description.
    const std::string& soundsDirectory() const { return m_dir; }

private:
    // A swing whose damage is not yet confirmed. Stored from the attack hook
    // and verified on the following ticks: once the victim's hurt-time rises
    // above m_baselineHurtTime, the hit connected and the sound plays.
    struct PendingHit {
        bedrocktools::sdk::Actor* target;
        int baselineHurtTime;
        std::chrono::steady_clock::time_point at;
    };
    void ensureSoundsDirectory();
    void writeSampleWav(const std::string& path) const;
    // Derives m_currentPath from m_selectedIndex + m_files ("" when None) and,
    // while the module is enabled, preloads the selection into the SoundPool.
    void refreshSelectionPath();

    std::string m_dir;
    std::vector<std::string> m_files; // refreshed by saveConfig (menu build / save)
    int m_selectedIndex = 0;          // 0 = None, i>=1 -> m_files[i-1]
    std::string m_currentPath;        // absolute path of the selected file, empty when None

    float m_volume = 0.8f;            // 0..1, clamped

    // Swings awaiting damage confirmation; only touched on the game thread
    // (attack hook + LocalPlayerTickEvent), so no extra synchronisation is
    // needed. Bounded to kMaxPendingHits entries.
    std::vector<PendingHit> m_pendingHits;
};

extern HitSoundModule* g_hitSound;

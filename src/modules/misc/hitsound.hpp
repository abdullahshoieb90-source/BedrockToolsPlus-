#pragma once

#include "../Module.hpp"
#include <string>
#include <vector>

// Hit Sound
//
// Lets the player pick a custom sound that plays whenever they land a melee
// hit on a mob or another player. The module owns a "hitsounds" directory
// next to config.json (`<configDir>/hitsounds`, created on first launch
// together with a generated sample WAV); every audio file in it (.wav, .ogg,
// .mp3, .m4a or .flac) shows up as an option of the module's radio picker in
// the launcher mod menu, and a "Volume" slider controls playback loudness.
//
// The sound is a purely client-side overlay on top of the vanilla audio —
// the victim's own hurt sound is not cancelled or replaced, so picking "None"
// simply leaves the default behaviour. Audio is decoded and played by the
// Android framework (android.media.MediaPlayer) through JNI, so every format
// the platform decodes works with no extra decoder library inside the mod.
// Playback happens on the native game thread: MediaPlayer is created,
// prepared synchronously and started per hit, finished players are reaped by
// polling isPlaying() on later ticks, and a hard cap on simultaneous players
// keeps a fast attack stream (or creative-mode clicks) from stacking
// unbounded decoders. All MediaPlayer calls are serialized behind a mutex so
// the attack hook and the tick hook can safely share the player list even if
// the engine dispatches them from different threads. On non-Android builds
// the audio engine compiles to a no-op.
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
    // not aimed at an entity, e.g. creative instant mining of blocks).
    void onAttack(void* target);

    // Called from the LocalPlayerTickEvent subscription; reaps finished
    // MediaPlayers so decoders are not held until the next hit.
    void onTick();

    // Directory the module watches; exposed for the menu description.
    const std::string& soundsDirectory() const { return m_dir; }

private:
    void ensureSoundsDirectory();
    void writeSampleWav(const std::string& path) const;
    // Derives m_currentPath from m_selectedIndex + m_files ("" when None).
    void refreshSelectionPath();

    std::string m_dir;
    std::vector<std::string> m_files; // refreshed by saveConfig (menu build / save)
    int m_selectedIndex = 0;          // 0 = None, i>=1 -> m_files[i-1]
    std::string m_currentPath;        // absolute path of the selected file, empty when None

    float m_volume = 0.8f;            // 0..1, clamped
};

extern HitSoundModule* g_hitSound;

#pragma once

#include "../Module.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Player Health — draws a live health indicator as a second nametag line,
// directly below the player's name and above the player's head (exactly the
// gap between the two). The health value is read primarily from the actor's
// live Mob/Health Attribute, falling back to synced entity data
// (ActorDataIDs::Health), accepting the int/float/short forms seen across
// Bedrock builds and protocol bridges. An uninitialized attribute (exactly
// 0.0f) is not trusted while the synced metadata still reports a non-zero
// health, so fresh actors no longer flash 0/20 hearts.
class PlayerHealthModule : public Module {
public:
    PlayerHealthModule();
    ~PlayerHealthModule() override;

    void onInit() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Driven from LocalPlayerTickEvent on the game thread (and directly by
    // the host-side unit test).
    void onLocalPlayerTick(void* localPlayer);

    // hearts | bar | numbers (serialized as radio value).
    std::string m_style = "hearts";
    int m_styleIndex = 0;
    // Force the nametag to stay visible without aiming at the player, like
    // the TNT Timer countdown. Off = vanilla nametag visibility rules.
    bool m_alwaysShow = true;
    // The local player's own nametag is only rendered in third person, and
    // writing it conflicts with the Nick module, so it is off by default.
    bool m_showSelf = false;
    // Maximum distance in blocks; players farther away are skipped. 0 = no
    // limit.
    int m_range = 64;

private:
    struct TrackedPlayer {
        std::string baseName;      // nametag as it was before our line
        std::string lastWritten;   // full string we wrote last (base + '\n' + line)
        bool hadAlwaysShowItem = false;
        std::int8_t alwaysShowValue = 0;
        bool forcedAlwaysShow = false;
        float maxObserved = 20.0f;
    };

    static PlayerHealthModule* s_instance;

    std::unordered_map<void*, TrackedPlayer> m_tracked;
    int m_refreshTicks = 0;
    bool m_selfNametagPatchActive = false;

    void restoreTracked(const std::vector<void*>& liveActors);
    void restoreOne(void* actor, TrackedPlayer& state);
    void updateSelfNametagPatch();
    void releaseSelfNametagPatch();
};

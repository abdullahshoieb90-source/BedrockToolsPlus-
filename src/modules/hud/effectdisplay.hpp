#pragma once

// Effect Display module
//
// Renders the local player's active status effects (potion effects) as a HUD
// list. Every row shows a colored icon, the effect name, the amplifier and
// the remaining duration. The full modern Bedrock effect set is supported
// (36 named effects) and any other valid effect id still renders with a
// generic style, so the module is never limited by the built-in table.

#include "../Module.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class EffectDisplayModule : public Module {
public:
    EffectDisplayModule();
    ~EffectDisplayModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void onLocalPlayerTick(void* localPlayer);

    // --- display config (the m_ prefix is stripped for the mod menu) ---
    float hudPosX = 15.0f;
    float hudPosY = 60.0f;
    bool isHudModule = true;

    float m_size = 28.0f;        // text size
    float m_iconSize = 30.0f;    // icon circle diameter
    float m_spacing = 6.0f;      // gap between rows
    float m_panelWidth = 210.0f;

    bool m_showIcons = true;
    bool m_showNames = true;
    bool m_showTimers = true;
    bool m_showAmplifier = true;
    bool m_romanNumerals = true;

    bool m_background = true;
    float m_backgroundOpacity = 0.45f;
    std::uint32_t m_backgroundColorHex = 0xFF000000;
    std::uint32_t m_textColorHex = 0xFFFFFFFF;

    std::string m_direction = "Vertical";   // Vertical or Horizontal
    int m_maxEffects = 20;                  // 0 = unlimited
    bool m_hideAmbient = false;
    bool m_hideInvisible = true;
    bool m_hideExpiring = false;
    int m_expireSeconds = 10;
    bool m_sortByDuration = true;

    static EffectDisplayModule* getInstance();

private:
    struct EffectInfo {
        int id = 0;
        int duration = 0;   // ticks remaining, -1 = infinite
        int amplifier = 0;
        bool visible = true;
        bool ambient = false;
    };

    void refresh(void* localPlayer);

    std::mutex m_mutex;
    std::vector<EffectInfo> m_effects;

    int m_refreshCooldown = 0;
    int m_tickCounter = 0;
    void* m_lastPlayer = nullptr;

    // Duration sync detection: depending on the game version the client may
    // or may not tick effect durations down locally.
    bool m_durationsTick = true;
    int m_probeId = -1;
    int m_probeRaw = 0;
    int m_probeTick = 0;
    struct StaticBase { int raw = 0; int tick = 0; };
    std::unordered_map<int, StaticBase> m_staticBase;

    static EffectDisplayModule* s_instance;
};

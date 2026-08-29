#pragma once

#include "../Module.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bedrocktools::sdk { class Player; }
namespace bedrocktools::hooks { struct State; }

class EffectDisplayModule final : public Module {
public:
    // A single active status effect. `amplifier` is the raw Bedrock amplifier
    // (0 == level I) or -1 when the game build's memory layout did not allow
    // reading it with confidence; the level is then hidden instead of guessed.
    struct ActiveEffect {
        std::uint32_t id = 0;
        int durationTicks = 0;
        int amplifier = -1;

        bool hasLevel() const { return amplifier >= 0; }
        int level() const { return amplifier + 1; }
    };

    // Per-effect bookkeeping used for animations and the remaining-time bar.
    // Timestamps are kept as time points so frame-to-frame deltas stay exact.
    struct EffectTiming {
        std::chrono::steady_clock::time_point appearAt{};
        std::chrono::steady_clock::time_point lastSeenAt{};
        int maxDurationTicks = 0;   // longest observed duration (bar reference)
        int amplifier = -1;         // potency the timing above was learned for
    };

    EffectDisplayModule();
    ~EffectDisplayModule() override;

    void onInit() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void updateEffects(bedrocktools::sdk::Player* player);

    // Detour installed on MinecraftUIRenderContext::drawImage. Public (and
    // with the original pointer kept as a class static) so the host-side
    // unit test can drive it directly with a fake render context.
    static void drawImageDetour(void* context, const void* texture, const void* position,
                                const void* size, const void* uv, const void* uvSize, bool tiled);
    static void (*s_originalDrawImage)(void* context, const void* texture, const void* position,
                                       const void* size, const void* uv, const void* uvSize, bool tiled);

private:
    void registerResources();
    void installVanillaBarHook();
    void installVanillaBarFilter();

    // Detour used to suppress the vanilla status-effect (potion) bar by
    // skipping the whole HUD draw. It is a static member so it can read
    // private state (m_hideVanillaHud) directly.
    static void renderPotionEffectsDetour(void* self, void* renderContext, void* screenView, float posX, float posY);

    std::mutex m_mutex;
    std::vector<ActiveEffect> m_effects;
    std::unordered_map<std::uint32_t, EffectTiming> m_timing;
    std::chrono::steady_clock::time_point m_lastChangeAt{};
    bool m_resourcesRegistered = false;

    // Animation clocks (render-thread only).
    std::chrono::steady_clock::time_point m_lastFrameTime{};
    float m_pulsePhase = 0.0f;

    float hudPosX = 8.0f;
    float hudPosY = 70.0f;
    bool isHudModule = true;

    // While enabled, the vanilla status-effect (potion) bar of the game is not
    // drawn, so it can never overlap this module's own effect panel; disabling
    // the module brings the vanilla bar straight back because nothing outside
    // the module is ever modified. This only has an effect while the module
    // itself is enabled; it defaults to true.
    bool m_hideVanillaHud = true;

    float m_scale = 1.0f;
    // Multiplier applied on top of m_scale to just the potion icons, so their
    // size can be tuned without resizing the whole panel. 1.0 == vanilla 18px.
    float m_iconScale = 1.0f;
    float m_width = 210.0f;
    float m_backgroundOpacity = 0.82f;
    bool m_showBackground = true;
    bool m_showIcons = true;
    bool m_showLevel = true;
    bool m_romanLevels = true;
    bool m_hideLevelOne = false;
    bool m_showProgressBar = true;
    bool m_animate = true;
    bool m_preview = false;
    int m_maxVisible = 36;

    // Effect-name language, persisted as a radio: 0 follows the game's own
    // language setting (options.txt `game_language`), 1..N pin one of the
    // languages from effecti18n.hpp. See saveConfig()/loadConfig().
    int m_language = 0;

    // Vanilla potion-bar suppression, layer 1 (see installVanillaBarHook): a
    // hook on HudScreen::_renderStatusEffects. The hook itself is kept
    // installed for the whole session; the detour decides per-frame whether
    // to skip the vanilla draw call.
    bedrocktools::hooks::State* m_vanillaBarHook = nullptr;
    bool m_vanillaBarHooked = false;

    // Vanilla potion-bar suppression, layer 2 (see installVanillaBarFilter):
    // a filter on MinecraftUIRenderContext::drawImage that swallows the bar's
    // texture draws while the module is enabled. Works without any per-build
    // byte pattern (the virtual is resolved through the class's RTTI name).
    bedrocktools::hooks::State* m_vanillaBarFilterHook = nullptr;
    bool m_vanillaBarFilterHooked = false;
};

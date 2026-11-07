#pragma once

#include "../Module.hpp"

#include <atomic>
#include <cstdint>

// Recolors the red "hurt" overlay the game draws on entities (the local player
// included) when they take damage, using the same actor shader-parameter path
// the vanilla damage flash travels through. The game computes the flash's
// fade per frame, so the module only replaces the RGB tint and keeps the
// original alpha — the hurt flash keeps its vanilla timing and intensity.
class DamageColorModule final : public Module {
public:
    DamageColorModule();
    ~DamageColorModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    std::uint32_t damageColor() const;
    bool rainbowEnabled() const;
    float rainbowSpeedValue() const;

private:
    std::atomic<std::uint32_t> m_damageColor{0xFF00E5FFu};
    std::atomic<bool> m_rainbow{false};
    std::atomic<float> m_rainbowSpeed{0.20f};

    void* m_entityTarget = nullptr;
    void* m_actorTarget = nullptr;
    void* m_foilTarget = nullptr;
    void* m_uiTarget = nullptr;
    bool m_entityHooked = false;
    bool m_actorHooked = false;
    bool m_foilHooked = false;
    bool m_uiHooked = false;

    void installHooks();
};

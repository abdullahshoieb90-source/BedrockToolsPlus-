#pragma once
#include "../Module.hpp"
#include <bedrocktools/sdk/world/Actor.hpp>
#include <cstdint>

class DamageCircleModule final : public Module {
public:
    DamageCircleModule();
    ~DamageCircleModule() override;
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    float effectDuration = 0.65f;
    float effectSize = 1.0f;
    float effectSpeed = 1.0f;
    int style = 0;
    std::uint32_t effectColor = 0xFFFFFFFFu;
    bool rainbow = false;
    float rainbowSpeed = 0.20f;
    bool showPlayers = true;
    bool showMobs = true;

    void onAttack(bedrocktools::sdk::Actor* target);

private:
    bool m_patched = false;
    void* m_patchTarget = nullptr;
};

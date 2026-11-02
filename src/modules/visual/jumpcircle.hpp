#pragma once
#include "../Module.hpp"
#include <cstdint>

class JumpCircleModule final : public Module {
public:
    JumpCircleModule();
    ~JumpCircleModule() override;
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    int style = 0;
    float effectDuration = 0.65f;
    std::uint32_t effectColor = 0xFFFFFFFFu;
    bool rainbow = false;
    float rainbowSpeed = 0.20f;
    bool persistent = false;
private:
    bool m_patched = false;
    void* m_patchTarget = nullptr;
};

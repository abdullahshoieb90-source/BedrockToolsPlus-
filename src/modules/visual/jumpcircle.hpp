#pragma once
#include "../Module.hpp"

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
private:
    bool m_patched = false;
    void* m_patchTarget = nullptr;
};

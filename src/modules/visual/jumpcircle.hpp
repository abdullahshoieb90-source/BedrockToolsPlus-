#pragma once
#include "../Module.hpp"

class JumpCircleModule final : public Module {
public:
    JumpCircleModule();
    ~JumpCircleModule() override;
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
private:
    bool m_patched = false;
    void* m_patchTarget = nullptr;
};

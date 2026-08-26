#pragma once
#include "../Module.hpp"

class ThirdPersonNametagModule : public Module {
private:
    bool m_patched;

    void applyPatch();
    void removePatch();

public:
    ThirdPersonNametagModule();
    ~ThirdPersonNametagModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
};

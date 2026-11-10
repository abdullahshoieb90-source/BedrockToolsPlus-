#pragma once
#include "modules/Module.hpp"
#include <string>

// The renderer and player tracking are shared with Wings; neither module edits skins.
class CustomCapesModule : public Module {
public:
    CustomCapesModule();
    ~CustomCapesModule() override;
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    const std::string& capePath() const { return m_capePath; }
private:
    std::string m_capePath;
};

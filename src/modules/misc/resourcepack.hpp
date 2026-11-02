#pragma once

#include "core/Runtime.hpp"
#include "resources/ResourcePackManager.hpp"
#include "launcher/ModuleMenu.hpp"
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <filesystem>
#include <pl/ModMenu.hpp>

namespace bedrocktools::modules {

// وحدة الـ Resource Pack: تقوم بمسح المجلد الخاص بالـ Resource Pack الخاص بـ BedrockToolsPlus
// داخل مجلد الموارد الخاص بالـ Launcher، وتسجيل الملفات المكتشفة في الـ Launcher Mod Menu
// ليتم تطبيقها على اللعبة الأصلية.
class ResourcePackModule : public Module {
public:
    ResourcePackModule();
    ~ResourcePackModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    void registerResources();

    static ResourcePackModule* g_resourcePack;
    bool m_resourcesRegistered = false;
};

extern ResourcePackModule* g_resourcePack;

} // namespace bedrocktools::modules

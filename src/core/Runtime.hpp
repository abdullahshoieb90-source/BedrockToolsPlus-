#pragma once

#include <pl/Mod.hpp>
#include <filesystem>

namespace bedrocktools::core {

class Runtime {
public:
    static Runtime& get();
    bool load(pl::mod::ModContext& context);
    bool enable(pl::mod::ModContext& context);
    bool disable(pl::mod::ModContext& context);
    bool unload(pl::mod::ModContext& context);
    void minecraftLoaded();
    const std::filesystem::path& resourceDirectory() const noexcept;
    
    // يرجع مجلد الـ Resource Pack الخاص بـ BedrockToolsPlus داخل مجلد الـ Launcher
    // (عادةً يكون <resourceDir>/resource_packs أو <configDir>/resource_packs)
    const std::filesystem::path& resourcePackDirectory() const noexcept;

private:
    bool resolveSignatures();
    bool install();
    bool launcherContext() const;
    void wireEvents();
    std::filesystem::path mResourceDirectory;
    std::filesystem::path mResourcePackDirectory;
};

}

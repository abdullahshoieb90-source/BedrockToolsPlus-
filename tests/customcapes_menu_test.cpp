// Menu-registration test for the Custom Capes module.
//
// The module's settings are not hand-registered: registerModulesWithLauncher()
// derives the menu entries from whatever saveConfig() emits. That means the
// *shape* of the emitted JSON decides what the user can do in the launcher:
//
//   * a string containing a comma becomes a Radio,
//   * a boolean whose key contains "button" becomes a Button,
//   * everything else becomes a toggle / slider / text field.
//
// This test drives the real registration path against a recording stub of the
// launcher API and pins:
//   * the Cape picker lists every cape file plus "None", with friendly labels,
//   * the fit picker is a Radio with exactly Cover/Contain/Stretch,
//   * "Reload" and "Copy Folder Path" are Buttons that start unpressed,
//   * the status row reports how many capes were found,
//   * pressing Reload re-reads the folder (a new file is picked up),
//   * a non-PNG file is never offered as a working cape.
//
// Needs the real preloader and nlohmann_json headers (the host fakes only
// cover what single modules use; this test drives the whole registration
// path). run_tests.sh skips it when those headers are missing.
//
// Build and run standalone (package paths from the xmake cache):
//
//   PRE_LOADER=$(echo ~/.xmake/packages/p/preloader/main/*/include)
//   JSON=$(echo ~/.xmake/packages/n/nlohmann_json/v3.11.3/*/include)
//   g++ -std=c++20 -I src -I include -I third_party -I "$PRE_LOADER" -I "$JSON"
//       tests/customcapes_menu_test.cpp src/modules/player/customcapes.cpp
//       src/launcher/ModuleMenu.cpp -o /tmp/customcapes_menu_test
//   /tmp/customcapes_menu_test

#include "modules/player/customcapes.hpp"
#include "modules/ModuleRegistry.hpp"
#include "config/ConfigManager.hpp"
#include "launcher/ModuleMenu.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <pl/ModMenu.hpp>

// Declarations only: the implementation lives in customcapes.cpp, which is
// linked into this test as a second translation unit.
#include <stb/stb_image_write.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace bedrocktools::config {
class ConfigManager;
}
static std::string g_testConfigPath;

// ---------------------------------------------------------------------------
// Launcher stub: records the module definition and its config entries.
// ---------------------------------------------------------------------------

namespace {

struct CapturedConfig {
    std::string key;
    std::string displayName;
    pl::modmenu::ConfigType type{};
    std::string defaultValue;
    std::string minValue;
    std::string maxValue;
    std::string dependsOn;
};

struct CapturedModule {
    bool registered = false;
    std::string moduleId;
    std::string displayName;
    std::string description;
    bool defaultEnabled = false;
    std::vector<CapturedConfig> configs;
};

CapturedModule g_captured;

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

const CapturedConfig* findConfig(const std::string& key) {
    for (const auto& cfg : g_captured.configs) {
        if (cfg.key == key) return &cfg;
    }
    return nullptr;
}

} // namespace

namespace pl::modmenu {
bool registerModule(const ModuleInfo& info) {
    g_captured.registered = true;
    g_captured.moduleId = info.moduleId;
    g_captured.displayName = info.displayName;
    g_captured.description = info.description;
    g_captured.defaultEnabled = info.defaultEnabled;
    g_captured.configs.clear();
    for (const auto& cfg : info.configs) {
        CapturedConfig captured;
        captured.key = cfg.key;
        captured.displayName = cfg.displayName;
        captured.type = cfg.type;
        captured.defaultValue = cfg.defaultValue;
        captured.minValue = cfg.minValue;
        captured.maxValue = cfg.maxValue;
        captured.dependsOn = cfg.dependsOn;
        g_captured.configs.push_back(std::move(captured));
    }
    return true;
}

void unregisterModule(std::string_view) {}
void setModuleEnabled(std::string_view, bool) {}
void submitDrawCommands(std::string_view, std::span<const DrawCommand>) {}
bool registerFont(std::string_view, std::span<const unsigned char>) { return true; }
bool registerImage(std::string_view, std::span<const unsigned char>, int, int) { return true; }
bool registerButton(const ButtonInfo&) { return true; }
void unregisterButton(std::string_view) {}
void requestOpenMenu() {}
void requestCloseMenu() {}

} // namespace pl::modmenu

// The module calls these while loading; the host has no video or player, so
// they are inert.
namespace bedrocktools::config {
ConfigManager::~ConfigManager() = default;
std::string ConfigManager::getConfigPath() const { return g_testConfigPath; }
} // namespace bedrocktools::config

namespace bedrocktools::events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace bedrocktools::events

namespace bedrocktools::launcher {
void* javaVm() { return nullptr; }
// ModuleMenu asks the overlay to rebuild external buttons after a text change;
// no overlay exists on the host.
void refreshExternalButtonsForModule(std::string_view) {}
} // namespace bedrocktools::launcher

namespace bedrocktools::config {
// ModuleMenu saves after every change; the host test keeps its own JSON.
void ConfigManager::save() {}
} // namespace bedrocktools::config

// ModuleRegistry is a plain container; the production .cpp would drag in every
// other module, so the members ModuleMenu uses are defined here.
ModuleRegistry& ModuleRegistry::get() {
    static ModuleRegistry instance;
    return instance;
}

Module* ModuleRegistry::find(std::string_view id) const {
    const auto it = mById.find(id);
    return it == mById.end() ? nullptr : it->second;
}

const std::vector<Module*>& ModuleRegistry::modules() const { return mView; }

bool ModuleRegistry::keybindBlocked() const { return false; }

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

int main() {
    const std::string root = "/tmp/bt-customcapes-menu-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root + "/capes");
    g_testConfigPath = root + "/config.json";

    // Two real PNGs (a 0-byte placeholder would be rejected by the header
    // check, which is exactly what the last part of this test verifies) and
    // one file that only pretends to be a PNG.
    const auto writePng = [&](const std::string& path, int width, int height) {
        std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4, 180);
        for (std::size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
        stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4);
    };
    writePng(root + "/capes/Alpha Cape.png", 32, 32);
    writePng(root + "/capes/Beta Cape.png", 64, 32);
    {
        std::ofstream out(root + "/capes/Broken.png", std::ios::binary);
        out << "this is not a png";
    }

    auto& registry = ModuleRegistry::get();
    auto* mod = &registry.emplace<CustomCapesModule>();
    mod->onInit();

    // Save the config once so the module emits its full key set (the launcher
    // reads it through saveConfig exactly like this).
    nlohmann::json cfg;
    mod->saveConfig(cfg);

    std::printf("cape picker\n");
    check(cfg.contains("m_cape"), "the module emits a cape picker key");
    const std::string picker = cfg["m_cape"].get<std::string>();
    check(picker.find("None") != std::string::npos, "the picker always offers None");
    check(picker.find("Alpha Cape") != std::string::npos &&
              picker.find("Beta Cape") != std::string::npos,
          "every cape file is offered by name (without .png)");
    check(picker.find("Broken") != std::string::npos,
          "broken files still appear (the picker is a file list, not a decoder)");
    check(picker.find(".png") == std::string::npos,
          "labels are stripped of the .png suffix");

    std::printf("fit picker\n");
    check(cfg.contains("m_capeFit"), "the module emits a fit key");
    const std::string fit = cfg["m_capeFit"].get<std::string>();
    check(fit == "0,Cover,Contain,Stretch", "fit value is a 3-option radio (" + fit + ")");

    std::printf("action buttons\n");
    check(cfg.contains("m_reloadButton") && cfg["m_reloadButton"].is_boolean(),
          "reload is emitted as a boolean (rendered as a Button)");
    check(cfg.contains("m_copyPathButton") && cfg["m_copyPathButton"].is_boolean(),
          "copy-path is emitted as a boolean (rendered as a Button)");
    check(cfg.contains("m_folderPath") && cfg["m_folderPath"].is_string() &&
              cfg["m_folderPath"].get<std::string>().find("capes") != std::string::npos,
          "the capes folder path is shown as a text row");

    std::printf("status row\n");
    check(cfg.contains("m_status") && cfg["m_status"].is_string(),
          "a human readable status row is emitted");
    const std::string status = cfg["m_status"].get<std::string>();
    check(status.find("3 capes found") != std::string::npos,
          "status counts the files found (" + status + ")");

    // ---------------------------------------------------------------
    // Pressing Reload picks up a file added after launch.
    // ---------------------------------------------------------------
    std::printf("reload button\n");
    writePng(root + "/capes/Gamma Cape.png", 16, 16);
    check(mod->capeCount() == 3, "the new file is not visible before the reload");
    check(mod->onMenuConfigChanged("m_reloadButton", "true"), "the press is handled");
    check(mod->capeCount() == 4, "the reload re-scans the folder");
    mod->saveConfig(cfg);
    check(cfg["m_cape"].get<std::string>().find("Gamma Cape") != std::string::npos,
          "the new cape appears in the picker after the reload");

    // A released button must NOT re-scan (the launcher reports up events too).
    writePng(root + "/capes/Delta Cape.png", 16, 16);
    mod->onMenuConfigChanged("m_reloadButton", "false");
    check(mod->capeCount() == 4, "a button release does not trigger a second reload");

    // ---------------------------------------------------------------
    // Selecting a cape applies it, selecting a broken one does not.
    // ---------------------------------------------------------------
    std::printf("selection\n");
    nlohmann::json select;
    select["m_cape"] = "1,None,Alpha Cape,Beta Cape,Broken,Gamma Cape";
    mod->loadConfig(select);
    check(mod->loadedFile() == "Alpha Cape.png",
          "the label resolves back to its file (" + mod->loadedFile() + ")");

    nlohmann::json broken;
    broken["m_cape"] = "3,None,Alpha Cape,Beta Cape,Broken,Gamma Cape";
    mod->loadConfig(broken);
    check(mod->loadFailed() && !mod->capeLoaded(),
          "a broken file is rejected by the header check (no pixels decoded)");
    check(mod->loadedFile().empty(),
          "nothing is reported as loaded while the file cannot be decoded");

    nlohmann::json fitThenCape;
    fitThenCape["m_capeFit"] = "1,Cover,Contain,Stretch";
    fitThenCape["m_cape"] = "2,None,Alpha Cape,Beta Cape,Broken,Gamma Cape";
    mod->loadConfig(fitThenCape);
    check(mod->fitMode() == customcapes::FitMode::Contain, "the fit change is applied");
    check(mod->loadedFile() == "Beta Cape.png", "the cape change is applied in the same load");

    // ---------------------------------------------------------------
    // A config that flips the module on/off used to re-enter the module from
    // Module::loadConfig -> onEnable/onDisable. With a non-recursive lock that
    // deadlocks, so this is exercised deliberately (it hangs, not fails).
    // ---------------------------------------------------------------
    std::printf("enable/disable re-entrancy\n");
    {
        nlohmann::json toggled;
        toggled["masterEnabled"] = true;
        toggled["keybindActive"] = true;
        toggled["m_cape"] = "1,None,Alpha Cape,Beta Cape,Broken,Gamma Cape";
        mod->loadConfig(toggled);
        check(mod->masterEnabled && mod->enabled, "enabling through the config applies");

        toggled["masterEnabled"] = false;
        mod->loadConfig(toggled);
        check(!mod->masterEnabled && !mod->enabled, "disabling through the config applies");
    }

    std::printf("launcher registration\n");
    registerModulesWithLauncher();
    check(g_captured.registered, "the module registers with the launcher");
    check(g_captured.moduleId == "bedrocktoolsplus.Custom Capes",
          "the module id is unchanged (" + g_captured.moduleId + ")");

    const CapturedConfig* capeEntry = findConfig("m_cape");
    check(capeEntry != nullptr && capeEntry->type == pl::modmenu::ConfigType::Radio,
          "the cape picker is registered as a Radio");
    const CapturedConfig* fitEntry = findConfig("m_capeFit");
    check(fitEntry != nullptr && fitEntry->type == pl::modmenu::ConfigType::Radio,
          "the fit picker is registered as a Radio");
    check(fitEntry == nullptr || fitEntry->minValue == "Cover,Contain,Stretch",
          "the fit radio uses the option list from the module, not a free text field");
    const CapturedConfig* reloadEntry = findConfig("m_reloadButton");
    check(reloadEntry != nullptr && reloadEntry->type == pl::modmenu::ConfigType::Button,
          "reload is registered as a Button");
    check(reloadEntry == nullptr || reloadEntry->defaultValue == "false",
          "the reload button starts unpressed");
    const CapturedConfig* folderEntry = findConfig("m_copyPathButton");
    check(folderEntry != nullptr && folderEntry->type == pl::modmenu::ConfigType::Button,
          "copy-path is registered as a Button");
    const CapturedConfig* statusEntry = findConfig("m_status");
    check(statusEntry != nullptr && statusEntry->type == pl::modmenu::ConfigType::Text,
          "the status row is registered as text");
    check(statusEntry == nullptr || statusEntry->dependsOn.empty(),
          "the status row is not nested under a toggle");

    std::filesystem::remove_all(root);
    std::printf("\n%s\n", g_failures == 0 ? "all custom capes menu tests passed"
                                          : "SOME MENU TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}

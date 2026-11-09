// Smoke test: every registered module must survive the launcher
// registration path. A single module whose saveConfig() emits a config entry
// the launcher rejects aborts registerModule() for the whole mod (the menu then
// shows *no* modules at all), so this test registers all of them and reports
// per-module failures.
//
// It needs the real preloader and nlohmann_json headers (ModuleBuilder and the
// JSON surface the menu builder uses), so run_tests.sh skips it without them.
//
// Build and run standalone:
//   PRE_LOADER=$(echo ~/.xmake/packages/p/preloader/main/*/include)
//   JSON=$(echo ~/.xmake/packages/n/nlohmann_json/v3.11.3/*/include)
//   g++ -std=c++20 -I src -I include -I third_party -I "$PRE_LOADER" -I "$JSON"
//       tests/module_registration_smoke_test.cpp
//       src/launcher/ModuleMenu.cpp src/modules/ModuleRegistry.cpp
//       ... (every module .cpp) -o /tmp/registration_smoke
//   /tmp/registration_smoke

#include "modules/ModuleRegistry.hpp"
#include "launcher/ExternalButtonRefresh.hpp"
#include "launcher/ModuleMenu.hpp"
#include "config/ConfigManager.hpp"
#include "core/Runtime.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>

#include <pl/ModMenu.hpp>

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

// What the launcher bridge enforces before it accepts a module definition.
bool isValidConfigType(pl::modmenu::ConfigType type) {
    switch (type) {
        case pl::modmenu::ConfigType::Toggle:
        case pl::modmenu::ConfigType::SliderInt:
        case pl::modmenu::ConfigType::SliderFloat:
        case pl::modmenu::ConfigType::Radio:
        case pl::modmenu::ConfigType::Color:
        case pl::modmenu::ConfigType::Keybind:
        case pl::modmenu::ConfigType::Text:
        case pl::modmenu::ConfigType::Button:
            return true;
    }
    return false;
}

struct CapturedEntry {
    std::string key;
    std::string displayName;
    pl::modmenu::ConfigType type{};
    std::string defaultValue;
    std::string minValue;
    std::string maxValue;
    std::string dependsOn;
};

std::vector<std::string> g_registered;
std::string g_lastModule;
std::vector<CapturedEntry> g_lastEntries;
std::vector<std::string> g_problems;

void checkModule(const std::string& id, const std::string& displayName,
                 const std::vector<CapturedEntry>& entries) {
    // Mirrors ModMenuBridge::RegisterModule validation.
    constexpr std::size_t kMaxConfigStringLength = 2048;
    constexpr std::size_t kMaxConfigCount = 128;
    constexpr std::size_t kMaxDescriptionLength = 1024;

    if (id.empty() || id.size() > 160) {
        g_problems.push_back("module id is empty or too long: '" + id + "'");
    }
    if (displayName.empty() || displayName.size() > 160) {
        g_problems.push_back("module display name is empty or too long: '" + displayName + "'");
    }
    (void)kMaxDescriptionLength;
    if (entries.size() > kMaxConfigCount) {
        g_problems.push_back(std::to_string(entries.size()) + " config entries (max 128)");
    }

    for (const auto& entry : entries) {
        const std::string where = "  [" + entry.key + "] ";
        if (entry.key.empty()) g_problems.push_back(where + "empty config key");
        if (entry.key.size() > kMaxConfigStringLength) g_problems.push_back(where + "key too long");
        if (entry.displayName.empty()) g_problems.push_back(where + "empty display name");
        if (entry.displayName.size() > kMaxConfigStringLength) {
            g_problems.push_back(where + "display name too long");
        }
        if (entry.defaultValue.size() > kMaxConfigStringLength) {
            g_problems.push_back(where + "default value too long");
        }
        if (entry.minValue.size() > kMaxConfigStringLength) {
            g_problems.push_back(where + "min value too long");
        }
        if (entry.maxValue.size() > kMaxConfigStringLength) {
            g_problems.push_back(where + "max value too long");
        }
        if (entry.dependsOn.size() > kMaxConfigStringLength) {
            g_problems.push_back(where + "depends_on too long");
        }
        if (!isValidConfigType(entry.type)) {
            g_problems.push_back(where + "invalid config type");
        }
        if (entry.type == pl::modmenu::ConfigType::Radio && entry.minValue.empty()) {
            g_problems.push_back(where + "radio without options");
        }
    }
}

} // namespace

// ---- launcher stub --------------------------------------------------------

namespace pl::modmenu {

bool registerModule(const ModuleInfo& info) {
    g_lastModule = info.moduleId;
    g_lastEntries.clear();
    for (const auto& cfg : info.configs) {
        CapturedEntry entry;
        entry.key = cfg.key;
        entry.displayName = cfg.displayName;
        entry.type = cfg.type;
        entry.defaultValue = cfg.defaultValue;
        entry.minValue = cfg.minValue;
        entry.maxValue = cfg.maxValue;
        entry.dependsOn = cfg.dependsOn;
        g_lastEntries.push_back(std::move(entry));
    }
    g_registered.push_back(info.moduleId);
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

// ---- host stubs for the engine/launcher surface the modules touch ----------

namespace pl::modmenu {
void submitHudEditorElements(std::string_view, std::span<const HudEditorElement>) {}
HudSurfaceSize getHudSurfaceSize() { return {}; }
bool setConfigSchemaJson(std::string_view, std::string_view) { return true; }
void clearConfigSchema(std::string_view) {}
bool setFloatingMenuButtonVisible(bool) { return true; }
} // namespace pl::modmenu

namespace bedrocktools::events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace bedrocktools::events

namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId) { return 0; }
} // namespace bedrocktools::memory

namespace bedrocktools::core::gamehooks {
void* clientInstance() { return nullptr; }
} // namespace bedrocktools::core::gamehooks

// Several modules read the resource directory (fonts, wing/effect assets).
namespace bedrocktools::core {
namespace {
Runtime& runtimeInstance() {
    static Runtime instance; // never constructed by the real code path here
    return instance;
}
} // namespace
Runtime& Runtime::get() { return runtimeInstance(); }
const std::filesystem::path& Runtime::resourceDirectory() const noexcept {
    static const std::filesystem::path empty;
    return empty;
}
} // namespace bedrocktools::core

namespace pl::memory {
void* resolveVtableFunction(std::string_view, std::size_t, std::string_view) { return nullptr; }
} // namespace pl::memory

// Hook helpers: the modules install detours at init; on the host every one is
// a no-op that reports "not installed".
#include <pl/memory/Hook.hpp>
namespace pl::memory {
int hook(void*, void*, void**, pl::memory::HookPriority) { return 0; }
bool unhook(void*, void*) { return true; }
} // namespace pl::memory


// The launcher surface (javaVm, refreshExternalButtonsForModule, failure
// reporting) comes from src/launcher/ExternalButtonRefresh.cpp, which this test
// links. On the host that file needs no JVM. A rejected definition hides its
// module from the menu entirely, so the count must stay at zero.

// ---- main -----------------------------------------------------------------

// A module that misbehaves the way a real one can: onInit() throws (bad
// asset), saveConfig() throws (unexpected JSON state). Before the registration
// pass was made fault tolerant, either of these aborted the whole pass and the
// launcher ended up with an empty menu.
class BrokenModule final : public Module {
public:
    BrokenModule() : Module("Broken Test Module", "exists only in the host test") {}

    void onInit() override { throw std::runtime_error("simulated init failure"); }

    void saveConfig(nlohmann::json& j) override {
        Module::saveConfig(j);
        throw std::runtime_error("simulated saveConfig failure");
    }
};

int main() {
    // Registration runs after ConfigManager::load() in Runtime::install().
    registerAllModules();
    auto& registry = ModuleRegistry::get();
    const std::size_t healthyCount = registry.modules().size();
    registry.emplace<BrokenModule>();
    const auto& modules = ModuleRegistry::get().modules();
    std::printf("modules registered in the registry: %zu (%zu healthy + 1 broken)\n",
                modules.size(), healthyCount);

    // initialize() must survive the throwing module too: it runs before the
    // menu is registered, so an escaping exception used to leave the launcher
    // with no modules at all.
    registry.initialize();

    // Emulate the launcher: it registers module by module. Each module must
    // produce a definition the bridge accepts; one bad entry hides the whole
    // menu in game, which is why every module is checked, not just one.
    registerModulesWithLauncher();

    std::printf("menus registered: %zu\n", g_registered.size());
    for (const auto& id : g_registered) std::printf("  + %s\n", id.c_str());

    // Exactly the broken module may be missing; every healthy one must still
    // be registered. This is the regression guard for "all modules vanished":
    // one bad module must never empty the menu.
    const std::size_t rejected = static_cast<std::size_t>(
            bedrocktools::launcher::moduleRegistrationFailureCount());
    if (rejected != 1) {
        g_problems.push_back("expected exactly the broken module to be rejected, got " +
                             std::to_string(rejected));
    }
    if (g_registered.size() != healthyCount) {
        g_problems.push_back("registered " + std::to_string(g_registered.size()) +
                             " menus, expected " + std::to_string(healthyCount));
    }
    for (const auto& id : g_registered) {
        if (id == "bedrocktoolsplus.Broken Test Module") {
            g_problems.push_back("the broken module was registered anyway");
        }
    }

    // The pass above only keeps the last module definition, so validate every
    // module's config keys the same way the bridge does: by checking the JSON
    // saveConfig() emits (the menu builder turns that JSON into entries).
    for (auto* mod : modules) {
        if (!mod->showInMenu) continue;
        if (std::string(mod->name) == "Broken Test Module") continue;
        nlohmann::json j;
        mod->saveConfig(j);
        if (j.empty()) {
            g_problems.push_back(std::string(mod->name) + ": saveConfig emitted nothing");
            continue;
        }
        for (auto& [key, value] : j.items()) {
            if (key.empty()) {
                g_problems.push_back(std::string(mod->name) + ": empty config key");
            }
            if (key.size() > 2048) {
                g_problems.push_back(std::string(mod->name) + "." + key + ": key too long");
            }
            if (value.is_string() && value.get<std::string>().size() > 2048) {
                g_problems.push_back(std::string(mod->name) + "." + key +
                                     ": value too long for the launcher");
            }
        }
    }

    if (!g_problems.empty()) {
        std::printf("\nPROBLEMS:\n");
        for (const auto& problem : g_problems) std::printf("  FAIL %s\n", problem.c_str());
        ++g_failures;
    }

    std::printf("\n%s\n", g_failures == 0 ? "all modules register cleanly"
                                          : "SOME MODULES FAILED TO REGISTER");
    return g_failures == 0 ? 0 : 1;
}

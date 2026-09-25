// Host test for the mod-menu registration of the world overlay modules.
// It runs the real registerModulesWithLauncher() against the three modules and
// inspects the controls the launcher is handed, so a setting whose name the
// slider heuristics cannot guess cannot silently get a useless range.
//
// The three modules are compiled as separate translation units: they each keep
// their state in an unnamed namespace, which would collide if they were
// #included into this one.
//
// Build: g++ -std=c++20 -I include -I src -I tests/fakepl -I tests/fakejson
//        tests/modulemenu_test.cpp src/modules/visual/chunkborder.cpp
//        src/modules/visual/breadcrumbs.cpp src/modules/visual/lightoverlay.cpp
//        -o /tmp/modulemenu_test
// Run:   /tmp/modulemenu_test

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "bedrocktools/events/EventBus.hpp"
#include "bedrocktools/memory/Signatures.hpp"
#include "bedrocktools/sdk/Offsets.hpp"
#include "bedrocktools/sdk/Types.hpp"

namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId) { return 0; }
}  // namespace bedrocktools::memory

namespace bedrocktools::events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
}  // namespace bedrocktools::events

#include "config/ConfigManager.hpp"
#include "launcher/ExternalButtonRefresh.hpp"
#include "modules/ModuleRegistry.hpp"

namespace {

// The registry the menu walks. This test cares about the menu the launcher is
// handed, not about who owns the modules, so the container is a plain vector.
std::vector<Module*> g_modules;

}  // namespace

ModuleRegistry& ModuleRegistry::get() {
    static ModuleRegistry instance;
    return instance;
}

Module* ModuleRegistry::find(std::string_view id) const {
    for (auto* module : g_modules) {
        if (module->moduleId == id) return module;
    }
    return nullptr;
}

const std::vector<Module*>& ModuleRegistry::modules() const { return g_modules; }

bool ModuleRegistry::keybindBlocked() const { return false; }

namespace bedrocktools::config {
void ConfigManager::save() {}
ConfigManager::~ConfigManager() {}
}  // namespace bedrocktools::config

namespace bedrocktools::launcher {
void refreshExternalButtonsForModule(std::string_view) {}
}  // namespace bedrocktools::launcher

#include "launcher/ModuleMenu.cpp"

// The module classes themselves come from their own translation units.
#include "modules/visual/breadcrumbs.hpp"
#include "modules/visual/chunkborder.hpp"
#include "modules/visual/lightoverlay.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (condition) {
        std::printf("  ok   %s\n", message);
    } else {
        std::printf("  FAIL %s\n", message);
        ++g_failures;
    }
}

const pl::modmenu::ModuleInfo* findModule(const std::string& moduleId) {
    for (const auto& info : pl::modmenu::registeredModules()) {
        if (info.moduleId == moduleId) return &info;
    }
    return nullptr;
}

const pl::modmenu::ConfigEntry* findConfig(const pl::modmenu::ModuleInfo& info,
                                           const std::string& key) {
    for (const auto& entry : info.configs) {
        if (entry.key == key) return &entry;
    }
    return nullptr;
}

bool isIntSlider(const pl::modmenu::ConfigEntry* entry, const char* min, const char* max) {
    return entry && entry->type == pl::modmenu::ConfigType::SliderInt && entry->minValue == min &&
           entry->maxValue == max;
}

bool isFloatSlider(const pl::modmenu::ConfigEntry* entry, const char* min, const char* max) {
    return entry && entry->type == pl::modmenu::ConfigType::SliderFloat && entry->minValue == min &&
           entry->maxValue == max;
}

}  // namespace

int main() {
    std::printf("mod menu registration\n");

    ChunkBorderModule chunkBorder;
    BreadcrumbsModule breadcrumbs;
    LightOverlayModule lightOverlay;
    g_modules = {&chunkBorder, &breadcrumbs, &lightOverlay};

    pl::modmenu::registeredModules().clear();
    registerModulesWithLauncher();

    check(pl::modmenu::registeredModules().size() == 3, "all three world overlays reach the menu");

    const auto* chunk = findModule("bedrocktoolsplus.Chunk Border");
    const auto* crumbs = findModule("bedrocktoolsplus.Breadcrumbs");
    const auto* light = findModule("bedrocktoolsplus.Light Overlay");
    check(chunk && crumbs && light, "each module is registered under its own id");
    if (!chunk || !crumbs || !light) return 1;

    check(chunk->hideInHudEditor && crumbs->hideInHudEditor && light->hideInHudEditor,
          "world overlays stay out of the HUD editor");
    check(!chunk->description.empty() && !crumbs->description.empty() &&
              !light->description.empty(),
          "every module ships a description");

    std::printf("chunk border controls\n");
    // One entry per module setting, plus the keybind control the launcher adds.
    check(chunk->configs.size() == 6, "chunk border exposes its five settings plus the keybind");
    const auto* chunkKeybind = findConfig(*chunk, "keybind");
    check(chunkKeybind && chunkKeybind->type == pl::modmenu::ConfigType::Keybind,
          "the keybind is registered as a key binding, not a slider");
    const auto* vertSpacing = findConfig(*chunk, "vertLineSpacing");
    const auto* horizSpacing = findConfig(*chunk, "horizLineSpacing");
    check(vertSpacing && vertSpacing->displayName == "Vert Line Spacing",
          "camel case keys become readable labels");
    check(isFloatSlider(vertSpacing, "0.000000", "16.000000"),
          "ring spacing slider stops at one chunk");
    check(isIntSlider(horizSpacing, "0", "16"), "post spacing slider stops at one chunk");
    const auto* cornerColor = findConfig(*chunk, "cornerColor");
    check(cornerColor && cornerColor->type == pl::modmenu::ConfigType::Color &&
              cornerColor->defaultValue == "#FF0000FF",
          "chunk colours are registered as colour pickers");

    std::printf("breadcrumbs controls\n");
    check(crumbs->configs.size() == 5, "breadcrumbs exposes its four settings plus the keybind");
    check(isIntSlider(findConfig(*crumbs, "tickInterval"), "1", "40"),
          "sampling rate slider matches the range the module accepts");
    check(isIntSlider(findConfig(*crumbs, "maxPoints"), "1", "2000"),
          "trail length slider covers the default instead of capping below it");
    const auto* clearButton = findConfig(*crumbs, "clearTrailButton");
    check(clearButton && clearButton->type == pl::modmenu::ConfigType::Button &&
              clearButton->displayName == "Clear Trail Button",
          "the clear-trail action is registered as a button, not a toggle");

    std::printf("light overlay controls\n");
    check(light->configs.size() == 8, "light overlay exposes its seven settings plus the keybind");
    check(isIntSlider(findConfig(*light, "radiusHorizontal"), "0", "32") &&
              isIntSlider(findConfig(*light, "radiusVertical"), "0", "32"),
          "scan radius sliders stop where the renderer can still keep up");
    check(isIntSlider(findConfig(*light, "dangerThreshold"), "0", "15"),
          "the danger threshold slider matches the light scale");
    const auto* onlyTop = findConfig(*light, "onlyTopFace");
    check(onlyTop && onlyTop->type == pl::modmenu::ConfigType::Toggle &&
              onlyTop->dependsOn.empty(),
          "mode toggles are not nested under an unrelated toggle");

    std::printf("range overrides are name exact\n");
    check(intSliderRangeFor("m_snapThreshold") == nullptr &&
              intSliderRangeFor("maxPointsCap") == nullptr &&
              intSliderRangeFor("tickInterval") != nullptr,
          "only the exact keys get an override, so other modules keep their range");

    std::printf("\n");
    if (g_failures != 0) {
        std::printf("%d mod menu check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all mod menu checks passed\n");
    return 0;
}

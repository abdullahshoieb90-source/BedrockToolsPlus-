// Unit tests for the No Potion Bar module.
//
// No Potion Bar is intentionally pure state: the vanilla potion-bar draw call
// is skipped by the shared detour installed by the Effect Display module
// (EffectDisplayModule::renderPotionEffectsDetour in effectdisplay.cpp), which
// ORs NoPotionBarModule::suppressesVanillaBar() into its suppression
// condition every frame. These tests pin that contract:
//
//   * nothing is suppressed until the module is enabled
//   * enabling through the master toggle suppresses the default bar
//   * the keybind re-arm (keybindActive) gates suppression like any module
//   * disabling stops suppressing again
//   * the enabled state round-trips through the module config json
//
// Build and run standalone (the module only needs nlohmann_json; the host
// fake in tests/fakejson is enough):
//
//     g++ -std=c++20 -I src -I include -I tests/fakejson
//         tests/nopotionbar_test.cpp src/modules/hud/nopotionbar.cpp
//         -o /tmp/nopotionbar_test && /tmp/nopotionbar_test

#include "modules/hud/nopotionbar.hpp"

#include <cstdio>

namespace {

int failures = 0;

void expect(const char* what, bool condition) {
    if (!condition) {
        std::printf("  FAIL %s\n", what);
        ++failures;
    }
}

} // namespace

int main() {
    {
        NoPotionBarModule module;
        expect("module id", module.moduleId == "bedrocktoolsplus.No Potion Bar");

        // Disabled by default: the vanilla potion bar must stay visible.
        expect("not suppressed while disabled", !NoPotionBarModule::suppressesVanillaBar());
        expect("enabled flag starts false", !module.enabled);

        // Master toggle on -> the shared detour must skip the vanilla bar.
        module.setMasterEnabled(true);
        expect("enabled after master toggle", module.enabled);
        expect("suppressed after master toggle", NoPotionBarModule::suppressesVanillaBar());

        // The keybind re-arm gates the module exactly like the master toggle.
        module.setKeybindActive(false);
        expect("disabled while keybind inactive", !module.enabled);
        expect("not suppressed while keybind inactive", !NoPotionBarModule::suppressesVanillaBar());
        module.setKeybindActive(true);
        expect("suppressed again after re-arm", NoPotionBarModule::suppressesVanillaBar());

        // Master toggle off -> back to the vanilla bar.
        module.setMasterEnabled(false);
        expect("not suppressed after disable", !NoPotionBarModule::suppressesVanillaBar());

        // The enabled state persists through a config round trip.
        nlohmann::json saved;
        module.setMasterEnabled(true);
        module.saveConfig(saved);
        expect("config records masterEnabled", saved.contains("masterEnabled") && saved["masterEnabled"].get<bool>());

        NoPotionBarModule restored;
        restored.loadConfig(saved);
        expect("config restores enabled state", restored.enabled);
        expect("suppressed after config restore", NoPotionBarModule::suppressesVanillaBar());
    }

    // The destroyed module unregisters itself, so suppression must stop
    // instead of dangling (the detour reads the instance live).
    expect("not suppressed after destruction", !NoPotionBarModule::suppressesVanillaBar());

    if (failures == 0) {
        std::printf("nopotionbar_test: all checks passed\n");
        return 0;
    }
    std::printf("nopotionbar_test: %d check(s) failed\n", failures);
    return 1;
}

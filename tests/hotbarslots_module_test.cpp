// Host-side test that builds the real Hotbar Slots module
// (src/modules/hud/hotbarslots.cpp is compiled by scripts/run_tests.sh as a
// second translation unit) and drives it against the recording fakes in
// tests/fakepl / tests/fakejson. The Android-only plumbing (item rendering,
// JNI geometry queries) is replaced with stubs; everything under test is the
// module's own logic.
//
// Covered, mirroring the "On Slot Buttons" artwork fix:
//   * with Icon Placement = On Slot Buttons (the default) the nine launcher
//     buttons are registered with the cut-open artwork (fill-rule="evenodd",
//     window at 11.625..52.375) and WITHOUT a launcher label, so nothing
//     opaque is drawn over the natively painted icon;
//   * switching to HUD Strip re-registers the buttons with the solid artwork
//     and the number label;
//   * changing an unrelated setting (slot size) does NOT re-register the
//     buttons, so the user's launcher-side placement is preserved;
//   * onFrame()/onDisable() stay quiet and clean up.
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src -I include -I tests/fakepl -I tests/fakejson
//         tests/hotbarslots_module_test.cpp src/modules/hud/hotbarslots.cpp
//         -o /tmp/hotbarslots_module_test && /tmp/hotbarslots_module_test

#include <pl/ModMenu.hpp>

#include "modules/hud/hotbarslots.hpp"
#include "modules/hud/hotbarslots_buttons.hpp"
#include "modules/hud/huditems.hpp"
#include "launcher/ExternalButtonGeometry.hpp"

#include <cstdio>
#include <string>

// ---- Stubs for the Android-only plumbing hotbarslots.cpp links against ----

namespace bedrocktools::huditems {

void initialize() {}
bool addRenderListener(RenderListener, void*) { return true; }
void removeRenderListener(RenderListener, void*) {}

ContainerSlots playerInventory(void*) { return ContainerSlots{}; }
void* getCarriedItem(void*) { return nullptr; }
void* stackItem(void*) { return nullptr; }

std::uint32_t parseColor(const std::string&, std::uint32_t fallback) { return fallback; }
std::uint32_t withOpacity(std::uint32_t color, float) { return color; }

// No ItemRenderer on the host: player() stays null, so renderNative() takes
// its early-exit path without touching any game memory.
IconPainter::IconPainter(void* context, void* client, bool) {
    (void)context;
    (void)client;
}
IconPainter::~IconPainter() = default;
bool IconPainter::draw(void*, void*, float, float, float) { return false; }

} // namespace bedrocktools::huditems

namespace bedrocktools::launcher {

bool queryExternalButtonGeometry(std::string_view, ExternalButtonGeometry&) { return false; }

} // namespace bedrocktools::launcher

namespace {

int failures = 0;

void expectTrue(const char* what, bool ok) {
    if (!ok) {
        std::printf("  FAIL %s\n", what);
        ++failures;
    }
}

void expectEq(const char* what, const std::string& actual, const std::string& expected) {
    if (actual != expected) {
        std::printf("  FAIL %s: expected \"%s\", got \"%s\"\n", what, expected.c_str(), actual.c_str());
        ++failures;
    }
}

using bedrocktools::hotbar::slotButtonActiveSvg;
using bedrocktools::hotbar::slotButtonSvg;

bool registeredHollow(const pl::modmenu::RecordedButton& button) {
    return button.svg == slotButtonSvg(true) && button.activeSvg == slotButtonActiveSvg(true);
}

bool registeredSolid(const pl::modmenu::RecordedButton& button) {
    return button.svg == slotButtonSvg(false) && button.activeSvg == slotButtonActiveSvg(false);
}

} // namespace

int main() {
    std::printf("hotbar slots module\n");

    HotbarSlotsModule module;

    // Default placement is On Slot Buttons (parity with the launcher mod).
    module.onInit();
    expectTrue("nine buttons registered on init", pl::modmenu::recordedButtons().size() == 9);
    for (int slot = 1; slot <= 9; ++slot) {
        const auto& button = pl::modmenu::recordedButtons()["bedrocktoolsplus.HotbarSlots.Button" + std::to_string(slot)];
        char label[96];
        std::snprintf(label, sizeof(label), "slot %d button uses the cut-open artwork", slot);
        expectTrue(label, registeredHollow(button));
        std::snprintf(label, sizeof(label), "slot %d window is cut at 11.625", slot);
        expectTrue(label, button.svg.find("M11.625,11.625") != std::string::npos);
        std::snprintf(label, sizeof(label), "slot %d launcher label is suppressed", slot);
        expectEq(label, button.label, "");
        std::snprintf(label, sizeof(label), "slot %d launcher text is transparent", slot);
        expectTrue(label, (button.textColor & 0xFF000000u) == 0 && (button.activeTextColor & 0xFF000000u) == 0);
        std::snprintf(label, sizeof(label), "slot %d keeps its hotbar key code", slot);
        expectTrue(label, button.androidKeyCode == 8 + slot - 1);
    }

    // Switching to the HUD strip re-registers with the solid artwork + label.
    nlohmann::json stripConfig;
    stripConfig["m_iconPlacement"] = std::string("0,HUD Strip,On Slot Buttons");
    const int generationAfterInit = pl::modmenu::buttonRegistryGeneration();
    module.loadConfig(stripConfig);
    expectTrue("switching placement re-registers the buttons",
               pl::modmenu::buttonRegistryGeneration() != generationAfterInit);
    {
        const auto& button = pl::modmenu::recordedButtons()["bedrocktoolsplus.HotbarSlots.Button1"];
        expectTrue("strip mode uses the solid artwork", registeredSolid(button));
        expectEq("strip mode restores the launcher label", button.label, "1");
        expectTrue("strip mode text is opaque", (button.textColor & 0xFF000000u) == 0xFF000000u);
    }

    // Switching back to On Slot Buttons cuts the window open again.
    nlohmann::json buttonsConfig;
    buttonsConfig["m_iconPlacement"] = std::string("1,HUD Strip,On Slot Buttons");
    module.loadConfig(buttonsConfig);
    {
        const auto& button = pl::modmenu::recordedButtons()["bedrocktoolsplus.HotbarSlots.Button1"];
        expectTrue("back on the buttons: hollow artwork again", registeredHollow(button));
        expectEq("back on the buttons: label suppressed again", button.label, "");
    }

    // Unrelated edits must not churn the launcher registry.
    nlohmann::json unrelated;
    unrelated["m_slotSize"] = 48.0f;
    unrelated["m_numberColor"] = std::string("#FF0000");
    const int generationBefore = pl::modmenu::buttonRegistryGeneration();
    module.loadConfig(unrelated);
    expectTrue("unrelated edits do not re-register the buttons",
               pl::modmenu::buttonRegistryGeneration() == generationBefore);

    // A frame without the game must not submit strip editor elements while the
    // icons live on the buttons.
    module.enabled = true;
    module.onFrame();
    expectTrue("no HUD editor element while on the buttons", pl::modmenu::recordedHudEditorElements().empty());

    // Disabling removes every button.
    module.onDisable();
    expectTrue("disable unregisters the buttons", pl::modmenu::recordedButtons().empty());

    if (failures != 0) {
        std::printf("\n%d hotbar slots module checks failed\n", failures);
        return 1;
    }
    std::printf("\nall hotbar slots module checks passed\n");
    return 0;
}

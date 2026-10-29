// Unit tests for the Comment Key on-screen buttons.
//
// Mirrors commandhotkey_test.cpp and pins the contract that the "Button
// Opacity" setting actually reaches the registered button (it used to be
// saved/loaded but never applied, so moving the slider changed nothing):
//   * the opacity is baked into the label color alpha and the SVG frame's
//     root opacity attribute (the launcher rasterizes the frame with lunasvg,
//     which honors it)
//   * an opacity-only config change re-registers the native button definition
//     so the launcher's overlay snapshot picks up the new look
//   * an unchanged config does NOT rebuild the buttons
//
// Build and run standalone (adjust the two package paths to your xmake cache):
//
//     PRE_LOADER=$(echo ~/.xmake/packages/p/preloader/main/*/include)
//     JSON=$(echo ~/.xmake/packages/n/nlohmann_json/v3.11.3/*/include)
//     g++ -std=c++20 -I include -I src -I "$PRE_LOADER" -I "$JSON" \
//         tests/commentkey_test.cpp -o /tmp/commentkey_test
//     /tmp/commentkey_test

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

// Real declarations first so the stubs below match them exactly.
#include "pl/ModMenu.hpp"
#include "bedrocktools/memory/Signatures.hpp"

// ---------------------------------------------------------------------------
// Launcher / SDK stubs
// ---------------------------------------------------------------------------

// Recorded overlay activity (buttonId -> last registered definition fields).
static std::unordered_map<std::string, std::string> g_labelById;
static std::unordered_map<std::string, int> g_registerCount;
static std::unordered_map<std::string, int> g_unregisterCount;
static std::unordered_map<std::string, unsigned int> g_textColorById;
static std::unordered_map<std::string, std::string> g_iconById;

namespace pl::modmenu {
    bool registerModule(const ModuleInfo&) { return true; }
    void unregisterModule(std::string_view) {}
    void setModuleEnabled(std::string_view, bool) {}
    void submitDrawCommands(std::string_view, std::span<const DrawCommand>) {}
    bool registerFont(std::string_view, std::span<const unsigned char>) { return true; }
    bool registerImage(std::string_view, std::span<const unsigned char>, int, int) { return true; }
    bool registerButton(const ButtonInfo& info) {
        g_labelById[info.buttonId] = info.label;
        g_textColorById[info.buttonId] = info.textColor;
        g_iconById[info.buttonId] = std::string(info.iconData.begin(), info.iconData.end());
        ++g_registerCount[info.buttonId];
        return true;
    }
    void unregisterButton(std::string_view id) {
        ++g_unregisterCount[std::string(id)];
    }
}

namespace bedrocktools::memory {
    std::uintptr_t resolve(SignatureId) { return 0; }
}

namespace bedrocktools::core::gamehooks {
    void* clientInstance() { return nullptr; }
}

// The module under test, included directly so its anonymous-namespace helpers
// (launcherLabel, ...) are reachable.
#include "modules/misc/commentkey.cpp"

// ModuleRegistry is only referenced for keybind gating in onKeyEvent; stub the
// members the translation unit touches.
ModuleRegistry& ModuleRegistry::get() {
    static ModuleRegistry instance;
    return instance;
}
bool ModuleRegistry::keybindBlocked() const { return false; }
Module* ModuleRegistry::find(std::string_view) const { return nullptr; }
const std::vector<Module*>& ModuleRegistry::modules() const {
    static std::vector<Module*> empty;
    return empty;
}

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) std::printf("  ok   %s\n", what);
    else { std::printf("  FAIL %s\n", what); ++g_failures; }
}

} // namespace

int main() {
    std::printf("Comment Key overlay buttons\n");

    CommentKey mod;

    // Initial config: slot 1 enabled with a comment text.
    nlohmann::json j;
    mod.saveConfig(j);                 // emit the full key set
    j["m_comment1"] = true;
    j["m_comment1Text"] = "Hello";
    j["m_comment1Keybind"] = 0;
    j["m_comment1Width"] = 64.0f;
    j["m_comment1Height"] = 64.0f;
    j["m_comment1TextColor"] = "#373737";
    j["m_buttonBorderColor"] = "#373737"; // non-legacy so opacity/text keys apply
    mod.loadConfig(j);

    const std::string btn1 = "bedrocktoolsplus.CommentKey.Button1";
    check(g_registerCount[btn1] >= 1, "slot 1 registered on first load");
    check(g_labelById[btn1] == "Hello", "button shows the configured comment");

    const int regAfterFirst = g_registerCount[btn1];
    const int unregAfterFirst = g_unregisterCount[btn1];

    // The Button Opacity slider must actually reach the registered button:
    // baked into the label color alpha and the SVG frame's root opacity.
    check((g_textColorById[btn1] >> 24) == 0xD9,
          "default opacity (0.85) is baked into the label alpha");
    check(g_iconById[btn1].find("opacity=\"0.850\"") != std::string::npos,
          "default opacity is baked into the SVG frame");
    check((g_textColorById[btn1] & 0xFFFFFFu) == 0x373737,
          "label RGB survives the alpha bake");

    // Re-load the *same* config: nothing changed, so no rebuild is expected.
    mod.loadConfig(j);
    check(g_registerCount[btn1] == regAfterFirst, "unchanged config does not rebuild");
    check(g_unregisterCount[btn1] == unregAfterFirst, "unchanged config does not unregister");

    // Change ONLY the opacity (simulating the Button Opacity slider).
    nlohmann::json j2 = j;
    j2["m_buttonOpacity"] = 0.25f;
    mod.loadConfig(j2);
    check(g_registerCount[btn1] > regAfterFirst, "opacity-only change re-registers the button");
    check((g_textColorById[btn1] >> 24) == 0x40,
          "opacity 0.25 is baked into the label alpha");
    check(g_iconById[btn1].find("opacity=\"0.250\"") != std::string::npos,
          "opacity 0.25 is baked into the SVG frame");

    // Changing the comment text alone still re-registers with the new label.
    const int regBeforeText = g_registerCount[btn1];
    nlohmann::json j3 = j2;
    j3["m_comment1Text"] = "GG";
    mod.loadConfig(j3);
    check(g_registerCount[btn1] > regBeforeText, "text-only change re-registers the button");
    check(g_labelById[btn1] == "GG", "button label updated to the new text");
    // The opacity bake survives unrelated edits.
    check((g_textColorById[btn1] >> 24) == 0x40,
          "opacity stays baked after an unrelated edit");

    // Fully opaque config emits the plain SVG (no opacity attribute) so the
    // launcher keeps byte-identical icons for the common case.
    nlohmann::json j4 = j3;
    j4["m_buttonOpacity"] = 1.0f;
    mod.loadConfig(j4);
    check((g_textColorById[btn1] >> 24) == 0xFF,
          "opacity 1.0 restores a fully opaque label");
    check(g_iconById[btn1].find("opacity=") == std::string::npos,
          "opacity 1.0 emits the plain SVG frame");

    std::printf(g_failures == 0 ? "ALL TESTS PASSED\n" : "%d TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

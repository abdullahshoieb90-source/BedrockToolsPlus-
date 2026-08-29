// Unit tests for the Effect Display HUD module's language support.
//
// Verifies the feature end-to-end against the documented contract, with the
// launcher APIs stubbed (same pattern as crosshair_test.cpp):
//   * "Auto" follows the game language read from options.txt
//     (games/com.mojang/minecraftpe/options.txt, `game_language` key)
//   * rewriting options.txt switches the names on the next poll (~2s)
//   * Arabic names use the launcher's default font (empty fontId) because
//     the bundled pixel font has no Arabic glyphs; English keeps it
//   * endless effects show the localized infinity label
//   * a pinned language setting wins over the game language
//   * the language radio round-trips as "<index>,Auto,<language>..."
//   * the vanilla potion-bar filter swallows the bar's texture draws while
//     the module is enabled and forwards them again once it is disabled
//     (drives the drawImage/drawNineslice detours through a fake render
//     context whose getTexture() resolves fake speed-effect records)
//   * the filter never latches: a texture record replaced by a resource
//     reload passes through, the fresh record is suppressed, and an
//     atlas-aliased group (one record for two paths) switches suppression
//     off entirely instead of hiding unrelated UI
//
// Like crosshair_test.cpp, the module needs the preloader and nlohmann_json
// headers (normally provided by xmake). Build and run standalone (adjust the
// package paths to your xmake cache):
//
//     PRE_LOADER=$(echo ~/.xmake/packages/p/preloader/main/*/include)
//     JSON=$(echo ~/.xmake/packages/n/nlohmann_json/v3.11.3/*/include)
//     ENTT=$(echo ~/.xmake/packages/e/entt/v3.16.0/*/include)
//     FMT=$(echo ~/.xmake/packages/f/fmt/12.2.0/*/include)
//     g++ -std=c++20 -I include -I src -I "$PRE_LOADER" -I "$JSON" \
//         -I "$ENTT" -I "$FMT" tests/effectdisplay_test.cpp -o /tmp/effectdisplay_test
//     /tmp/effectdisplay_test

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <vector>

// Real declarations first so the stubs below match them exactly.
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>
#include "bedrocktools/memory/Signatures.hpp"
#include "bedrocktools/events/EventBus.hpp"
#include "core/Runtime.hpp"

// ---- launcher / runtime stubs -------------------------------------------
static std::vector<pl::modmenu::DrawCommand> g_lastCmds;

namespace pl::memory {
    int hook(FuncPtr, FuncPtr, FuncPtr*, HookPriority) { return -1; }
    bool unhook(FuncPtr, FuncPtr) { return true; }
    std::uintptr_t resolveVtableFunction(std::string_view, std::size_t, std::string_view) { return 0; }
}

namespace pl::modmenu {
    bool registerModule(const ModuleInfo&) { return true; }
    void unregisterModule(std::string_view) {}
    void setModuleEnabled(std::string_view, bool) {}
    void submitDrawCommands(std::string_view, std::span<const DrawCommand> commands) {
        g_lastCmds.assign(commands.begin(), commands.end());
    }
    bool registerFont(std::string_view, std::span<const unsigned char>) { return true; }
    bool registerImage(std::string_view, std::span<const unsigned char>, int, int) { return true; }
    bool registerButton(const ButtonInfo&) { return true; }
    void unregisterButton(std::string_view) {}
}

namespace bedrocktools::memory {
    std::uintptr_t resolve(SignatureId) { return 0; }   // no vanilla-bar hook
}

namespace bedrocktools::events {
    EventBus& bus() { static EventBus instance; return instance; }
}

namespace bedrocktools::core {
    Runtime& Runtime::get() { static Runtime instance; return instance; }
    const std::filesystem::path& Runtime::resourceDirectory() const noexcept {
        static const std::filesystem::path empty;
        return empty;   // no minecraft.ttf on the host: font stays unregistered
    }
}

#include "modules/hud/effectdisplay.cpp"

namespace {
int g_failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}
const pl::modmenu::DrawCommand* findText(const std::string& contains) {
    for (const auto& c : g_lastCmds) {
        if (c.type == pl::modmenu::DrawCommandType::Text && c.text.find(contains) != std::string::npos) return &c;
    }
    return nullptr;
}
void writeOptions(const std::filesystem::path& dir, const std::string& language, int epochOffset) {
    std::filesystem::create_directories(dir / "games/com.mojang/minecraftpe");
    const auto file = dir / "games/com.mojang/minecraftpe/options.txt";
    std::ofstream(file, std::ios::trunc) << "mp_username:Steve\ngame_difficulty_new:1\n"
                                            "game_language:" << language << "\nmp_server_visible:1\n";
    // Force well-separated mtimes (the watcher compares stat() seconds).
    std::filesystem::last_write_time(file,
        std::filesystem::file_time_type{} + std::chrono::seconds(epochOffset));
}

// ---- fake MinecraftUIRenderContext for the vanilla-bar filter ------------
// The suppression detours resolve the vanilla bar's textures through the
// context's getTexture() virtual (vtable slot documented in offsets::VTable),
// compare draws against the resolved ClientTexture records by pointer
// identity, and forward everything else through the s_original* statics. A
// fake vtable exposing just getTexture, plus recording stand-ins for the
// original draw functions, is enough to drive the whole contract on the host.
void* g_fakeRenderVtable[33] = {};
struct FakeRenderContext { void* vtable; };
FakeRenderContext g_fakeRenderContext{g_fakeRenderVtable};

// path -> record the fake texture group returns for it. Tests swap entries to
// simulate records being created or replaced when the player joins a world.
std::unordered_map<std::string, vanilla_ui::BedrockTextureData*> g_fakeTextures;
std::unordered_map<std::string, std::shared_ptr<vanilla_ui::ResourceLocation>> g_fakeLocations;

vanilla_ui::TexturePtr fakeGetTexture(void*, const vanilla_ui::ResourceLocation& location, bool) {
    vanilla_ui::TexturePtr handle;
    const auto it = g_fakeTextures.find(location.path);
    if (it == g_fakeTextures.end()) return handle;
    auto& loc = g_fakeLocations[location.path];
    if (!loc) loc = std::make_shared<vanilla_ui::ResourceLocation>(location.path.c_str());
    handle.resourceLocation = loc;
    // Non-owning handle: the test keeps the records alive for the whole run.
    handle.clientTexture = std::shared_ptr<const vanilla_ui::BedrockTextureData>(
        std::shared_ptr<const vanilla_ui::BedrockTextureData>(), it->second);
    return handle;
}

int g_drawImageCalls = 0;
int g_ninesliceCalls = 0;
void recordingDrawImage(void*, const void*, const void*, const void*, const void*, const void*, bool) {
    ++g_drawImageCalls;
}
void recordingDrawNineslice(void*, const void*, const void*) { ++g_ninesliceCalls; }

// Draws one image through the suppression detour.
void fakeDrawImage(const void* texture) {
    EffectDisplayModule::drawImageDetour(&g_fakeRenderContext, texture, nullptr, nullptr, nullptr, nullptr, false);
}
} // namespace

int main() {
    const auto sandbox = std::filesystem::temp_directory_path() / "effectdraw_sandbox";
    std::filesystem::remove_all(sandbox);
    writeOptions(sandbox, "en_US", 1000);
    std::filesystem::current_path(sandbox);

    EffectDisplayModule mod;
    mod.setMasterEnabled(true);
    nlohmann::json cfg;
    cfg["m_preview"] = true;
    mod.loadConfig(cfg);
    mod.onInit();
    mod.onFrame();

    std::printf("english (auto, from options.txt)\n");
    const auto* speed = findText("Speed");
    check(speed != nullptr, "preview shows Speed II");
    check(speed && speed->text == "Speed II", "name text is 'Speed II'");
    check(speed && speed->fontId == "minecraft", "English uses the pixel font");
    const auto* infinite = findText("\xE2\x88\x9E");
    check(infinite != nullptr, "endless Invisibility shows the infinity label");

    std::printf("arabic (auto, options.txt rewritten)\n");
    writeOptions(sandbox, "ar_SA", 2000);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    mod.onFrame();
    const auto* arabic = findText("\xD8\xb3\xD8\xb1\xD8\xb9\xD8\xa9");  // سرعة
    check(arabic != nullptr, "preview shows سرعة (Speed in Arabic)");
    check(arabic && arabic->fontId.empty(), "Arabic switches to the system font");
    check(findText("Speed") == nullptr, "no English name left");

    std::printf("pinned language override (English while game says Arabic)\n");
    nlohmann::json pin;
    pin["m_language"] = "1";   // index 1 -> kLanguages[0] == en_US
    mod.loadConfig(pin);
    mod.onFrame();
    check(findText("Speed II") != nullptr, "pinned English wins over the game language");

    std::printf("config round-trip\n");
    nlohmann::json saved;
    mod.saveConfig(saved);
    const std::string radio = saved["m_language"];
    check(radio.rfind("1,Auto,English (US)", 0) == 0, "radio persists as '<index>,Auto,...' (got: " + radio.substr(0, 24) + "...)");
    size_t commas = std::count(radio.begin(), radio.end(), ',');
    check(commas == bedrocktools::hud::effects::languageCount() + 1,
          "one option per language plus Auto");

    std::printf("vanilla potion-bar suppression\n");
    {
        // Deterministic refreshes: every detour call re-resolves the texture
        // set, standing in for the ~2 Hz refresh the device build uses.
        EffectDisplayModule::s_vanillaBarRefreshInterval = std::chrono::milliseconds(0);
        EffectDisplayModule::s_originalDrawImage = recordingDrawImage;
        EffectDisplayModule::s_originalDrawNineslice = recordingDrawNineslice;
        g_drawImageCalls = g_ninesliceCalls = 0;
        g_fakeRenderVtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetTexture] =
            reinterpret_cast<void*>(&fakeGetTexture);

        static vanilla_ui::BedrockTextureData recordA{};
        static vanilla_ui::BedrockTextureData recordB{};
        static vanilla_ui::BedrockTextureData frameRecord{};
        static vanilla_ui::BedrockTextureData otherRecord{};
        g_fakeTextures.clear();
        g_fakeLocations.clear();
        g_fakeTextures["textures/ui/speed_effect"] = &recordA;
        g_fakeTextures["textures/ui/mob_effect_background"] = &frameRecord;

        fakeDrawImage(&otherRecord);
        check(g_drawImageCalls == 1, "unrelated image still draws while the module is on");
        fakeDrawImage(&recordA);
        check(g_drawImageCalls == 1, "vanilla effect icon draw is swallowed while the module is on");
        EffectDisplayModule::drawNinesliceDetour(&g_fakeRenderContext, &frameRecord, nullptr);
        check(g_ninesliceCalls == 0, "vanilla bar frame (nineslice) is swallowed while the module is on");

        std::printf("module toggling\n");
        mod.setMasterEnabled(false);
        fakeDrawImage(&recordA);
        check(g_drawImageCalls == 2, "every vanilla bar draw is forwarded again once the module is off");
        mod.setMasterEnabled(true);

        // Simulate a resource-stack change joining a world: the speed_effect
        // record the HUD now holds is a different one than the filter saw
        // before the module was toggled.
        g_fakeTextures["textures/ui/speed_effect"] = &recordB;
        fakeDrawImage(&recordA);
        check(g_drawImageCalls == 3, "stale record from before the toggle is not suppressed");
        fakeDrawImage(&recordB);
        check(g_drawImageCalls == 3, "suppression resumes on the fresh record when the module is back on");

        std::printf("record refresh without latching\n");
        g_fakeTextures["textures/ui/speed_effect"] = &recordA;
        fakeDrawImage(&recordB);
        check(g_drawImageCalls == 4, "record replaced by a resource reload passes through again");
        fakeDrawImage(&recordA);
        check(g_drawImageCalls == 4, "new record for the same path is suppressed");

        std::printf("atlas fail-safe\n");
        g_fakeTextures.clear();
        g_fakeTextures["textures/ui/speed_effect"] = &recordA;
        g_fakeTextures["textures/ui/slowness_effect"] = &recordA;   // one record, two paths
        fakeDrawImage(&recordA);
        check(g_drawImageCalls == 5, "atlas-aliased records are never suppressed");
        fakeDrawImage(&otherRecord);
        check(g_drawImageCalls == 6, "with an atlas suspected nothing is suppressed at all");

        std::printf("recovery after the atlas latch\n");
        mod.setMasterEnabled(false);
        mod.setMasterEnabled(true);
        g_fakeTextures.clear();
        g_fakeTextures["textures/ui/speed_effect"] = &recordA;
        fakeDrawImage(&recordA);
        check(g_drawImageCalls == 6, "suppression recovers once the suspicion clears");

        // Leave the fake group empty so nothing below can suppress.
        g_fakeTextures.clear();
        mod.setMasterEnabled(false);
        mod.setMasterEnabled(true);
    }
    EffectDisplayModule::s_originalDrawImage = nullptr;
    EffectDisplayModule::s_originalDrawNineslice = nullptr;

    std::printf("\n%s\n", g_failures ? "HARNESS FAILED" : "harness passed");
    return g_failures ? 1 : 0;
}

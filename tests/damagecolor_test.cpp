// Host integration test for the Damage Color module. No Minecraft or Android
// runtime is required: signature resolution is stubbed to 0 (so no hooks are
// installed), and the color-replacement helpers and detours are exercised
// directly.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/memory/Signatures.hpp>

namespace bedrocktools::memory { std::uintptr_t resolve(SignatureId) { return 0; } }
#include "modules/visual/damagecolor.cpp"

namespace {

using Color = bedrocktools::sdk::Color;

// Recorders used as fake trampolines (function pointers, no captures).
const Color* g_seenEntityOverlay = nullptr;
Color g_seenUiOverlay{};
bool g_uiCalled = false;

void recordSetEntityConstants(
    void*, void*, const Color*, const void*, const void*, const Color* overlayArg,
    const Color*, const Color*, const Color*, const void*, const void*,
    float, float, float, float) {
    g_seenEntityOverlay = overlayArg;
}

void recordSetupShaderParametersGlint(
    void*, const Color* overlayArg, const Color*, const Color*, const Color*,
    float, float, float, float, const void*, const void*, float) {
    g_seenUiOverlay = *overlayArg;
    g_uiCalled = true;
}

void assertFloatClose(float actual, float expected, float eps = 1e-6f) {
    assert(std::fabs(actual - expected) <= eps);
}

}

int main() {
    DamageColorModule mod;
    mod.onInit(); // resolve() is stubbed to 0, so hooks stay uninstalled and nothing crashes.
    assert(!mod.enabled);

    const Color overlay{0.9f, 0.3f, 0.2f, 0.4f};
    Color storage{};

    // Disabled: the caller's overlay is returned untouched.
    assert(customOverlay(&overlay, storage) == &overlay);
    assert(customOverlay(nullptr, storage) == nullptr);

    // Enabled: RGB is replaced, the game's fade alpha is preserved verbatim.
    mod.setMasterEnabled(true);
    assert(mod.enabled);

    nlohmann::json j;
    j["damageColor"] = std::string("#00FF00");
    mod.loadConfig(j);
    const Color* replaced = customOverlay(&overlay, storage);
    assert(replaced == &storage);
    assertFloatClose(storage.r, 0.0f);
    assertFloatClose(storage.g, 1.0f);
    assertFloatClose(storage.b, 0.0f);
    assertFloatClose(storage.a, overlay.a);

    // 8-digit hex keeps the stored alpha, 7-digit hex defaults it to opaque.
    nlohmann::json j2;
    j2["damageColor"] = std::string("#80112233");
    mod.loadConfig(j2);
    assert(mod.damageColor() == 0x80112233u);
    nlohmann::json j3;
    j3["damageColor"] = std::string("#AABBCC");
    mod.loadConfig(j3);
    assert(mod.damageColor() == 0xFFAABBCCu);

    // Malformed values are ignored and the previous color survives.
    for (const char* bad : {"#GG0000", "#12345", "FF0000", "#1234567890", "red", ""}) {
        nlohmann::json jb;
        jb["damageColor"] = std::string(bad);
        mod.loadConfig(jb);
        assert(mod.damageColor() == 0xFFAABBCCu);
    }

    // Disabling restores the pass-through behaviour.
    mod.setMasterEnabled(false);
    assert(customOverlay(&overlay, storage) == &overlay);
    mod.setMasterEnabled(true);

    // Rainbow: fully saturated output (max channel 1, min channel 0) and the
    // game alpha still preserved.
    nlohmann::json j4;
    j4["rainbow"] = true;
    j4["rainbowSpeed"] = 0.5f;
    mod.loadConfig(j4);
    assert(mod.rainbowEnabled());
    const Color* rainbow = customOverlay(&overlay, storage);
    assert(rainbow == &storage);
    const float hi = std::max({storage.r, storage.g, storage.b});
    const float lo = std::min({storage.r, storage.g, storage.b});
    assertFloatClose(hi, 1.0f);
    assertFloatClose(lo, 0.0f, 1e-3f);
    assertFloatClose(storage.a, overlay.a);

    // Rainbow speed is clamped to the menu range; NaN is rejected.
    nlohmann::json j5;
    j5["rainbowSpeed"] = 99.0f;
    mod.loadConfig(j5);
    assertFloatClose(mod.rainbowSpeedValue(), 1.0f);
    nlohmann::json j6;
    j6["rainbowSpeed"] = -5.0f;
    mod.loadConfig(j6);
    assertFloatClose(mod.rainbowSpeedValue(), 0.05f);
    nlohmann::json j7;
    j7["rainbowSpeed"] = std::nanf("");
    mod.loadConfig(j7);
    assertFloatClose(mod.rainbowSpeedValue(), 0.05f); // unchanged after NaN rejection

    // Idle overlay (alpha 0) keeps alpha 0 through the replacement.
    nlohmann::json j8;
    j8["rainbow"] = false;
    j8["damageColor"] = std::string("#FF0000");
    mod.loadConfig(j8);
    const Color idle{0.0f, 0.0f, 0.0f, 0.0f};
    const Color* idleOut = customOverlay(&idle, storage);
    assert(idleOut == &storage);
    assertFloatClose(storage.a, 0.0f);

    // Config roundtrip: the saved keys are re-parsed identically.
    DamageColorModule saved;
    nlohmann::json out;
    saved.saveConfig(out);
    assert(out.contains("damageColor"));
    assert(out.contains("rainbow"));
    assert(out.contains("rainbowSpeed"));
    DamageColorModule restored;
    restored.loadConfig(out);
    assert(restored.damageColor() == saved.damageColor());
    assert(restored.rainbowEnabled() == saved.rainbowEnabled());
    assertFloatClose(restored.rainbowSpeedValue(), saved.rainbowSpeedValue());

    // Detour wiring: with fake trampolines installed, the detours forward the
    // replacement color (or the caller's own overlay when disabled) on both
    // the world and the UI paths. A fresh module becomes the global instance,
    // so its default red is what the detours produce.
    DamageColorModule final;
    final.setMasterEnabled(true);

    g_setEntityConstantsOriginal = &recordSetEntityConstants;
    setEntityConstantsDetour(
        nullptr, nullptr,
        nullptr, nullptr, nullptr, &overlay,
        nullptr, nullptr, nullptr, nullptr, nullptr,
        0.0f, 0.0f, 0.0f, 0.0f);
    assert(g_seenEntityOverlay && g_seenEntityOverlay != &overlay);
    assertFloatClose(g_seenEntityOverlay->r, 1.0f);
    assertFloatClose(g_seenEntityOverlay->g, 0.0f);
    assertFloatClose(g_seenEntityOverlay->b, 0.0f);
    assertFloatClose(g_seenEntityOverlay->a, overlay.a);

    g_setupShaderParametersGlintOriginal = &recordSetupShaderParametersGlint;
    g_uiCalled = false;
    final.setMasterEnabled(false);
    setupShaderParametersGlintDetour(
        nullptr, &overlay, nullptr, nullptr, nullptr,
        0.0f, 0.0f, 0.0f, 0.0f, nullptr, nullptr, 0.0f);
    assert(g_uiCalled);
    assertFloatClose(g_seenUiOverlay.g, overlay.g); // disabled: untouched

    final.setMasterEnabled(true);
    setupShaderParametersGlintDetour(
        nullptr, &overlay, nullptr, nullptr, nullptr,
        0.0f, 0.0f, 0.0f, 0.0f, nullptr, nullptr, 0.0f);
    assertFloatClose(g_seenUiOverlay.r, 1.0f);
    assertFloatClose(g_seenUiOverlay.g, 0.0f);
    assertFloatClose(g_seenUiOverlay.a, overlay.a);

    return 0;
}

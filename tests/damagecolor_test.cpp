// Host integration test for the Damage Color module. No Minecraft or Android
// runtime is required: signature resolution is stubbed to 0 (so no hooks are
// installed), and the color-replacement helpers and detours are exercised
// directly.
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/memory/Signatures.hpp>

namespace bedrocktools::memory { std::uintptr_t resolve(SignatureId) { return 0; } }
#include "modules/visual/damagecolor.cpp"

namespace {

using Color = bedrocktools::sdk::Color;

// Recorders used as fake trampolines (function pointers, no captures).
const Color* g_seenEntityOverlay = nullptr;
const Color* g_seenEntityChangeColor = nullptr;
Color g_seenUiOverlay{};
const Color* g_seenUiChangeColor = nullptr;
bool g_uiCalled = false;
const Color* g_seenActorOverlay = nullptr;
const Color* g_seenActorChangeColor = nullptr;
bool g_actorCalled = false;

void recordSetEntityConstants(
    void*, void*, const Color*, const void*, const void*, const Color* overlayArg,
    const Color* changeColorArg, const Color*, const Color*, const void*, const void*,
    float, float, float, float) {
    g_seenEntityOverlay = overlayArg;
    g_seenEntityChangeColor = changeColorArg;
}

void recordSetupActorShaderParametersGlint(
    void*, void*, void*, const Color* overlayArg,
    const Color* changeColorArg, const Color*, const Color*,
    float, float, float, float, const void*, const void*,
    float, std::uint8_t, const void*) {
    g_seenActorOverlay = overlayArg;
    g_seenActorChangeColor = changeColorArg;
    g_actorCalled = true;
}

void recordSetupShaderParametersGlint(
    void*, const Color* overlayArg, const Color* changeColorArg, const Color*, const Color*,
    float, float, float, float, const void*, const void*, float) {
    g_seenUiOverlay = *overlayArg;
    g_seenUiChangeColor = changeColorArg;
    g_uiCalled = true;
}

void assertFloatClose(float actual, float expected, float eps = 1e-6f) {
    assert(std::fabs(actual - expected) <= eps);
}

template <typename T, std::size_t N>
void put(std::array<std::byte, N>& data, std::size_t offset, T value) {
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

}

int main() {
    DamageColorModule mod;
    mod.onInit(); // resolve() is stubbed to 0, so hooks stay uninstalled and nothing crashes.
    assert(!mod.enabled);

    const Color overlay{0.9f, 0.3f, 0.2f, 0.4f};
    const Color change{0.2f, 0.8f, 0.2f, 0.5f};
    const Color change2{0.1f, 0.2f, 0.9f, 0.6f};
    Color storage{};

    // Inactive flash: the caller's colors are returned untouched.
    assert(recolored(&overlay, storage, false) == &overlay);
    assert(recolored(nullptr, storage, true) == nullptr);

    // Disabled module: even an "active" flash keeps the caller's colors.
    assert(recolored(&overlay, storage, true) == &overlay);

    // Enabled: RGB is replaced, the source alpha is preserved verbatim.
    mod.setMasterEnabled(true);
    assert(mod.enabled);

    nlohmann::json j;
    j["damageColor"] = std::string("#00FF00");
    mod.loadConfig(j);
    const Color* replaced = recolored(&overlay, storage, true);
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
    assert(recolored(&overlay, storage, true) == &overlay);
    mod.setMasterEnabled(true);

    // Rainbow: fully saturated output (max channel 1, min channel 0) and the
    // game alpha still preserved.
    nlohmann::json j4;
    j4["rainbow"] = true;
    j4["rainbowSpeed"] = 0.5f;
    mod.loadConfig(j4);
    assert(mod.rainbowEnabled());
    const Color* rainbow = recolored(&overlay, storage, true);
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

    // Idle overlay (alpha 0) keeps alpha 0 through the replacement when the
    // caller still marks the flash active (actor path uses hurtTime).
    nlohmann::json j8;
    j8["rainbow"] = false;
    j8["damageColor"] = std::string("#FF0000");
    mod.loadConfig(j8);
    const Color idle{0.0f, 0.0f, 0.0f, 0.0f};
    const Color* idleOut = recolored(&idle, storage, true);
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
    // replacement colors (or the caller's own colors when disabled or idle) on
    // the world and the UI paths. A fresh module becomes the global instance,
    // so its default cyan (#00E5FF) is what the detours produce.
    DamageColorModule final;
    final.setMasterEnabled(true);

    // World path with an overlay flash active: overlay AND changeColor are
    // recolored, the changeColor alpha is preserved.
    g_setEntityConstantsOriginal = &recordSetEntityConstants;
    setEntityConstantsDetour(
        nullptr, nullptr,
        nullptr, nullptr, nullptr, &overlay,
        &change, &change2, nullptr, nullptr, nullptr,
        0.0f, 0.0f, 0.0f, 0.0f);
    assert(g_seenEntityOverlay && g_seenEntityOverlay != &overlay);
    assertFloatClose(g_seenEntityOverlay->r, 0.0f);
    assertFloatClose(g_seenEntityOverlay->g, 229.0f / 255.0f);
    assertFloatClose(g_seenEntityOverlay->b, 1.0f);
    assertFloatClose(g_seenEntityOverlay->a, overlay.a);
    assert(g_seenEntityChangeColor && g_seenEntityChangeColor != &change);
    assertFloatClose(g_seenEntityChangeColor->r, 0.0f);
    assertFloatClose(g_seenEntityChangeColor->g, 229.0f / 255.0f);
    assertFloatClose(g_seenEntityChangeColor->b, 1.0f);
    assertFloatClose(g_seenEntityChangeColor->a, change.a);

    // World path with no flash: the caller's colors pass through untouched.
    const Color idleOverlay{0.0f, 0.0f, 0.0f, 0.0f};
    setEntityConstantsDetour(
        nullptr, nullptr,
        nullptr, nullptr, nullptr, &idleOverlay,
        &change, &change2, nullptr, nullptr, nullptr,
        0.0f, 0.0f, 0.0f, 0.0f);
    assert(g_seenEntityOverlay == &idleOverlay);
    assert(g_seenEntityChangeColor == &change);

    // Actor path: the flash is detected from hurtTime even when the overlay
    // alpha is 0, so the change colors are recolored regardless.
    alignas(std::max_align_t) std::array<std::byte, 0x400> actor{};
    put(actor, bedrocktools::sdk::offsets::Actor::mHurtTime, 10);
    g_setupActorShaderParametersGlintOriginal = &recordSetupActorShaderParametersGlint;
    g_actorCalled = false;
    setupActorShaderParametersGlintDetour(
        nullptr, nullptr, actor.data(), &idleOverlay,
        &change, &change2, nullptr,
        0.0f, 0.0f, 0.0f, 0.0f, nullptr, nullptr,
        0.0f, 0, nullptr);
    assert(g_actorCalled);
    assert(g_seenActorChangeColor && g_seenActorChangeColor != &change);
    assertFloatClose(g_seenActorChangeColor->r, 0.0f);
    assertFloatClose(g_seenActorChangeColor->g, 229.0f / 255.0f);
    assertFloatClose(g_seenActorChangeColor->a, change.a);

    // Actor path outside the hurt window: pass-through.
    put(actor, bedrocktools::sdk::offsets::Actor::mHurtTime, 0);
    g_actorCalled = false;
    setupActorShaderParametersGlintDetour(
        nullptr, nullptr, actor.data(), &idleOverlay,
        &change, &change2, nullptr,
        0.0f, 0.0f, 0.0f, 0.0f, nullptr, nullptr,
        0.0f, 0, nullptr);
    assert(g_actorCalled);
    assert(g_seenActorOverlay == &idleOverlay);
    assert(g_seenActorChangeColor == &change);

    // UI path honours the enabled state and recolors while a flash is active.
    g_setupShaderParametersGlintOriginal = &recordSetupShaderParametersGlint;
    g_uiCalled = false;
    final.setMasterEnabled(false);
    setupShaderParametersGlintDetour(
        nullptr, &overlay, &change, nullptr, nullptr,
        0.0f, 0.0f, 0.0f, 0.0f, nullptr, nullptr, 0.0f);
    assert(g_uiCalled);
    assert(std::fabs(g_seenUiOverlay.g - overlay.g) < 1e-6f); // disabled: untouched
    assert(g_seenUiChangeColor == &change);

    final.setMasterEnabled(true);
    setupShaderParametersGlintDetour(
        nullptr, &overlay, &change, nullptr, nullptr,
        0.0f, 0.0f, 0.0f, 0.0f, nullptr, nullptr, 0.0f);
    assertFloatClose(g_seenUiOverlay.r, 0.0f);
    assertFloatClose(g_seenUiOverlay.g, 229.0f / 255.0f);
    assertFloatClose(g_seenUiOverlay.a, overlay.a);
    assertFloatClose(g_seenUiChangeColor->g, 229.0f / 255.0f);

    return 0;
}

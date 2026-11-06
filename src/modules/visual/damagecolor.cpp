#include "damagecolor.hpp"

#include "blockoutline_geometry.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

using Color = bedrocktools::sdk::Color;
using Clock = std::chrono::steady_clock;

// The game passes the hurt (and fire) overlay tint as a raw Color pointer to
// the actor shader-parameter setup functions. Entity constants and actor
// constants travel on two near-identical paths; the foil/glint paths serve
// the same overlay to UI (HUD/screen) draws. All four are replaced so the
// color is consistent in the world, in hand and on the HUD.
using SetEntityConstantsFn = void (*)(
    void*,
    void*,
    const Color*,
    const void*,
    const void*,
    const Color*,
    const Color*,
    const Color*,
    const Color*,
    const void*,
    const void*,
    float,
    float,
    float,
    float
);

using SetupActorShaderParametersGlintFn = void (*)(
    void*,
    void*,
    void*,
    const Color*,
    const Color*,
    const Color*,
    const Color*,
    float,
    float,
    float,
    float,
    const void*,
    const void*,
    float,
    std::uint8_t,
    const void*
);

using SetupFoilShaderParametersFn = void (*)(
    void*,
    const Color*,
    const Color*,
    const Color*,
    const void*
);

using SetupShaderParametersGlintFn = void (*)(
    void*,
    const Color*,
    const Color*,
    const Color*,
    const Color*,
    float,
    float,
    float,
    float,
    const void*,
    const void*,
    float
);

DamageColorModule* g_damageColor = nullptr;
SetEntityConstantsFn g_setEntityConstantsOriginal = nullptr;
SetupActorShaderParametersGlintFn g_setupActorShaderParametersGlintOriginal = nullptr;
SetupFoilShaderParametersFn g_setupFoilShaderParametersOriginal = nullptr;
SetupShaderParametersGlintFn g_setupShaderParametersGlintOriginal = nullptr;

// Builds the replacement overlay tint. The game animates the flash's alpha
// itself (0 while idle, ramping up while the hurt timer is active), so the
// alpha is copied verbatim from the incoming overlay and only the RGB tint is
// replaced. The configured RGB picker value is used unless Rainbow is on, in
// which case the hue cycles through the whole wheel at Rainbow Speed.
Color makeDamageColor(const Color* overlay) {
    const float alpha = overlay ? std::clamp(overlay->a, 0.0f, 1.0f) : 1.0f;

    std::uint32_t rgb = 0x0000E5FFu;
    if (g_damageColor) {
        if (g_damageColor->rainbowEnabled()) {
            const double seconds = std::chrono::duration<double>(
                Clock::now().time_since_epoch()).count();
            rgb = blockoutline::animatedRgb(0u, true, seconds, g_damageColor->rainbowSpeedValue());
        } else {
            rgb = g_damageColor->damageColor() & 0x00FFFFFFu;
        }
    }

    return {
        static_cast<float>((rgb >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFFu) / 255.0f,
        static_cast<float>(rgb & 0xFFu) / 255.0f,
        alpha
    };
}

// Returns the overlay pointer the game should see: the caller's own overlay
// when the module is disabled (or absent), otherwise a pointer to `storage`.
// Note the fire overlay travels through the same constant as the hurt flash,
// so an active module recolors it too.
const Color* customOverlay(const Color* overlay, Color& storage) {
    if (!g_damageColor || !g_damageColor->enabled) return overlay;
    if (!overlay) return overlay;
    storage = makeDamageColor(overlay);
    return &storage;
}

void setEntityConstantsDetour(
    void* entityConstants,
    void* renderContext,
    const Color* tileLightColor,
    const void* tileLightColorUV,
    const void* blockLightColor,
    const Color* overlay,
    const Color* changeColor,
    const Color* changeColor2,
    const Color* glintColor,
    const void* glintUVScale,
    const void* uvAnim,
    float uvOffset1,
    float uvOffset2,
    float uvRot1,
    float uvRot2
) {
    if (!g_setEntityConstantsOriginal) return;

    Color custom{};
    const Color* customOverlayColor = customOverlay(overlay, custom);
    g_setEntityConstantsOriginal(
        entityConstants,
        renderContext,
        tileLightColor,
        tileLightColorUV,
        blockLightColor,
        customOverlayColor,
        changeColor,
        changeColor2,
        glintColor,
        glintUVScale,
        uvAnim,
        uvOffset1,
        uvOffset2,
        uvRot1,
        uvRot2
    );
}

void setupActorShaderParametersGlintDetour(
    void* screenContext,
    void* entityContext,
    void* actor,
    const Color* overlay,
    const Color* changeColor,
    const Color* changeColor2,
    const Color* glintColor,
    float uvOffset1,
    float uvOffset2,
    float uvRot1,
    float uvRot2,
    const void* glintUVScale,
    const void* uvAnim,
    float br,
    std::uint8_t lightEmission,
    const void* lightEmissionColor
) {
    if (!g_setupActorShaderParametersGlintOriginal) return;

    Color custom{};
    const Color* customOverlayColor = customOverlay(overlay, custom);
    g_setupActorShaderParametersGlintOriginal(
        screenContext,
        entityContext,
        actor,
        customOverlayColor,
        changeColor,
        changeColor2,
        glintColor,
        uvOffset1,
        uvOffset2,
        uvRot1,
        uvRot2,
        glintUVScale,
        uvAnim,
        br,
        lightEmission,
        lightEmissionColor
    );
}

void setupFoilShaderParametersDetour(
    void* screenContext,
    const Color* overlay,
    const Color* changeColor,
    const Color* changeColor2,
    const void* uvScale
) {
    if (!g_setupFoilShaderParametersOriginal) return;

    Color custom{};
    const Color* customOverlayColor = customOverlay(overlay, custom);
    g_setupFoilShaderParametersOriginal(screenContext, customOverlayColor, changeColor, changeColor2, uvScale);
}

void setupShaderParametersGlintDetour(
    void* screenContext,
    const Color* overlay,
    const Color* changeColor,
    const Color* changeColor2,
    const Color* glintColor,
    float uvOffset1,
    float uvOffset2,
    float uvRot1,
    float uvRot2,
    const void* glintUVScale,
    const void* uvAnim,
    float br
) {
    if (!g_setupShaderParametersGlintOriginal) return;

    Color custom{};
    const Color* customOverlayColor = customOverlay(overlay, custom);
    g_setupShaderParametersGlintOriginal(
        screenContext,
        customOverlayColor,
        changeColor,
        changeColor2,
        glintColor,
        uvOffset1,
        uvOffset2,
        uvRot1,
        uvRot2,
        glintUVScale,
        uvAnim,
        br
    );
}

} // namespace

DamageColorModule::DamageColorModule()
    : Module("Damage Color", "Recolors the red hurt flash shown on entities (you included) when they take damage. The fire overlay flash is recolored too.") {
    g_damageColor = this;
    hideInHudEditor = true;
}

DamageColorModule::~DamageColorModule() {
    if (g_damageColor == this) g_damageColor = nullptr;
}

void DamageColorModule::onInit() {
    const auto entityAddress = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::ActorShaderManagerSetEntityConstants);
    const auto actorAddress = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::ActorShaderManagerSetupShaderParametersActorGlint);
    const auto foilAddress = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::ActorShaderManagerSetupFoilShaderParameters);
    const auto uiAddress = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::ActorShaderManagerSetupShaderParametersGlint);

    m_entityTarget = reinterpret_cast<void*>(entityAddress);
    m_actorTarget = reinterpret_cast<void*>(actorAddress);
    m_foilTarget = reinterpret_cast<void*>(foilAddress);
    m_uiTarget = reinterpret_cast<void*>(uiAddress);
    installHooks();
}

void DamageColorModule::installHooks() {
    if (!m_entityHooked && m_entityTarget) {
        const auto hook = bedrocktools::hooks::install(
            m_entityTarget,
            reinterpret_cast<void*>(&setEntityConstantsDetour),
            reinterpret_cast<void**>(&g_setEntityConstantsOriginal));
        m_entityHooked = hook != nullptr;
    }

    if (!m_actorHooked && m_actorTarget) {
        const auto hook = bedrocktools::hooks::install(
            m_actorTarget,
            reinterpret_cast<void*>(&setupActorShaderParametersGlintDetour),
            reinterpret_cast<void**>(&g_setupActorShaderParametersGlintOriginal));
        m_actorHooked = hook != nullptr;
    }

    if (!m_foilHooked && m_foilTarget) {
        const auto hook = bedrocktools::hooks::install(
            m_foilTarget,
            reinterpret_cast<void*>(&setupFoilShaderParametersDetour),
            reinterpret_cast<void**>(&g_setupFoilShaderParametersOriginal));
        m_foilHooked = hook != nullptr;
    }

    if (!m_uiHooked && m_uiTarget) {
        const auto hook = bedrocktools::hooks::install(
            m_uiTarget,
            reinterpret_cast<void*>(&setupShaderParametersGlintDetour),
            reinterpret_cast<void**>(&g_setupShaderParametersGlintOriginal));
        m_uiHooked = hook != nullptr;
    }
}

void DamageColorModule::onEnable() {
    installHooks();
}

void DamageColorModule::onDisable() {
}

void DamageColorModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    if (j.contains("damageColor") && j["damageColor"].is_string()) {
        const std::string text = j["damageColor"].get<std::string>();
        if ((text.size() == 7 || text.size() == 9) && text[0] == '#' &&
            text.find_first_not_of("0123456789abcdefABCDEF", 1) == std::string::npos) {
            try {
                const auto value = static_cast<std::uint32_t>(std::stoul(text.substr(1), nullptr, 16));
                m_damageColor.store(text.size() == 7 ? 0xFF000000u | value : value,
                                    std::memory_order_relaxed);
            } catch (...) {
            }
        }
    }

    if (j.contains("rainbow")) {
        try {
            m_rainbow.store(j["rainbow"].get<bool>(), std::memory_order_relaxed);
        } catch (...) {
        }
    }

    if (j.contains("rainbowSpeed")) {
        try {
            const float value = j["rainbowSpeed"].get<float>();
            if (std::isfinite(value)) {
                m_rainbowSpeed.store(std::clamp(value, 0.05f, 1.0f), std::memory_order_relaxed);
            }
        } catch (...) {
        }
    }
}

void DamageColorModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    char color[10];
    std::snprintf(color, sizeof(color), "#%08X", m_damageColor.load(std::memory_order_relaxed));
    j["damageColor"] = color;
    j["rainbow"] = m_rainbow.load(std::memory_order_relaxed);
    j["rainbowSpeed"] = rainbowSpeedValue();
}

std::uint32_t DamageColorModule::damageColor() const {
    return m_damageColor.load(std::memory_order_relaxed);
}

bool DamageColorModule::rainbowEnabled() const {
    return m_rainbow.load(std::memory_order_relaxed);
}

float DamageColorModule::rainbowSpeedValue() const {
    const float value = m_rainbowSpeed.load(std::memory_order_relaxed);
    if (!std::isfinite(value)) return 0.20f;
    return std::clamp(value, 0.05f, 1.0f);
}

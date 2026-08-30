#include "freelook.hpp"
#include "core/memory/Hooks.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>

// ---------------------------------------------------------------------------
// Free Look
//
// While active, the look input turns a virtual camera instead of the player:
//   * LocalPlayer::applyTurnDelta is intercepted. The turn the game would
//     have applied is measured (the game's own clamping included), undone on
//     the player and accumulated into the free camera instead.
//   * Around every LocalPlayer tick the body rotation is forced back to the
//     locked angle, so movement, attacks and the rotation sent to the server
//     keep the direction Free Look started with.
//   * Right after the tick the free camera angle is written into the player's
//     rotation component, which is what the frames until the next tick (and
//     with them the camera) render with.
//
// The applyTurnDelta hook is shared with the Zoom module; the two hooks chain,
// so Zoom's sensitivity scaling still applies while Free Look is active.
// ---------------------------------------------------------------------------

static FreeLookModule* g_freeLookMod = nullptr;

static void (*_applyTurnDelta_orig)(void*, bedrocktools::sdk::Vec2*) = nullptr;

// The first Vec2 of ActorRotationComponent is { x = pitch, y = yaw } (the
// same layout the Debug Menu and Hitbox modules read).
static bedrocktools::sdk::Vec2 readRotation(void* player) {
    bedrocktools::sdk::Vec2 rot{0.0f, 0.0f};
    if (!player || (uintptr_t)player < 0x1000) return rot;
    const uintptr_t component =
        *(uintptr_t*)((uintptr_t)player + bedrocktools::sdk::offsets::Actor::mActorRotationComponent);
    if (component < 0x1000) return rot;
    rot = *(bedrocktools::sdk::Vec2*)component;
    return rot;
}

static void writeRotation(void* player, const bedrocktools::sdk::Vec2& rot) {
    if (!player || (uintptr_t)player < 0x1000) return;
    const uintptr_t component =
        *(uintptr_t*)((uintptr_t)player + bedrocktools::sdk::offsets::Actor::mActorRotationComponent);
    if (component < 0x1000) return;
    *(bedrocktools::sdk::Vec2*)component = rot;
}

static void _applyTurnDelta_hook(void* _this, bedrocktools::sdk::Vec2* rotationDelta) {
    FreeLookModule* mod = g_freeLookMod;
    if (!mod || !mod->enabled || !rotationDelta) {
        if (_applyTurnDelta_orig) _applyTurnDelta_orig(_this, rotationDelta);
        return;
    }

    const bool redirect = mod->shouldRedirectTurn(_this);
    const bedrocktools::sdk::Vec2 before = readRotation(_this);

    if (!redirect) {
        if (_applyTurnDelta_orig) _applyTurnDelta_orig(_this, rotationDelta);
        // Pass-through: keep learning how the game applies turns, but only
        // from the local player — applyTurnDelta may be shared with mobs.
        if (_this == mod->player()) {
            const bedrocktools::sdk::Vec2 after = readRotation(_this);
            mod->onTurnObserved(_this, rotationDelta->x, rotationDelta->y,
                                after.x - before.x, after.y - before.y);
        }
        return;
    }

    if (mod->useMeasuredTurns()) {
        // Calibrated: let the game apply the turn with all of its internal
        // logic (clamping, Zoom's sensitivity scaling, ...), measure the
        // effect, undo it on the player and hand it to the free camera.
        if (_applyTurnDelta_orig) _applyTurnDelta_orig(_this, rotationDelta);
        const bedrocktools::sdk::Vec2 after = readRotation(_this);
        writeRotation(_this, before);
        mod->onTurnMeasured(rotationDelta->x, rotationDelta->y,
                            after.x - before.x, after.y - before.y);
    } else {
        // Not calibrated yet (no turn observed so far): block the turn so the
        // body stays locked and decode the raw argument with the assumed
        // { x = pitch, y = yaw } layout.
        bedrocktools::sdk::Vec2 zero{0.0f, 0.0f};
        if (_applyTurnDelta_orig) _applyTurnDelta_orig(_this, &zero);
        const bedrocktools::sdk::Vec2 after = readRotation(_this);
        if (after.x != before.x || after.y != before.y) {
            writeRotation(_this, before);
        }
        mod->onTurnRaw(rotationDelta->x, rotationDelta->y);
    }
}

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

FreeLookModule::FreeLookModule()
    : Module("Free Look", "Look around freely while your movement stays locked.") {
    this->keybind = 0;
    g_freeLookMod = this;
}

FreeLookModule::~FreeLookModule() {
    if (g_freeLookMod == this) g_freeLookMod = nullptr;
}

void FreeLookModule::pushSettings() {
    m_core.settings.maxYaw = static_cast<float>(m_maxYaw);
    m_core.settings.maxPitch = static_cast<float>(m_maxPitch);
    m_core.settings.smoothReturn = m_smoothReturn;
    m_core.settings.returnLerp = m_returnSpeed;
}

void FreeLookModule::syncRequestActive() {
    const bool keyWants = m_holdMode ? m_keyHeld : m_toggled;
    m_core.setRequestActive(keyWants || m_buttonActive);
}

void FreeLookModule::handlePlayerChanged(void* player) {
    // A different local player instance (respawn, dimension change, new
    // world): the previous state is meaningless and the old actor may
    // already be destroyed, so drop it without writing to it.
    m_core.forceInactive();
    m_player = player;
}

void FreeLookModule::restoreLockedRotation() {
    if (!m_player || !m_core.directing()) return;
    writeRotation(m_player, bedrocktools::sdk::Vec2{m_core.locked().pitch, m_core.locked().yaw});
}

bool FreeLookModule::shouldRedirectTurn(void* player) const {
    return m_core.directing() && player != nullptr && player == m_player;
}

void FreeLookModule::onTurnObserved(void* player, float argX, float argY, float dPitch, float dYaw) {
    (void)player;
    freelook::observeTurn(m_core.calibration, argX, argY, dPitch, dYaw);
}

void FreeLookModule::onTurnMeasured(float argX, float argY, float dPitch, float dYaw) {
    freelook::observeTurn(m_core.calibration, argX, argY, dPitch, dYaw);
    m_core.applyTurn(dPitch, dYaw);
}

void FreeLookModule::onTurnRaw(float argX, float argY) {
    float dPitch = 0.0f;
    float dYaw = 0.0f;
    freelook::decodeRawDelta(m_core.calibration, argX, argY, dPitch, dYaw);
    m_core.applyTurn(dPitch, dYaw);
}

void FreeLookModule::onPreTick(void* player) {
    if (!player) return;

    // A long gap between ticks means the world was left or reloaded (or the
    // process was suspended). The locked angle can be stale then, so restart
    // clean instead of snapping the player back to it.
    const auto now = std::chrono::steady_clock::now();
    const bool gapSuspect =
        m_lastTickValid &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastTick).count() > 1000;
    m_lastTick = now;
    m_lastTickValid = true;

    if (player != m_player) {
        handlePlayerChanged(player);
    } else if (gapSuspect && m_core.directing()) {
        m_core.forceInactive();
    }

    const bedrocktools::sdk::Vec2 current = readRotation(player);
    // Without the applyTurnDelta hook the look input cannot be redirected,
    // so never engage (the body would just freeze).
    const auto body = m_core.preTick(enabled && m_turnHooked,
                                     freelook::Angles{current.x, current.y});
    if (body) {
        writeRotation(player, bedrocktools::sdk::Vec2{body->pitch, body->yaw});
    }
}

void FreeLookModule::onPostTick(void* player) {
    if (!player || player != m_player) return;
    const auto view = m_core.postTick();
    if (view) {
        writeRotation(player, bedrocktools::sdk::Vec2{view->pitch, view->yaw});
    }
}

void FreeLookModule::onInit() {
    if (!m_turnHooked) {
        const uintptr_t addr = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::LocalPlayerApplyTurnDelta);
        if (addr != 0 &&
            bedrocktools::hooks::install((void*)addr, (void*)_applyTurnDelta_hook,
                                         (void**)&_applyTurnDelta_orig)) {
            m_turnHooked = true;
        }
    }

    // Pre-tick runs first: the locked body angle must be in place before the
    // rest of the tick (movement, MovePlayerPacket) reads the rotation.
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerPreTickEvent>(
        [this](auto& event) { onPreTick(event.player); },
        bedrocktools::events::EventPriority::First);
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { onPostTick(event.player); });
    bedrocktools::events::bus().subscribe<bedrocktools::events::ScreenStateEvent>(
        [this](auto& event) {
            if (event.phase != bedrocktools::events::ScreenPhase::Opened) return;
            // Keybind events are blocked while a screen is open, so a held
            // key would never see its release. Let go of it instead of
            // leaving Free Look stuck on.
            m_keyHeld = false;
            syncRequestActive();
        });

    pushSettings();
    updateOverlayButton();
}

void FreeLookModule::onEnable() {
    pushSettings();
}

void FreeLookModule::onDisable() {
    m_keyHeld = false;
    m_toggled = false;
    m_buttonActive = false;
    syncRequestActive();
    restoreLockedRotation();
    m_core.forceInactive();
}

void FreeLookModule::onKeybindEvent(const std::string& key, bool isDown) {
    if (key != "keybind") return;
    if (m_holdMode) {
        m_keyHeld = isDown;
    } else if (isDown) {
        m_toggled = !m_toggled;
    }
    syncRequestActive();
}

void FreeLookModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_holdMode")) m_holdMode = j["m_holdMode"].get<bool>();
    if (j.contains("m_smoothReturn")) m_smoothReturn = j["m_smoothReturn"].get<bool>();
    if (j.contains("m_returnSpeed")) m_returnSpeed = j["m_returnSpeed"].get<float>();
    if (j.contains("m_maxYaw")) m_maxYaw = j["m_maxYaw"].get<int>();
    if (j.contains("m_maxPitch")) m_maxPitch = j["m_maxPitch"].get<int>();
    if (j.contains("m_overlayToggle")) m_overlayToggle = j["m_overlayToggle"].get<bool>();

    m_returnSpeed = std::max(0.05f, std::min(1.0f, m_returnSpeed));
    m_maxYaw = std::max(15, std::min(180, m_maxYaw));
    m_maxPitch = std::max(15, std::min(90, m_maxPitch));

    pushSettings();
    updateOverlayButton();
}

void FreeLookModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_holdMode"] = m_holdMode;
    j["m_smoothReturn"] = m_smoothReturn;
    j["m_returnSpeed"] = m_returnSpeed;
    j["m_maxYaw"] = m_maxYaw;
    j["m_maxPitch"] = m_maxPitch;
    j["m_overlayToggle"] = m_overlayToggle;
}

// ---------------------------------------------------------------------------
// Overlay button (Zoom-style frame + an "eye with look-around arrows" glyph).
// ---------------------------------------------------------------------------

// Frame shared by both button states (same frame as the Zoom button).
static const char* freeLookFrame =
    "<path fill=\"#C6C6C6\" stroke=\"#373737\" stroke-width=\"2\" d=\"M2,2 L62,2 L62,62 L2,62 Z M4,4 L60,4 L60,60 L4,60 Z\"/>"
    "<path fill=\"#8B8B8B\" stroke=\"#5B5B5B\" stroke-width=\"2\" d=\"M6,6 L58,6 L58,58 L6,58 Z M8,8 L56,8 L56,56 L8,56 Z\"/>";

// Glyph: an eye flanked by two look-around arrows.
static const char* freeLookGlyph =
    // Left look arrow: shaft + head.
    "<path fill=\"#373737\" d=\"M6,30 L13,30 L13,34 L6,34 Z\"/>"
    "<path fill=\"#373737\" d=\"M12,26 L4,32 L12,38 Z\"/>"
    // Right look arrow: shaft + head.
    "<path fill=\"#373737\" d=\"M51,30 L58,30 L58,34 L51,34 Z\"/>"
    "<path fill=\"#373737\" d=\"M52,26 L60,32 L52,38 Z\"/>"
    // Eye ring (outer lens minus inner lens, even-odd) + pupil.
    "<path fill=\"#373737\" fill-rule=\"evenodd\" d=\""
    "M32,22 C25,22 19,28 15,32 C19,36 25,42 32,42 C39,42 45,36 49,32 C45,28 39,22 32,22 Z "
    "M32,27 C27,27 22.5,30 18.5,32 C22.5,34 27,37 32,37 C37,37 41.5,34 45.5,32 C41.5,30 37,27 32,27 Z\"/>"
    "<path fill=\"#373737\" d=\"M28.5,32 A3.5,3.5 0 1,0 35.5,32 A3.5,3.5 0 1,0 28.5,32 Z\"/>";

// The two SVGs are assembled from the shared frame/glyph fragments above so
// the enabled variant stays in sync with the disabled one. The enabled icon
// scales the glyph in slightly, the same "pressed in" look as Zoom's.
static const std::string freeLookDisabledSvg =
    std::string("<svg viewBox=\"0 0 64 64\" xmlns=\"http://www.w3.org/2000/svg\">\n    ")
    + freeLookFrame + "\n    " + freeLookGlyph + "\n</svg>";

static const std::string freeLookEnabledSvg =
    std::string("<svg viewBox=\"0 0 64 64\" xmlns=\"http://www.w3.org/2000/svg\">\n    ")
    + freeLookFrame
    + "\n    <g transform=\"translate(32, 32) scale(0.85) translate(-32, -32)\">\n        "
    + freeLookGlyph + "\n    </g>\n</svg>";

void FreeLookModule::updateOverlayButton() {
    if (m_overlayToggle) {
        pl::modmenu::ButtonBuilder("bedrocktoolsplus.Free Look.Button", "Free Look")
                .moduleId("bedrocktoolsplus.Free Look")
                .behavior(pl::modmenu::ButtonBehavior::Toggle)
                .stylePreset(pl::modmenu::ButtonStylePreset::Accent)
                .styleColors(0x00000001, 0x00000001, 0x00000001)
                .svgIcon(freeLookDisabledSvg.c_str())
                .activeSvgIcon(freeLookEnabledSvg.c_str())
                .onEvent([this](std::string_view buttonId, pl::modmenu::ButtonEvent event, float value) {
                    (void)buttonId;
                    if (event == pl::modmenu::ButtonEvent::StateChanged) {
                        m_buttonActive = (value > 0.5f);
                        syncRequestActive();
                    }
                })
                .registerButton();
    } else {
        pl::modmenu::unregisterButton("bedrocktoolsplus.Free Look.Button");
    }
}

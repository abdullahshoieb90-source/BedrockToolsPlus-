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
// On modern Bedrock the camera and the body are two separate things:
//
//   * LocalPlayer::applyTurnDelta feeds the new camera system. It moves the
//     camera and does not touch the actor rotation.
//   * The actor rotation component (Actor::mActorRotationComponent, Vec2
//     { x = pitch, y = yaw }) is the body: player model, movement direction
//     and the rotation that is sent to the server.
//
// So Free Look lets the look input reach the camera as usual and freezes the
// body instead:
//
//   * The applyTurnDelta hook passes the deltas straight through while
//     active; it only trims them where the camera would swing further than
//     Max Yaw / Max Pitch from the locked angle, and zeroes them while the
//     release animation is steering the camera home. The camera angle is
//     tracked by summing whatever was allowed through.
//   * The locked body angle is written into the rotation component at
//     LocalPlayerPreTickEvent (so the tick, and with it the movement and the
//     MovePlayerPacket, uses it) and again at LocalPlayerTickEvent (so the
//     player model renders with it).
//   * On release the accumulated swing is undone with compensating deltas
//     sent through `_applyTurnDelta_orig` from the post-tick — one full step
//     with Smooth Return off, an exponential lerp otherwise. The body is
//     unlocked only once the camera has arrived back on it.
//
// The applyTurnDelta hook is shared with the Zoom module; the two hooks
// chain, so Zoom's sensitivity scaling still applies while Free Look is
// active.
// ---------------------------------------------------------------------------

static FreeLookModule* g_freeLookMod = nullptr;

static void (*_applyTurnDelta_orig)(void*, bedrocktools::sdk::Vec2*) = nullptr;

// The first Vec2 of ActorRotationComponent is { x = pitch, y = yaw } (the
// same layout the Debug Menu, Hitbox and Wings modules read).
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
    if (!mod || !rotationDelta) {
        if (_applyTurnDelta_orig) _applyTurnDelta_orig(_this, rotationDelta);
        return;
    }

    // The delta always goes on to the camera; filterTurnDelta only decides
    // how much of it gets there. The body is never written from here.
    bedrocktools::sdk::Vec2 delta = mod->filterTurnDelta(_this, *rotationDelta);
    if (_applyTurnDelta_orig) _applyTurnDelta_orig(_this, &delta);
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
    // world): the previous state is meaningless, the old actor may already be
    // destroyed and the camera has been reset by the game anyway, so drop the
    // swing without trying to compensate it.
    m_core.forceInactive();
    m_player = player;
}

void FreeLookModule::sendCameraDelta(void* player, const freelook::Turn& delta) {
    m_lastCameraDelta = delta;
    ++m_cameraDeltaCount;
    if (!_applyTurnDelta_orig || !player) return;
    // Straight to the original: going through our own hook would just have
    // the core swallow it again.
    bedrocktools::sdk::Vec2 vec{delta.pitch, delta.yaw};
    _applyTurnDelta_orig(player, &vec);
}

bool FreeLookModule::shouldInterceptTurn(void* player) const {
    return enabled && m_core.directing() && player != nullptr && player == m_player;
}

bedrocktools::sdk::Vec2 FreeLookModule::filterTurnDelta(void* player,
                                                        const bedrocktools::sdk::Vec2& delta) {
    // Not ours (Free Look idle, or a turn for some other actor): pass it on
    // untouched so the camera behaves exactly as it would without the module
    // and Zoom keeps its sensitivity scaling.
    if (!shouldInterceptTurn(player)) return delta;

    // applyTurnDelta takes { x = pitch, y = yaw }, the same identity layout
    // the rotation component uses.
    const freelook::Turn allowed = m_core.filterTurn(freelook::Turn{delta.x, delta.y});
    return bedrocktools::sdk::Vec2{allowed.pitch, allowed.yaw};
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
    // Without the applyTurnDelta hook the camera cannot be steered, so never
    // engage — the body would freeze together with the camera.
    const auto body = m_core.preTick(enabled && m_turnHooked,
                                     freelook::Angles{current.x, current.y});
    if (body) {
        writeRotation(player, bedrocktools::sdk::Vec2{body->pitch, body->yaw});
    }
}

void FreeLookModule::onPostTick(void* player) {
    if (!player || player != m_player) return;
    const auto outcome = m_core.postTick();
    // Re-assert the lock after the tick: the tick itself may have written the
    // rotation, and the player model is rendered from it.
    if (outcome.body) {
        writeRotation(player, bedrocktools::sdk::Vec2{outcome.body->pitch, outcome.body->yaw});
    }
    // Release animation: walk the camera back onto the body.
    if (outcome.camera) {
        sendCameraDelta(player, *outcome.camera);
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

    // Snap the camera back onto the body in one step and release the lock;
    // the tick handlers stop running the return once the module is off.
    if (m_core.directing()) {
        const freelook::Turn swing = m_core.swing();
        if (swing.pitch != 0.0f || swing.yaw != 0.0f) {
            sendCameraDelta(m_player, freelook::Turn{-swing.pitch, -swing.yaw});
        }
        if (m_player) {
            writeRotation(m_player, bedrocktools::sdk::Vec2{m_core.locked().pitch,
                                                           m_core.locked().yaw});
        }
    }
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

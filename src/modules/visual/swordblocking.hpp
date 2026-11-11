#pragma once

#include "../Module.hpp"
#include "swordblocking_logic.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

// Sword Blocking
//
// Recreates the Java-Edition "sword block" on Bedrock, client-side and purely
// visual: while you hold a sword and keep the use control down, the sword in
// your first-person hand glides into a guard pose (raised, tilted across the
// face) and a small shield indicator appears under the crosshair.
//
// Nothing here changes combat. Bedrock has no sword blocking in its protocol
// and damage is decided by the server, so the module deliberately never sends
// a packet, never cancels an attack and never rewrites a health value; it only
// moves the first-person item matrix at render time and paints HUD geometry.
// Because of that it is safe to use anywhere and it cannot desync anything.
//
// How it works
//
//   * The local-player tick samples the carried stack (Player vtable slot 78)
//     and asks its Item description id whether it is a sword (see
//     isSwordName in swordblocking_logic.hpp), so modded swords that keep the
//     vanilla "<material>_sword" naming are picked up for free. No new game
//     signature is needed: the item pointer is reached by the same
//     ItemStackBase layout the inventory HUD already uses.
//   * Hold state comes from three sources - the mouse callback, a launcher
//     overlay button (Behavior::Hold, for touch) and an optional keybind - and
//     the "Hold Style" radio decides which of them count.
//   * A single weight eases between 0 and 1 with the Fade Speed setting. The
//     weight drives both the pose and the indicator, so they can never
//     disagree, and a pause (menu, app switch) is capped by maxFrameDelta so
//     the sword never snaps.
//   * The pose is applied in ItemInHandRenderer::renderObject - the one call
//     that actually owns the item's matrix - but only while the surrounding
//     first-person scopes say it is the main hand (renderFirstPerson ->
//     renderHand / RenderItem, both of which carry the hand flag, and both of
//     which are hooked here). The current matrix is read out of the render
//     context's matrix stack, multiplied by the local pose, used for the draw
//     and written back afterwards, and a non-finite pose or matrix is rejected
//     instead of being pushed into the game's stack. Third person never goes
//     through these scopes, so the pose is automatically first-person only.
//   * The indicator is submitted as overlay draw commands from onFrame, the
//     same path the Crosshair module uses, and honours a "Show In Third Person"
//     toggle observed through Options::getPlayerViewPerspective().
//   * The guard shape comes either from a "Pose Preset" radio (Classic, High,
//     Wide, Subtle) or from the individual Tilt/Pitch/Spin/Lift/Across/Forward/
//     Scale sliders, which keep the radio on Custom once they are touched.
//
// The maths lives in swordblocking_logic.hpp so it is host-testable; see
// tests/swordblocking_test.cpp.
class SwordBlockingModule final : public Module {
public:
    SwordBlockingModule();
    ~SwordBlockingModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool onMouseEvent(int button, bool isDown) override;
    bool onKeyEvent(int key, bool isDown) override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;

    // ---- settings (mirrored into the mod menu by saveConfig) ----------------

    // Hold Style radio, stored by index (append-only, see HoldMode).
    int m_holdStyle = static_cast<int>(swordblocking::HoldMode::Auto);
    // Android MotionEvent button code used by Right Click / Auto (0 = none).
    int m_mouseButton = 2;
    // Optional dedicated hold key (0 = none). Named with "keybind" so the
    // launcher renders it as a key picker instead of a slider.
    int m_holdKeybind = 0;
    // Registers the launcher overlay hold button (the touch path).
    bool m_screenButton = true;

    float m_fadeSpeed = swordblocking::defaultFadeSpeed;

    // Guard preset radio, stored by index (append-only, see PosePreset). Picking
    // a preset writes the five pose values below; moving a pose slider switches
    // the radio to Custom.
    int m_posePreset = static_cast<int>(swordblocking::PosePreset::Classic);

    // Pose, all in Bedrock model units / degrees.
    float m_tiltDegrees = 27.0f;   // the guard swing, in the screen plane
    float m_pitchDegrees = 10.0f;  // tips the blade toward the camera
    float m_spinDegrees = 20.0f;   // turns the flat of the blade to the camera
    float m_lift = 4.0f;
    float m_across = 3.0f;
    float m_forward = 2.0f;
    float m_poseScale = 1.12f;

    // Shield indicator under the crosshair.
    bool m_indicator = true;
    std::string m_indicatorColor = "#7FE3FF";
    float m_indicatorOpacity = 0.85f;
    float m_indicatorScale = 1.0f;
    // Draw the indicator in third person too (the pose itself is first-person
    // only, like the vanilla hand).
    bool m_showThirdPerson = false;

    // ---- runtime ------------------------------------------------------------

    // Current eased weight, 0 (idle) .. 1 (fully raised).
    float poseWeight() const { return m_weight.load(std::memory_order_acquire); }
    swordblocking::PoseParams poseParams() const;
    swordblocking::IndicatorStyle indicatorStyle() const;

    // Called on every local-player tick; samples the carried item.
    void onPlayerTick(void* player);

    // Copies the named preset into the pose values above (a no-op for Custom).
    void applyPosePreset(swordblocking::PosePreset preset);

    // True when the pose sliders still hold exactly the values of the selected
    // preset, i.e. nothing has been tuned by hand.
    bool poseMatchesPreset() const;

    // Set from the Options::getPlayerViewPerspective hook (false while the
    // camera is in first person), so the indicator can follow the same rule
    // the Crosshair module uses.
    std::atomic_bool m_thirdPerson{false};

    // Live input state: the three hold sources are written from the input
    // callbacks (mouse, launcher overlay button, keybind) and `m_swordHeld`
    // from the player tick. Kept public next to the settings because they are
    // the module's observable state, not an implementation detail (the host
    // tests drive them directly).
    std::atomic_bool m_mouseHeld{false};
    std::atomic_bool m_buttonHeld{false};
    std::atomic_bool m_keyHeld{false};
    std::atomic_bool m_swordHeld{false};

    // True while the module is actually showing the guard pose, i.e. enabled,
    // a sword is carried and one of the configured hold sources is down.
    bool guardActive() const;

    // Whether the launcher overlay hold button is currently registered (the
    // touch path). False when the option is off or the button was rejected.
    bool overlayButtonRegistered() const { return m_buttonRegistered; }

private:
    void refreshHoldState(std::chrono::steady_clock::time_point now);
    void syncOverlayButton();
    void unregisterOverlayButton();

    std::atomic<float> m_weight{0.0f};

    std::mutex m_weightMutex;
    std::chrono::steady_clock::time_point m_lastWeightUpdate{};
    bool m_weightClockValid = false;

    std::atomic_bool m_submitted{false};
    bool m_buttonRegistered = false;
};

extern SwordBlockingModule* g_swordBlocking;

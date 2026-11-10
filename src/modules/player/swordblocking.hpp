#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/world/Actor.hpp>

#include <cstdint>

// Sword Blocking
//
// Puts the classic Java 1.7 sword-block pose on the local player: while a
// sword is in the main hand and the trigger is satisfied (sneak held,
// always-on, or a latched toggle), the player keeps the blade braced across
// the chest instead of dangling at the side - the same silhouette MarkClient's
// "Sword Blocking" module draws in 1.8 PvP.
//
// Client-side cosmetic: nothing is sent to the server and no gameplay value is
// touched, so it works in any world and on any server.
//
// How the pose is produced
// ------------------------
// Bedrock has no per-bone animation control from a native mod, and the
// third-person player mesh is drawn by the game's own renderer. Instead of
// driving the skeleton (fragile, offset-heavy and version-bound), the module
// draws a small tinted "blade + crossguard + grip (+ brace arm)" overlay in
// world space through the same RenderLevel/tessellator route the Wings and
// Hitbox modules already use, anchored to the player's AABB and body yaw
// every frame. Only signatures and offsets the SDK already pins are used, so
// the feature survives game updates that do not move them.
//
// Triggers
// --------
//   * Sneak   - hold sneak while a sword is selected (the original 1.7
//               behaviour; the latched mobile sneak state counts too).
//               Default.
//   * Always  - pose whenever a sword is selected.
//   * Toggle  - press the module keybind to latch the pose on/off without
//               having to hold anything.
//
// Settings (all editable from the launcher mod menu, all persisted in
// config.json)
// ----------------------------------------------------
//   m_trigger            radio : Sneak, Always, Toggle
//   m_anySword           bool  : accept any item whose identifier ends in
//                                "_sword" (off: iron/diamond/netherite only)
//   m_showArm            bool  : draw the brace arm along with the blade
//   m_showInFirstPerson  bool  : keep the pose visible in first person
//   m_bladeColor         #RGB  : tint of the blade
//   m_guardColor         #RGB  : tint of the crossguard
//   m_scale              float : overall size multiplier (0.5 .. 2.0)
//   m_offsetX/Y/Z        float : fine placement nudges in blocks (+-2)
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

    // With the Toggle trigger a keybind press latches the pose instead of
    // toggling the module (see the cpp for the state machine).
    void onKeybindEvent(const std::string& key, bool isDown) override;

    // ---- Settings (public: the render hook reads them directly) ----
    // Index into the trigger list: 0 = Sneak, 1 = Always, 2 = Toggle.
    int m_trigger = 0;
    bool m_anySword = true;
    bool m_showArm = true;
    bool m_showInFirstPerson = false;
    std::uint32_t m_bladeColor = 0xFFFFFFFFu; // stored as "#RRGGBB" in config
    std::uint32_t m_guardColor = 0xFFE08A2Eu;
    float m_scale = 1.0f;
    float m_offsetX = 0.0f;
    float m_offsetY = 0.0f;
    float m_offsetZ = 0.0f;

    // Latched state for the Toggle trigger.
    bool m_latched = false;

    // Called from the LocalPlayerTickEvent subscription: refreshes the cached
    // player pointer, the "sword in the main hand" flag and the pose state.
    void onLocalPlayerTick(bedrocktools::sdk::Actor* player);

    // Clears the pose and the latch (module disabled / re-enabled).
    void resetPose();

    // Read by the render hook. Written on the tick thread, read on the render
    // thread; plain scalars, so a torn read is harmless.
    bool poseActive() const { return m_poseActive; }
    bool hasSword() const { return m_hasSword; }

private:
    bool m_poseActive = false;
    bool m_hasSword = false;
    bool m_patched = false;
    void* m_patchTarget = nullptr;

    void applyPatch();
};

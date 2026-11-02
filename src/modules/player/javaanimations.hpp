#pragma once

#include "../Module.hpp"
#include "javaanimations_curve.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>

// Java Animations
//
// A first-person animation module in the spirit of the Java 1.7 player
// animation packs: it reshapes how the local player's hand and held item move
// instead of touching the player model, so it needs no resource pack and no
// skin patch, and it takes effect the moment the module is switched on.
//
// Three things are animated, all from javaanimations_curve.hpp:
//
//   * the rest stance - Java 1.7 holds the item lower and flatter
//   * the swing       - one large overhead arc that runs for six ticks
//   * the use raise   - eating and drinking lift the arm to the mouth
//
// The piece that makes it look like an animation rather than a static offset is
// the timing: LocalPlayer::swing arms a timer, Mob::getModifiedSwingDuration
// supplies the length of that swing (and is overridden with the 1.7 length of
// six ticks), and the pose is sampled from the timer on the render thread, so
// the arc plays out smoothly at any frame rate.
//
// The pose is applied in the hand's own space: the module captures the matrix
// at the top of ItemInHandRenderer::renderFirstPerson / renderHand and rebuilds
// the item and arm matrices as base * local * pose, restoring the matrix
// afterwards so nothing else inherits it. The arm and the item are both
// transformed, so the hand does not slide out from under the item.
//
// Scope, stated plainly: this is a first-person (your own hand) module. The
// third-person player model is driven by resource-pack animations, which a
// native module cannot replace - changing it still needs an animation pack.
class JavaAnimationsModule final : public Module {
public:
    JavaAnimationsModule();
    ~JavaAnimationsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from the LocalPlayer::swing detour. `player` is the actor that
    // swung; anything that is not the tracked local player is ignored so other
    // players' swings never move your hand.
    void onSwing(void* player, std::uint8_t source);

    // Called from the GameModeActionEvent::UseItem subscription.
    void onUseItem();

    // Called from the LocalPlayerTickEvent subscription to keep track of which
    // actor is the local player.
    void onLocalPlayerTick(void* player);

    // Mob::getModifiedSwingDuration override: returns the Java swing length for
    // the local player and leaves every other mob alone.
    int overrideSwingTicks(void* mob, int originalTicks) const;

    // Samples the animation at the current instant and returns the pose for the
    // given hand. Advances the swing/use timers by the time elapsed since the
    // previous sample, so a stall (a paused game, a long frame) can never make
    // the pose jump.
    javaanim::HandPose poseNow(bool mainHand);

    javaanim::Settings settings() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_settings;
    }

    int swingTicks() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_settings.style == javaanim::SwingStyle::Vanilla ? 0 : m_swingTicks;
    }

    void* localPlayer() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_localPlayer;
    }

    static JavaAnimationsModule* instance() { return s_instance; }

private:
    void resetState();

    mutable std::mutex m_mutex;
    javaanim::Settings m_settings{};
    javaanim::SwingState m_swing{};
    javaanim::UseState m_use{};
    int m_swingTicks = javaanim::kDefaultSwingTicks;
    void* m_localPlayer = nullptr;

    using Clock = std::chrono::steady_clock;
    Clock::time_point m_lastSample{};
    bool m_haveSample = false;

    static JavaAnimationsModule* s_instance;
};

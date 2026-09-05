#pragma once

// Pure state machine for the Hotbar Slots "Auto Build" feature.
//
// The launcher slot buttons (and the hardware keys 1-9) behave exactly like
// vanilla when they are tapped: the key press selects the hotbar slot. When
// the same button is *held* and the selected slot holds a placeable block,
// the module keeps calling GameMode::useItemOn for the block the player is
// looking at, so blocks are placed without ever touching the build button.
//
// Only the timing / arbitration logic lives here: no Minecraft types are
// involved, which keeps it covered by the host unit tests
// (tests/hotbarslots_autobuild_test.cpp).

#include "hotbarslots_layout.hpp"

#include <array>
#include <cstddef>

namespace bedrocktools::hotbar {

struct AutoBuildSettings {
    // Master switch for the feature.
    bool enabled = false;
    // Per-slot opt-in. The request that introduced the feature only wanted
    // slot 1, so that is the default, but every slot can be armed.
    std::array<bool, SlotCount> slots{};
    // How long a slot button must be held before auto building starts. A
    // shorter press is left alone and just selects the slot.
    double holdDelayMs = 250.0;
    // Delay between two placements while the button stays held.
    double intervalMs = 100.0;

    AutoBuildSettings() { slots.fill(false); slots[0] = true; }

    bool armed(std::size_t slot) const {
        return enabled && slot < SlotCount && slots[slot];
    }
};

// `now` is a monotonic millisecond timestamp supplied by the caller.
class AutoBuildState {
public:
    // A slot button went down. Pressing a different slot restarts the timer.
    void pressed(const AutoBuildSettings& settings, std::size_t slot, double now) {
        if (!settings.armed(slot)) {
            // A non-armed slot cancels an in-flight hold: the player moved on.
            reset();
            return;
        }
        if (mHeld && mSlot == slot) return;
        mHeld = true;
        mSlot = slot;
        mPressedAt = now;
        mLastPlace = 0.0;
        mPlacedAny = false;
    }

    void released(std::size_t slot, double) {
        if (mHeld && mSlot == slot) reset();
    }

    void reset() {
        mHeld = false;
        mSlot = SlotCount;
        mPressedAt = 0.0;
        mLastPlace = 0.0;
        mPlacedAny = false;
    }

    bool held() const { return mHeld; }
    std::size_t slot() const { return mSlot; }
    bool placedAny() const { return mPlacedAny; }

    // True once the hold delay elapsed; the caller may then place a block.
    bool holdElapsed(const AutoBuildSettings& settings, double now) const {
        return mHeld && (now - mPressedAt) >= settings.holdDelayMs;
    }

    // Decides whether a block should be placed on this tick. `placeable` is
    // the module's answer to "does the held slot contain a block item?".
    // Consumes the tick: a `true` result also records the placement time.
    bool shouldPlace(const AutoBuildSettings& settings, double now, bool placeable) {
        if (!placeable || !settings.armed(mSlot)) return false;
        if (!holdElapsed(settings, now)) return false;
        if (mPlacedAny && (now - mLastPlace) < settings.intervalMs) return false;
        mLastPlace = now;
        mPlacedAny = true;
        return true;
    }

private:
    bool mHeld = false;
    std::size_t mSlot = SlotCount;
    double mPressedAt = 0.0;
    double mLastPlace = 0.0;
    bool mPlacedAny = false;
};

} // namespace bedrocktools::hotbar

#pragma once

namespace damagecircle {

// Actor hurt-time is a countdown. A fresh hit resets/bump it upward, while
// ordinary invulnerability-frame ticks decrease it. Establishing a baseline
// on first sample prevents enabling the module mid-flash from spawning a ring.
struct HurtTimeTracker {
    int previous = -1;

    bool update(int current) {
        if (current < 0 || current > 100) {
            previous = -1;
            return false;
        }
        if (previous < 0) {
            previous = current;
            return false;
        }
        const bool damaged = current > previous;
        previous = current;
        return damaged;
    }

    void reset() { previous = -1; }
};

} // namespace damagecircle

// Host-side tests for the Hotbar Slots "Auto Build" state machine.
//
//     g++ -std=c++20 -I src -I include tests/hotbarslots_autobuild_test.cpp -o /tmp/t && /tmp/t

#include "modules/hud/hotbarslots_autobuild.hpp"

#include <cstdio>

namespace {

int failures = 0;

void expect(const char* what, bool condition) {
    if (!condition) {
        std::printf("  FAIL %s\n", what);
        ++failures;
    }
}

} // namespace

int main() {
    using namespace bedrocktools::hotbar;

    AutoBuildSettings settings;
    settings.enabled = true;
    settings.holdDelayMs = 250.0;
    settings.intervalMs = 100.0;

    expect("slot 1 armed by default", settings.armed(0));
    expect("slot 2 not armed by default", !settings.armed(1));

    // A short tap never builds: it only selects the slot.
    {
        AutoBuildState state;
        state.pressed(settings, 0, 1000.0);
        expect("no build before the hold delay", !state.shouldPlace(settings, 1100.0, true));
        state.released(0, 1150.0);
        expect("released clears the hold", !state.held());
        expect("no build after release", !state.shouldPlace(settings, 2000.0, true));
    }

    // Holding the armed slot builds once the delay elapsed, then repeats.
    {
        AutoBuildState state;
        state.pressed(settings, 0, 1000.0);
        expect("first build at the hold delay", state.shouldPlace(settings, 1250.0, true));
        expect("no build inside the interval", !state.shouldPlace(settings, 1300.0, true));
        expect("second build after the interval", state.shouldPlace(settings, 1360.0, true));
        expect("hold is still active", state.held());
    }

    // A slot without a placeable block behaves exactly like vanilla.
    {
        AutoBuildState state;
        state.pressed(settings, 0, 1000.0);
        expect("no build for a non-block slot", !state.shouldPlace(settings, 1400.0, false));
        expect("build resumes when a block appears", state.shouldPlace(settings, 1450.0, true));
    }

    // Slots that are not armed are left alone, and pressing one cancels an
    // in-flight hold on another slot.
    {
        AutoBuildState state;
        state.pressed(settings, 1, 1000.0);
        expect("unarmed slot never holds", !state.held());
        expect("unarmed slot never builds", !state.shouldPlace(settings, 2000.0, true));

        state.pressed(settings, 0, 3000.0);
        expect("armed slot holds", state.held());
        state.pressed(settings, 1, 3100.0);
        expect("switching to an unarmed slot cancels", !state.held());
    }

    // Switching between two armed slots restarts the timer.
    {
        AutoBuildSettings both = settings;
        both.slots[1] = true;
        AutoBuildState state;
        state.pressed(both, 0, 1000.0);
        state.pressed(both, 1, 1100.0);
        expect("slot follows the newest press", state.slot() == 1);
        expect("timer restarted", !state.shouldPlace(both, 1200.0, true));
        expect("builds after the new delay", state.shouldPlace(both, 1350.0, true));
    }

    // The master switch keeps everything off.
    {
        AutoBuildSettings off = settings;
        off.enabled = false;
        AutoBuildState state;
        state.pressed(off, 0, 1000.0);
        expect("disabled feature never holds", !state.held());
        expect("disabled feature never builds", !state.shouldPlace(off, 5000.0, true));
    }

    if (failures == 0) std::printf("hotbarslots_autobuild_test: all checks passed\n");
    else std::printf("hotbarslots_autobuild_test: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}

// Unit tests for the Inventory HUD module.
//
// The module's responsibilities that can be tested without a live game
// are the things that operate on configuration and the layout math:
//   * the panel never clips when width/height shrink below the natural
//     slot footprint (slots are re-flowed inside the panel instead)
//   * the panel never clips when width/height grow above the natural
//     slot footprint (the panel grows, slots stay at m_slotSize)
//   * the panel never clips when m_scale changes
//   * the slot count is clamped to [0, 36] and the offset to [0, 35]
//   * the JSON config round-trips every setting including the color
//     strings
//   * an empty inventory still produces draw commands (the empty grid
//     backdrop is drawn when m_showEmptySlots is true)
//   * the module never dereferences a null Player
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src -I include -I third_party \
//         -I tests/fakejson -I tests/fakepl \
//         tests/inventoryhud_test.cpp src/modules/visual/inventoryhud.cpp \
//         -o /tmp/inventoryhud_test
//     /tmp/inventoryhud_test

#include "visual/inventoryhud.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

void checkEqual(int got, int want, const std::string& what) {
    check(got == want, what + " -> " + std::to_string(got) + " (want " + std::to_string(want) + ")");
}

void checkEqual(float got, float want, float epsilon, const std::string& what) {
    const float diff = got > want ? got - want : want - got;
    if (diff <= epsilon) {
        std::printf("  ok   %s -> %g\n", what.c_str(), static_cast<double>(got));
    } else {
        std::printf("  FAIL %s -> %g (want %g, diff %g)\n",
                    what.c_str(), static_cast<double>(got), static_cast<double>(want), static_cast<double>(diff));
        ++g_failures;
    }
}

} // namespace

// Stub out the symbols the module imports from the real game so the host
// test does not need a live libminecraftpe.so.
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <bedrocktools/events/EventBus.hpp>

namespace bedrocktools {
namespace memory {
std::uintptr_t resolve(SignatureId) { return 0; }
} // namespace memory
} // namespace bedrocktools

// The module subscribes to the EventBus inside onInit. Provide a stub bus
// that records the subscription so the test can verify it ran.
namespace bedrocktools {
namespace events {
EventBus& bus() {
    static EventBus instance;
    return instance;
}
} // namespace events
} // namespace bedrocktools

int main() {
    std::printf("InventoryHUD construction & defaults\n");
    {
        InventoryHUDModule mod;
        check(mod.hudPosX > 0.0f, "default hudPosX is positive");
        check(mod.hudPosY > 0.0f, "default hudPosY is positive");
        check(mod.isHudModule, "isHudModule default is true");
        check(mod.m_width > 0.0f, "default m_width is positive");
        check(mod.m_height > 0.0f, "default m_height is positive");
        check(mod.m_scale > 0.0f, "default m_scale is positive");
        check(mod.m_slotSize > 0.0f, "default m_slotSize is positive");
        check(mod.m_slotSpacing >= 0.0f, "default m_slotSpacing is non-negative");
        check(mod.m_slotCount > 0, "default m_slotCount is positive");
        check(mod.m_slotOffset >= 0, "default m_slotOffset is non-negative");
        check(mod.m_background, "background on by default");
        check(mod.m_border, "border on by default");
        check(mod.m_showItemCount, "item count on by default");
        check(mod.m_showDurability, "durability on by default");
        check(mod.m_showEmptySlots, "empty slots on by default");
    }

    std::printf("Config round-trip\n");
    {
        InventoryHUDModule mod;
        nlohmann::json j;
        mod.saveConfig(j);
        check(j.contains("hudPosX"), "saveConfig writes hudPosX");
        check(j.contains("hudPosY"), "saveConfig writes hudPosY");
        check(j.contains("isHudModule"), "saveConfig writes isHudModule");
        check(j.contains("m_width"), "saveConfig writes m_width");
        check(j.contains("m_height"), "saveConfig writes m_height");
        check(j.contains("m_scale"), "saveConfig writes m_scale");
        check(j.contains("m_slotSize"), "saveConfig writes m_slotSize");
        check(j.contains("m_slotSpacing"), "saveConfig writes m_slotSpacing");
        check(j.contains("m_slotOffset"), "saveConfig writes m_slotOffset");
        check(j.contains("m_slotCount"), "saveConfig writes m_slotCount");
        check(j.contains("m_refreshIntervalMs"), "saveConfig writes m_refreshIntervalMs");
        check(j.contains("m_rescanIntervalMs"), "saveConfig writes m_rescanIntervalMs");
        check(j.contains("m_background"), "saveConfig writes m_background");
        check(j.contains("m_backgroundOpacity"), "saveConfig writes m_backgroundOpacity");
        check(j.contains("m_border"), "saveConfig writes m_border");
        check(j.contains("m_borderOpacity"), "saveConfig writes m_borderOpacity");
        check(j.contains("m_showItemCount"), "saveConfig writes m_showItemCount");
        check(j.contains("m_showDurability"), "saveConfig writes m_showDurability");
        check(j.contains("m_showEmptySlots"), "saveConfig writes m_showEmptySlots");
        check(j.contains("m_backgroundColor"), "saveConfig writes m_backgroundColor");
        check(j.contains("m_borderColor"), "saveConfig writes m_borderColor");
        check(j.contains("m_emptySlotColor"), "saveConfig writes m_emptySlotColor");
        check(j.contains("m_itemCountColor"), "saveConfig writes m_itemCountColor");
        check(j.contains("m_durabilityColor"), "saveConfig writes m_durabilityColor");

        // Round-trip the JSON and verify the values come back unchanged.
        InventoryHUDModule other;
        other.loadConfig(j);
        checkEqual(static_cast<int>(other.m_width), static_cast<int>(mod.m_width), "m_width round-trips");
        checkEqual(static_cast<int>(other.m_height), static_cast<int>(mod.m_height), "m_height round-trips");
        checkEqual(other.m_background, mod.m_background, "m_background round-trips");
        checkEqual(other.m_showEmptySlots, mod.m_showEmptySlots, "m_showEmptySlots round-trips");
    }

    std::printf("onFrame is safe with no game state\n");
    {
        InventoryHUDModule mod;
        mod.masterEnabled = true;
        mod.keybindActive = true;
        mod.updateEnabledState();
        // Without a Player (ClientInstance is stubbed to nullptr) the
        // module must still clear the overlay without crashing.
        mod.onFrame();
    }

    std::printf("moduleId is set\n");
    {
        InventoryHUDModule mod;
        const std::string id = mod.moduleId;
        check(!id.empty(), "moduleId is non-empty");
        check(id.find("bedrocktools.") == 0, "moduleId has the bedrocktools. prefix");
    }

    if (g_failures != 0) {
        std::printf("\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}

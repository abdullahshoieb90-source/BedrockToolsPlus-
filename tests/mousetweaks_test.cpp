// Host tests for the Mouse Tweaks module.
//
// The module's inventory movement is delegated to the genuine Minecraft native
// container functions, so on the host those are replaced by stubs (they return
// 0 / nullptr and never touch memory). What we *can* verify without the game is
// the drag/scroll/quick-move state machine and the hard rule that a slot is
// never handled twice during a single drag.
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src -I include -I tests/fakejson
//         tests/mousetweaks_test.cpp -o /tmp/mousetweaks_test
//     /tmp/mousetweaks_test

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include "core/GameHooks.hpp"

// Stub the native container interaction entry points. They are declared in
// GameHooks.hpp and called by the module; without the game they are inert.
namespace bedrocktools::core::gamehooks {
std::uint32_t selectContainerSlot(void*, const std::string&, int) { return 0; }
std::uint32_t autoPlaceContainerSlot(void*, const std::string&, int) { return 0; }
const void* getContainerSlotItemStack(void*, const std::string&, int) { return nullptr; }
}

// Stub the signature resolver used to fetch the native accessors.
namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId) { return 0; }
bool resolveAll(std::string_view) { return false; }
void clear() {}
}

// The EventBus singleton (src/core/events/Events.cpp is header-light).
#include "core/events/Events.cpp"

#include "modules/misc/mousetweaks.hpp"
#include "modules/misc/mousetweaks.cpp"

#include <cstdio>
#include <string>

namespace {

int failures = 0;
void check(bool condition, const char* message) {
    std::printf("  %s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) ++failures;
}

void publishContainerOpen(void* controller) {
    bedrocktools::events::ScreenStateEvent open{bedrocktools::events::ScreenKind::Container,
                                                bedrocktools::events::ScreenPhase::Opened,
                                                controller};
    bedrocktools::events::bus().publish(open);
}

void publishContainerClose(void* controller) {
    bedrocktools::events::ScreenStateEvent close{bedrocktools::events::ScreenKind::Container,
                                                 bedrocktools::events::ScreenPhase::Closed,
                                                 controller};
    bedrocktools::events::bus().publish(close);
}

// Publishes a slot selection and returns whether the module cancelled it.
bool publishSlot(void* controller, const std::string& collection, int slot) {
    bedrocktools::events::ContainerSlotSelectedEvent event{controller, collection, slot};
    bedrocktools::events::bus().publish(event);
    return event.cancelled();
}

void testDedupDuringLeftDrag() {
    std::printf("== left-button drag dedup ==\n");
    MouseTweaksModule module;
    module.onInit();
    module.setMasterEnabled(true);

    void* controller = reinterpret_cast<void*>(0x1234);
    publishContainerOpen(controller);

    // Press left button over the container.
    check(!module.onMouseEvent(1, true), "mouse down not consumed");

    // First reported slot starts the drag and falls back to the native action.
    check(!publishSlot(controller, "inventory", 0), "first slot handled once (not cancelled)");

    // Same slot again on the same drag -> suppressed.
    check(publishSlot(controller, "inventory", 0), "repeat of same slot cancelled");

    // A new slot is handled once.
    check(!publishSlot(controller, "inventory", 1), "second distinct slot handled once");

    // Repeat -> cancelled.
    check(publishSlot(controller, "inventory", 1), "repeat of second slot cancelled");

    // Release the button, drag ends.
    check(!module.onMouseEvent(1, false), "mouse up not consumed");

    // After the drag a plain click is not suppressed.
    check(!publishSlot(controller, "inventory", 2), "post-drag click not cancelled");

    publishContainerClose(controller);
    bedrocktools::events::bus().clear();
}

void testRightButtonDistribution() {
    std::printf("== right-button drag distribution ==\n");
    MouseTweaksModule module;
    module.onInit();
    module.setMasterEnabled(true);

    void* controller = reinterpret_cast<void*>(0x5678);
    publishContainerOpen(controller);

    check(!module.onMouseEvent(2, true), "right mouse down not consumed");
    check(!publishSlot(controller, "chest", 3), "rmb first matching slot let through");
    check(publishSlot(controller, "chest", 3), "rmb repeat slot cancelled");
    check(!publishSlot(controller, "chest", 4), "rmb second matching slot let through");
    check(publishSlot(controller, "chest", 4), "rmb repeat second slot cancelled");

    check(!module.onMouseEvent(2, false), "right mouse up not consumed");
    publishContainerClose(controller);
    bedrocktools::events::bus().clear();
}

void testLeftDragDisabled() {
    std::printf("== left drag disabled -> no interference ==\n");
    MouseTweaksModule module;
    module.onInit();

    nlohmann::json config;
    config["m_lmbDrag"] = false;
    module.loadConfig(config);

    void* controller = reinterpret_cast<void*>(0x999);
    publishContainerOpen(controller);

    check(!module.onMouseEvent(1, true), "mouse down not consumed");
    // With the feature off the module must not cancel anything.
    check(!publishSlot(controller, "inventory", 0), "feature off -> slot not cancelled");
    check(!publishSlot(controller, "inventory", 0), "feature off -> repeat not cancelled");
    check(!module.onMouseEvent(1, false), "mouse up not consumed");
    publishContainerClose(controller);
    bedrocktools::events::bus().clear();
}

void testQuickMoveViaKey() {
    std::printf("== quick move keybind ==\n");
    MouseTweaksModule module;
    module.onInit();
    module.setMasterEnabled(true);

    void* controller = reinterpret_cast<void*>(0xab);
    publishContainerOpen(controller);

    nlohmann::json config;
    config["m_quickMove"] = true;
    config["m_mouseTweaksEnabled"] = true;
    config["m_quickMoveKeybind"] = 69;
    module.loadConfig(config);

    // Hover a slot by letting the game report a selection.
    check(!publishSlot(controller, "inventory", 5), "hover slot handled once");

    // The key fires quick move; it must not consume the key event (return false).
    check(!module.onKeyEvent(69, true), "quick move key not consumed");

    module.onKeyEvent(69, false);
    publishContainerClose(controller);
    bedrocktools::events::bus().clear();
}

void testTouchDragBehavesLikeLeft() {
    std::printf("== touch drag behaves like left ==\n");
    MouseTweaksModule module;
    module.onInit();
    module.setMasterEnabled(true);

    void* controller = reinterpret_cast<void*>(0xcd);
    publishContainerOpen(controller);

    check(!module.onTouchEvent(10.0f, 20.0f, true), "touch down not consumed");
    check(!publishSlot(controller, "inventory", 0), "touch first slot let through");
    check(publishSlot(controller, "inventory", 0), "touch repeat slot cancelled");
    check(!module.onTouchEvent(12.0f, 20.0f, false), "touch up not consumed");
    publishContainerClose(controller);
    bedrocktools::events::bus().clear();
}

void testModuleDisabledNoInterference() {
    std::printf("== module disabled -> no interference ==\n");
    MouseTweaksModule module;
    module.onInit(); // leave the module disabled (masterEnabled off)

    void* controller = reinterpret_cast<void*>(0xef);
    publishContainerOpen(controller);

    // Even while a button is held and slots are selected, a disabled module must
    // never consume the mouse/touch events nor cancel any slot selection.
    check(!module.onMouseEvent(1, true), "disabled mouse down not consumed");
    check(!publishSlot(controller, "inventory", 0), "disabled: slot not cancelled");
    check(!publishSlot(controller, "inventory", 0), "disabled: repeat not cancelled");
    check(!module.onMouseEvent(1, false), "disabled mouse up not consumed");

    // "Enable Mouse Tweaks" turned off but module master on is also a no-op.
    module.setMasterEnabled(true);
    nlohmann::json sub; 
    sub["m_mouseTweaksEnabled"] = false;
    module.loadConfig(sub);
    check(!module.onMouseEvent(1, true), "sub-toggle off: mouse down not consumed");
    check(!publishSlot(controller, "inventory", 1), "sub-toggle off: slot not cancelled");
    check(!publishSlot(controller, "inventory", 1), "sub-toggle off: repeat not cancelled");
    check(!module.onMouseEvent(1, false), "sub-toggle off: mouse up not consumed");

    publishContainerClose(controller);
    bedrocktools::events::bus().clear();
}

} // namespace

int main() {
    testDedupDuringLeftDrag();
    testRightButtonDistribution();
    testLeftDragDisabled();
    testQuickMoveViaKey();
    testTouchDragBehavesLikeLeft();
    testModuleDisabledNoInterference();

    std::printf("\n");
    if (failures == 0) {
        std::printf("all mousetweaks checks passed\n");
        return 0;
    }
    std::printf("%d mousetweaks check(s) FAILED\n", failures);
    return 1;
}

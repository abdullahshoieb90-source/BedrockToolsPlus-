#pragma once

#include "../Module.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Mouse Tweaks for Minecraft Bedrock.
//
// Recreates the InventoryTweaks-style behaviour from Minecraft Java using the
// *native* Minecraft Bedrock inventory/container interaction functions, never
// writing to the inventory memory directly.
//
// How it maps onto Bedrock
// ------------------------
// Bedrock has no Java-style "hover + mouse cursor" inventory screen. Item
// movement is driven by the ContainerScreenController, and the mod already
// hooks the native OnContainerSlotSelected invocation (see GameHooks.cpp) and
// publishes a cancellable ContainerSlotSelectedEvent for every slot the game
// processes. Mouse Tweaks subscribes to that stream: the event is the
// authoritative "the game is acting on slot (collection, index)" signal, so the
// module does not need to invent a coordinate -> slot mapping (which would
// require fake offsets).
//
// The module therefore:
//   * observes touch / mouse press and release to know *when* a drag happens,
//   * observes the container slot selections the game reports to know *which*
//     slot the user is currently over,
//   * and, for each slot, chooses whether to fall back to the native action
//     (letting Minecraft move/merge the items) or to suppress the repeat of a
//     slot it already handled in the same drag.
//
// This satisfies the two hard requirements of the request: nothing is written
// to the inventory through setItem (all movement is done by Minecraft's own
// container functions), and no slot is processed twice during one drag.

class MouseTweaksModule final : public Module {
public:
    MouseTweaksModule();
    ~MouseTweaksModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

    // Input observation. Both return false so the vanilla UI keeps receiving
    // the event (returning true would consume it and break the container).
    bool onMouseEvent(int button, bool isDown) override;
    bool onTouchEvent(float x, float y, bool isDown) override;

    // Quick Move / Scroll keybinds.
    bool onKeyEvent(int key, bool isDown) override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    enum class ScrollMode { Normal = 0, Reverse = 1 };

    // Which mouse/touch button owns the current drag.
    struct DragKind {
        enum Value { None = 0, Left, Right, Touch };
        Value value = None;
        bool operator==(Value v) const { return value == v; }
        bool operator!=(Value v) const { return value != v; }
    };

    // Identifies one slot in one container screen. Used to dedup a drag so the
    // same slot is never handled twice within a single drag.
    struct SlotKey {
        void* controller = nullptr;
        std::string collection;
        int slot = 0;

        bool operator==(const SlotKey& o) const {
            return controller == o.controller && collection == o.collection && slot == o.slot;
        }
    };

    // ---- config ------------------------------------------------------------
    bool m_mouseTweaksEnabled = true;      // Enable Mouse Tweaks (on top of the module toggle)
    bool m_rmbDrag = true;      // Enable RMB Drag
    bool m_lmbDrag = true;      // Enable LMB Drag
    bool m_scroll = true;       // Enable Scroll
    bool m_quickMove = true;    // Enable Quick Move
    bool m_sameItemOnly = true; // RMB spread only onto slots with the same item
    int m_dragDelay = 0;        // Drag Delay (ms) between two handled slots
    ScrollMode m_scrollMode = ScrollMode::Normal; // Scroll Mode
    // Android key codes; the launcher menu exposes these as keybind pickers.
    // Defaults: Shift = quick move, PageUp/PageDown = scroll.
    int m_quickMoveKeybind = 59;
    int m_scrollUpKeybind = 92;
    int m_scrollDownKeybind = 93;

    // ---- runtime tracking ---------------------------------------------------
    std::atomic_int m_containerDepth{0};
    void* m_activeController = nullptr;

    DragKind m_dragKind{};
    bool m_dragActive = false;
    bool m_pressed = false;      // a button/finger is currently held down
    bool m_mouseDown = false;
    bool m_mouseUp = true;
    bool m_mouseMove = false;
    std::uint32_t m_button = 0;  // last pressed Android button (1 = left, 2 = right)

    // Drag tracking.
    SlotKey m_dragStartSlot;
    std::string m_referenceItemName; // carried item type captured at drag start
    SlotKey m_hoveredSlot;           // last slot the game reported
    float m_pointerX = 0.0f;
    float m_pointerY = 0.0f;
    int m_mouseWheel = 0;            // reserved: pl::input on Bedrock has no wheel

    // Slots handled during the current drag (bounded; cleared on drag start).
    std::vector<SlotKey> m_handledSlots;
    std::uint32_t m_lastDragStepMs = 0;
    std::mutex m_mutex;

    // True only while the module is enabled *and* the "Enable Mouse Tweaks"
    // sub-toggle is on — i.e. only then may the module interfere at all.
    bool active() const { return enabled && m_mouseTweaksEnabled; }

    // Processes one container slot selection reported by the game while a drag
    // is active. Returns true when the event should be cancelled (already
    // handled this slot / outside the drag).
    bool handleSlotSelection(const SlotKey& slot);

    // Same item type as the drag-start carried item?
    bool slotMatchesReference(const SlotKey& slot);
    // Item identifier of a slot, or empty when it cannot be read safely.
    std::string readItemName(const SlotKey& slot);

    void beginDrag(DragKind::Value kind, const SlotKey& startSlot);
    void endDrag();
    SlotKey slotFrom(void* controller, const std::string& collection, int slot) const;
    bool alreadyHandled(const SlotKey& slot) const;
    void markHandled(const SlotKey& slot);
    void clearHandled();

    // Native quick-move / scroll actions.
    void quickMove(const SlotKey& slot);
    void scrollStep(const SlotKey& slot, bool up);
};

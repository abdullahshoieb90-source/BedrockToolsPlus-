#include "mousetweaks.hpp"

#include "core/GameHooks.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace {

using namespace bedrocktools::sdk::offsets;

template <class T>
T readField(const void* object, std::size_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const std::byte*>(object) + offset);
}

// Offsets used to walk an ItemStack only for the "same item type" reference
// comparison. No slot content is ever written back.
constexpr std::size_t ItemStackBaseItemOffset = ShulkerPreview::ItemStackBaseItem;        // 0x8
constexpr std::size_t SharedCounterPointerOffset = ShulkerPreview::SharedCounterPointer;  // 0x0

std::uint32_t nowMs() {
    using namespace std::chrono;
    return static_cast<std::uint32_t>(duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count());
}

} // namespace

MouseTweaksModule::MouseTweaksModule()
    : Module("Mouse Tweaks",
             "Adds Java-style Mouse Tweaks drag/scroll/quick-move behaviour to "
             "Bedrock inventory and container screens, using Minecraft's own "
             "container interaction functions.") {
}

MouseTweaksModule::~MouseTweaksModule() = default;

void MouseTweaksModule::onInit() {
    using namespace bedrocktools::events;

    // Track container open/close so the module only reacts while a container
    // (player inventory, chest, shulker, crafting, furnace, ...) is visible.
    bus().subscribe<ScreenStateEvent>([this](ScreenStateEvent& event) {
        if (event.screen != ScreenKind::Container) return;
        if (event.phase == ScreenPhase::Opened) {
            m_containerDepth.fetch_add(1, std::memory_order_acq_rel);
            m_activeController = event.controller;
        } else {
            int depth = m_containerDepth.load(std::memory_order_acquire);
            while (depth > 0 &&
                   !m_containerDepth.compare_exchange_weak(depth, depth - 1,
                                                           std::memory_order_acq_rel)) {
            }
            if (depth == 1) {
                m_activeController = nullptr;
                std::lock_guard lock(m_mutex);
                clearHandled();
                m_dragActive = false;
                m_pressed = false;
            }
        }
    });

    // Observe every slot the game processes. On the *before* selection we decide
    // whether to fall back to the native action or to suppress a repeat. When the
    // module is not active we never look at these events, so the vanilla UI is
    // left completely untouched.
    bus().subscribe<ContainerSlotSelectedEvent>([this](ContainerSlotSelectedEvent& event) {
        if (!active() || m_containerDepth.load(std::memory_order_acquire) <= 0) return;
        if (event.afterSelection) return; // only the cancellable before-phase

        std::lock_guard lock(m_mutex);
        const auto slot = slotFrom(event.controller, event.collectionName, event.index);
        m_hoveredSlot = slot;
        if (handleSlotSelection(slot)) {
            event.cancel(true);
        }
    });
}

void MouseTweaksModule::onEnable() {
    std::lock_guard lock(m_mutex);
    clearHandled();
    m_dragActive = false;
    m_pressed = false;
}

void MouseTweaksModule::onDisable() {
    std::lock_guard lock(m_mutex);
    clearHandled();
    m_dragActive = false;
    m_pressed = false;
}

void MouseTweaksModule::onFrame() {
    // Drag Delay throttling is implemented at the moment a slot is handled; the
    // frame hook is a no-op. It exists so the module follows the Module contract
    // and can host time-based bookkeeping later without changing the interface.
}

bool MouseTweaksModule::onMouseEvent(int button, bool isDown) {
    if (!active()) return false; // do not interfere (or consume) while off
    std::lock_guard lock(m_mutex);
    m_mouseDown = isDown;
    m_mouseUp = !isDown;
    m_mouseMove = m_dragActive; // remains true while a drag is in progress
    m_button = static_cast<std::uint32_t>(button);

    // Android MotionEvent button constants: 1 = left/primary, 2 = right/secondary,
    // 4 = middle/tertiary. Anything else is treated as a non-drag button.
    if (isDown) {
        m_pressed = true;
        m_dragKind.value =
            button == 2 ? DragKind::Right : (button == 1 ? DragKind::Left : DragKind::None);
    } else {
        m_pressed = false;
        endDrag();
    }
    return false; // never consume: the vanilla UI must keep receiving events
}

bool MouseTweaksModule::onTouchEvent(float x, float y, bool isDown) {
    if (!active()) return false; // do not interfere while off
    std::lock_guard lock(m_mutex);
    m_pointerX = x;
    m_pointerY = y;
    if (isDown) {
        m_pressed = true;
        m_dragKind.value = DragKind::Touch;
    } else {
        m_pressed = false;
        endDrag();
    }
    return false; // never consume
}

bool MouseTweaksModule::onKeyEvent(int key, bool isDown) {
    if (!isDown || !active()) return false;
    if (m_containerDepth.load(std::memory_order_acquire) <= 0 || !m_activeController) return false;

    std::lock_guard lock(m_mutex);

    const bool enableQuick = m_quickMove;
    const bool enableScroll = m_scroll;

    if (enableQuick && m_quickMoveKeybind != 0 && key == m_quickMoveKeybind) {
        quickMove(m_hoveredSlot);
        return false;
    }
    if (enableScroll && m_scrollUpKeybind != 0 && key == m_scrollUpKeybind) {
        scrollStep(m_hoveredSlot, true);
        return false;
    }
    if (enableScroll && m_scrollDownKeybind != 0 && key == m_scrollDownKeybind) {
        scrollStep(m_hoveredSlot, false);
        return false;
    }
    return false;
}

MouseTweaksModule::SlotKey MouseTweaksModule::slotFrom(void* controller,
                                                       const std::string& collection,
                                                       int slot) const {
    SlotKey key;
    key.controller = controller;
    key.collection = collection;
    key.slot = slot;
    return key;
}

void MouseTweaksModule::beginDrag(DragKind::Value kind, const SlotKey& startSlot) {
    m_dragKind.value = kind;
    m_dragActive = true;
    m_dragStartSlot = startSlot;
    m_referenceItemName.clear();
    if (m_sameItemOnly) m_referenceItemName = readItemName(startSlot);
    clearHandled();
    m_lastDragStepMs = nowMs();
}

void MouseTweaksModule::endDrag() {
    if (!m_dragActive) return;
    m_dragActive = false;
    m_dragKind.value = DragKind::None;
    clearHandled();
}

bool MouseTweaksModule::alreadyHandled(const SlotKey& slot) const {
    return std::find(m_handledSlots.begin(), m_handledSlots.end(), slot) != m_handledSlots.end();
}

void MouseTweaksModule::markHandled(const SlotKey& slot) {
    // Bounded approximation of the processed-slot set; a drag over a large
    // container can never exceed the number of real slots in it, so keep this
    // comfortably above any realistic count.
    constexpr std::size_t MaxHandled = 256;
    if (m_handledSlots.size() >= MaxHandled) m_handledSlots.erase(m_handledSlots.begin());
    m_handledSlots.push_back(slot);
}

void MouseTweaksModule::clearHandled() {
    m_handledSlots.clear();
}

bool MouseTweaksModule::slotMatchesReference(const SlotKey& slot) {
    if (m_referenceItemName.empty()) return true; // unknown -> do not over-filter
    const std::string name = readItemName(slot);
    if (name.empty()) return false; // different/empty slot
    return name == m_referenceItemName;
}

std::string MouseTweaksModule::readItemName(const SlotKey& slot) {
    if (!slot.controller) return {};
    const void* stack = bedrocktools::core::gamehooks::getContainerSlotItemStack(
        slot.controller, slot.collection, slot.slot);
    if (!stack) return {};
    const void* counter = readField<const void*>(stack, ItemStackBaseItemOffset);
    if (!counter) return {};
    const void* item = readField<const void*>(counter, SharedCounterPointerOffset);
    if (!item) return {};
    // The Item pointer alone does not carry the identifier; the ItemStack's
    // description id is fetched through the native accessor. When it is not
    // resolved (e.g. host tests) the reference stays unknown and matching is
    // skipped rather than guessing.
    using RawNameFn = const std::string& (*)(const void*);
    const auto target = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::ItemStackBaseGetRawNameId);
    if (!target) return {};
    return reinterpret_cast<RawNameFn>(target)(stack);
}

bool MouseTweaksModule::handleSlotSelection(const SlotKey& slot) {
    // If a button/finger is held but the drag has not formally begun, the first
    // slot the game reports starts it (this is the slot that was pressed).
    if (m_pressed && !m_dragActive) {
        beginDrag(m_dragKind.value, slot);
    }

    if (!m_dragActive) return false; // plain click / not a Mouse Tweaks drag

    // Enforce the "never apply the same operation twice to one slot during a
    // single drag" rule regardless of the feature that is about to run.
    if (alreadyHandled(slot)) return true;

    const bool rmb = m_dragKind == DragKind::Right;
    const bool lmb = m_dragKind == DragKind::Left;
    const bool touch = m_dragKind == DragKind::Touch;
    const bool leftBehavior = lmb || touch;

    if (rmb) {
        if (!(m_mouseTweaksEnabled && m_rmbDrag)) return false; // feature off -> let the game handle
        // Only spread onto slots that already hold the same item type; suppress
        // the native (swap) action on every other slot so a stray drag never
        // swaps items by accident.
        if (m_sameItemOnly && !slotMatchesReference(slot)) return true;

        // Throttle consecutive placements by Drag Delay.
        if (m_dragDelay > 0 && nowMs() - m_lastDragStepMs < static_cast<std::uint32_t>(m_dragDelay)) {
            return true;
        }
        m_mouseMove = true; // reaching a new slot implies the pointer moved
        markHandled(slot);
        m_lastDragStepMs = nowMs();
        return false; // let the native OnContainerSlotSelected merge one step
    }

    if (leftBehavior) {
        if (!(m_mouseTweaksEnabled && m_lmbDrag)) return false; // feature off -> let the game handle
        if (m_dragDelay > 0 && nowMs() - m_lastDragStepMs < static_cast<std::uint32_t>(m_dragDelay)) {
            return true;
        }
        m_mouseMove = true; // reaching a new slot implies the pointer moved
        markHandled(slot);
        m_lastDragStepMs = nowMs();
        return false; // let the native pick up / place / swap once
    }

    return false;
}

void MouseTweaksModule::quickMove(const SlotKey& slot) {
    // Quick Move = shift-click: call Minecraft's own auto-place, which moves an
    // entire stack between the container and the player inventory without us
    // writing to the inventory memory.
    if (!slot.controller) return;
    bedrocktools::core::gamehooks::autoPlaceContainerSlot(
        slot.controller, slot.collection, slot.slot);
}

void MouseTweaksModule::scrollStep(const SlotKey& slot, bool up) {
    // Bedrock's input layer (pl::input) exposes no raw wheel event, so Scroll is
    // bound to a key. It performs a native one-step quick move (auto-place) on
    // the hovered slot; the actual move direction is decided by Minecraft's own
    // container logic based on which side of the container the slot is on. The
    // Scroll Mode setting swaps which key represents "inward" (up vs down) but
    // the native function is always the same — we never touch the inventory.
    (void)up;
    if (!slot.controller) return;
    bedrocktools::core::gamehooks::autoPlaceContainerSlot(
        slot.controller, slot.collection, slot.slot);
}

void MouseTweaksModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    if (j.contains("m_mouseTweaksEnabled")) { try { m_mouseTweaksEnabled = j["m_mouseTweaksEnabled"].get<bool>(); } catch (...) {} }
    if (j.contains("m_rmbDrag")) { try { m_rmbDrag = j["m_rmbDrag"].get<bool>(); } catch (...) {} }
    if (j.contains("m_lmbDrag")) { try { m_lmbDrag = j["m_lmbDrag"].get<bool>(); } catch (...) {} }
    if (j.contains("m_scroll")) { try { m_scroll = j["m_scroll"].get<bool>(); } catch (...) {} }
    if (j.contains("m_quickMove")) { try { m_quickMove = j["m_quickMove"].get<bool>(); } catch (...) {} }
    if (j.contains("m_sameItemOnly")) { try { m_sameItemOnly = j["m_sameItemOnly"].get<bool>(); } catch (...) {} }
    if (j.contains("m_dragDelay")) { try { m_dragDelay = std::max(0, j["m_dragDelay"].get<int>()); } catch (...) {} }

    if (j.contains("m_quickMoveKeybind")) { try { m_quickMoveKeybind = j["m_quickMoveKeybind"].get<int>(); } catch (...) {} }
    if (j.contains("m_scrollUpKeybind")) { try { m_scrollUpKeybind = j["m_scrollUpKeybind"].get<int>(); } catch (...) {} }
    if (j.contains("m_scrollDownKeybind")) { try { m_scrollDownKeybind = j["m_scrollDownKeybind"].get<int>(); } catch (...) {} }

    if (j.contains("m_scrollMode")) {
        const auto& value = j["m_scrollMode"];
        // Radio configs persist as "<index>,<label>..."; the menu reports a
        // plain index on change (see CrosshairModule for the same pattern).
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            const auto comma = text.find(',');
            try {
                const int mode = std::stoi(text.substr(0, comma));
                if (mode == static_cast<int>(ScrollMode::Normal) ||
                    mode == static_cast<int>(ScrollMode::Reverse)) {
                    m_scrollMode = static_cast<ScrollMode>(mode);
                }
            } catch (...) {}
        } else if (value.is_number_integer()) {
            const int mode = value.get<int>();
            if (mode == static_cast<int>(ScrollMode::Normal) ||
                mode == static_cast<int>(ScrollMode::Reverse)) {
                m_scrollMode = static_cast<ScrollMode>(mode);
            }
        }
    }
}

void MouseTweaksModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    j["m_mouseTweaksEnabled"] = m_mouseTweaksEnabled;
    j["m_rmbDrag"] = m_rmbDrag;
    j["m_lmbDrag"] = m_lmbDrag;
    j["m_scroll"] = m_scroll;
    j["m_quickMove"] = m_quickMove;
    j["m_sameItemOnly"] = m_sameItemOnly;
    j["m_dragDelay"] = m_dragDelay;
    j["m_quickMoveKeybind"] = m_quickMoveKeybind;
    j["m_scrollUpKeybind"] = m_scrollUpKeybind;
    j["m_scrollDownKeybind"] = m_scrollDownKeybind;
    j["m_scrollMode"] = std::to_string(static_cast<int>(m_scrollMode)) +
        ",Normal,Reverse";
}

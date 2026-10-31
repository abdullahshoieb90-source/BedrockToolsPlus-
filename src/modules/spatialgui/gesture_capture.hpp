#pragma once

#include "touch_router.hpp"
#include <algorithm>
#include <vector>

namespace bedrocktools::spatialgui {
// One Android MotionEvent can carry multiple pointers, and the launcher
// consumes the WHOLE event when any pointer callback returns true. Therefore
// ownership must be decided at the initial ACTION_DOWN and held through the
// entire gesture. Additional fingers are swallowed while captured, never sent
// to the UI sink. Do not begin capture on ACTION_POINTER_DOWN.
//
// All callback invocations for a MotionEvent must be delivered, including the
// launcher's per-pointer MOVE and CANCEL callbacks. The sink must support
// cancellation. This helper alone cannot inject Bedrock events.
class GestureCapture {
public:
    RouteResult route(int action, int pointerId, Vec2 point, const Camera& camera,
                      const Panel& panel, Vec2 uiSize) {
        constexpr int down=0, up=1, move=2, cancel=3, pointerDown=5, pointerUp=6;
        if (action == down) {
            // A new Android gesture supersedes stale bookkeeping. The caller
            // must deliver abandon()'s cancellation before processing a new
            // DOWN if an earlier stream was lost.
            if (m_active) return {true, {}};
            auto first = m_router.route(down, pointerId, point, camera, panel, uiSize);
            if (!first.consume) return {};
            m_active = true;
            m_primary = pointerId;
            m_contacts = {pointerId};
            return first;
        }
        if (!m_active) return {};
        if (action == pointerDown) {
            if (std::find(m_contacts.begin(), m_contacts.end(), pointerId) == m_contacts.end())
                m_contacts.push_back(pointerId);
            return {true, {}};
        }
        if (action == cancel) {
            // ACTION_CANCEL is delivered once per pointer by this launcher.
            // Cancel the UI once, then consume until all known pointers have
            // received their cancellation callback.
            auto event = m_router.reset();
            erase(pointerId);
            if (m_contacts.empty()) m_active = false;
            return {true, event};
        }
        if (action == move) {
            if (pointerId != m_primary) return {true, {}};
            auto result = m_router.route(move, pointerId, point, camera, panel, uiSize);
            return {true, result.event};
        }
        if (action == pointerUp || action == up) {
            std::optional<RoutedTouch> event;
            if (pointerId == m_primary)
                event = m_router.route(action, pointerId, point, camera, panel, uiSize).event;
            erase(pointerId);
            if (action == up) {
                // ACTION_UP ends the Android gesture even if callbacks were
                // missing; cancel any still-held UI state and clear contacts.
                auto remaining = m_router.reset();
                if (!event) event = remaining;
                m_contacts.clear();
                m_active = false;
            }
            return {true, event};
        }
        return {true, {}};
    }

    // Disable/close: cancel UI input but keep consuming the captured Android
    // gesture until it ends. Do not destroy the instance mid-gesture.
    std::optional<RoutedTouch> cancelUi() { return m_router.reset(); }
    // Use only when the Android stream itself is known to have ended/lost.
    std::optional<RoutedTouch> abandon() {
        auto result = m_router.reset();
        m_contacts.clear();
        m_active = false;
        return result;
    }
    bool captured() const { return m_active; }
private:
    void erase(int id) {
        m_contacts.erase(std::remove(m_contacts.begin(), m_contacts.end(), id), m_contacts.end());
    }
    TouchRouter m_router;
    std::vector<int> m_contacts;
    int m_primary = -1;
    bool m_active = false;
};
}

#pragma once

#include "input_mapping.hpp"
#include <optional>

namespace bedrocktools::spatialgui {
// Pure single-pointer gesture adapter. The caller must deliver a complete
// gesture to an actual Bedrock UI input sink and coordinate event consumption
// across the launcher; this class does not inject input or consume it itself.
enum class TouchPhase { Down, Move, Up, Cancel };
struct RoutedTouch { TouchPhase phase; Vec2 ui; };
struct RouteResult { bool consume = false; std::optional<RoutedTouch> event; };

class TouchRouter {
public:
    // Android actionMasked values, matching the preloader callback. One MOVE
    // and CANCEL may be delivered per pointer. Other fingers are untouched.
    RouteResult route(int action, int pointerId, Vec2 point, const Camera& camera,
                      const Panel& panel, Vec2 uiSize) {
        constexpr int down = 0, up = 1, move = 2, cancel = 3, pointerDown = 5, pointerUp = 6;
        if (!m_pointer) {
            if (action != down && action != pointerDown) return {};
            auto mapped = mapTouch(camera, panel, point, uiSize);
            if (!mapped) return {};
            m_pointer = pointerId;
            m_last = *mapped;
            m_cancelled = false;
            return {true, RoutedTouch{TouchPhase::Down, *mapped}};
        }
        if (*m_pointer != pointerId) return {};
        if (action == cancel) return finish(TouchPhase::Cancel);
        if (action == up || action == pointerUp) {
            if (m_cancelled) { m_pointer.reset(); m_cancelled = false; return {true, {}}; }
            auto mapped = mapTouch(camera, panel, point, uiSize);
            if (!mapped) return finish(TouchPhase::Cancel);
            m_last = *mapped;
            return finish(TouchPhase::Up);
        }
        if (action != move) return {true, {}};
        if (m_cancelled) return {true, {}};
        auto mapped = mapTouch(camera, panel, point, uiSize);
        if (!mapped) {
            // Keep consuming this pointer until up/cancel so a partial gesture
            // cannot leak through to vanilla after the synthetic cancellation.
            m_cancelled = true;
            return {true, RoutedTouch{TouchPhase::Cancel, m_last}};
        }
        m_last = *mapped;
        return {true, RoutedTouch{TouchPhase::Move, *mapped}};
    }

    // Call before closing/disabling the screen, while the UI sink is alive.
    // Returns at most one cancellation for forwarding to that sink.
    std::optional<RoutedTouch> reset() {
        if (!m_pointer) return {};
        auto result = m_cancelled ? std::optional<RoutedTouch>{} : RoutedTouch{TouchPhase::Cancel, m_last};
        m_pointer.reset();
        m_cancelled = false;
        return result;
    }
private:
    RouteResult finish(TouchPhase phase) {
        auto event = m_cancelled ? std::optional<RoutedTouch>{} : RoutedTouch{phase, m_last};
        m_pointer.reset();
        m_cancelled = false;
        return {true, event};
    }
    std::optional<int> m_pointer;
    Vec2 m_last{};
    bool m_cancelled = false;
};
}

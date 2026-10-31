#pragma once

#include <bedrocktools/events/EventBus.hpp>

namespace bedrocktools::core {
// Preserve the pre-existing single-touch module behavior while exposing the
// complete action stream to subscribers. A listener that captures a gesture is
// responsible for managing all pointer IDs and every subsequent action.
template <class LegacyCallback>
bool dispatchTouch(events::EventBus& bus, int action, int pointerId, float x, float y, LegacyCallback&& legacy) {
    events::TouchInputEvent event{action, pointerId, x, y};
    bus.publish(event);
    if (event.cancelled()) return true;
    // Existing legacy modules only receive primary DOWN, UP and CANCEL.
    if (action != 0 && action != 1 && action != 3) return false;
    return legacy(x, y, action == 0);
}
}

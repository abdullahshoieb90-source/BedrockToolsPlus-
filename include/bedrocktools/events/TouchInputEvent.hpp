#pragma once

#include <bedrocktools/events/Event.hpp>

namespace bedrocktools::events {
// Raw Android MotionEvent actionMasked, pointer ID and coordinates as
// delivered by LeviLaunchroid/Preloader. MOVE and CANCEL may be delivered once
// per pointer. Coordinates are in Android view pixels, not Minecraft UI units.
struct TouchInputEvent : Cancellable {
    static constexpr EventType type = EventType::TouchInput;
    TouchInputEvent(int actionValue, int id, float xValue, float yValue)
        : action(actionValue), pointerId(id), x(xValue), y(yValue) {}
    int action;
    int pointerId;
    float x;
    float y;
};
}

#pragma once

#include <string_view>

namespace bedrocktools::launcher {

// Where a launcher overlay button currently sits on screen.
//
// The Hotbar Slots module needs this to paint the item that occupies a slot
// *on top of the slot button itself* (the LeviLauncher "Use item icons from
// hotbar" behaviour) instead of on a separate HUD strip. The launcher owns
// the button views, so the rectangle is read back over JNI from the
// InbuiltOverlayManager -> ExternalButtonOverlay -> WindowManager.LayoutParams
// chain. All values are in screen pixels of the game's decor view.
struct ExternalButtonGeometry {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float screenWidth = 0.0f;
    float screenHeight = 0.0f;
    bool visible = false;

    bool valid() const {
        return width > 0.0f && height > 0.0f && screenWidth > 0.0f && screenHeight > 0.0f;
    }
};

// Reads the current geometry of a registered overlay button. Returns false
// when the launcher classes are unavailable, the button is not shown, or the
// build is not Android.
bool queryExternalButtonGeometry(std::string_view buttonId, ExternalButtonGeometry& out);

} // namespace bedrocktools::launcher

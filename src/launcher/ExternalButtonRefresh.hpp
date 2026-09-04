#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace bedrocktools::launcher {

// The preloader supplies the VM in ModContext during the native module load.
// Keeping it here lets config callbacks call back into the launcher's overlay
// manager without depending on a private preloader header.
void setJavaVm(void* javaVm);

// Returns the JavaVM supplied by the preloader (null until the native module
// has been loaded). Modules that need JNI (e.g. the Hit Sound module's
// android.media.MediaPlayer playback) use this instead of caching the VM in
// their constructor, which runs before load() is called.
void* javaVm();

// The launcher keeps an ExternalButtonOverlay snapshot after it is created.
// Ask it to rebuild the buttons after a module changes one of its button
// definitions (for example, a command label or a comment text).
void refreshExternalButtonsForModule(std::string_view moduleId);

// On-screen geometry of one launcher overlay button, in physical screen
// pixels (the values View.getLocationOnScreen/getWidth/getHeight report).
// The launcher hosts its overlay buttons in a fullscreen window, so these
// pixels coincide with the HUD surface units the native draw commands use.
struct ButtonGeometry {
    std::string buttonId;
    float x = 0.0f;      // left edge of the button view
    float y = 0.0f;      // top edge of the button view
    float width = 0.0f;  // view width
    float height = 0.0f; // view height
};

// Reads the on-screen geometry of every overlay button owned by the given
// module from InbuiltOverlayManager.externalButtonOverlayMap. Buttons whose
// overlay (or view) is missing, degenerate, or not currently visible are
// skipped, so callers must treat a missing entry as "fall back to whatever
// the module would draw without button geometry". Returns an empty vector
// when the VM is unavailable or the launcher classes cannot be found.
//
// Like the refresh above this may run on any thread: a thread the JVM does
// not know about is attached for the duration of the query and detached
// afterwards. The query walks several Java objects per button, so callers
// that need the geometry every frame must throttle it (a few Hz is plenty -
// buttons only move while the user drags them in the button editor).
std::vector<ButtonGeometry> queryButtonGeometry(std::string_view moduleId);

} // namespace bedrocktools::launcher

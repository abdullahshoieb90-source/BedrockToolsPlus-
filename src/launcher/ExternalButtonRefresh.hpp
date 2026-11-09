#pragma once

#include <string_view>

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

// Called when the launcher refuses a module definition. A rejected module does
// not show up in the menu at all, and the launcher's own error only goes to
// logcat, so this logs the id through the native logger and counts failures
// (see moduleRegistrationFailureCount()). Best effort: it never throws and
// never blocks registration of the remaining modules.
void reportModuleRegistrationFailure(std::string_view moduleId);

// Number of module definitions the launcher refused since startup; 0 means the
// whole menu was registered.
int moduleRegistrationFailureCount();

} // namespace bedrocktools::launcher

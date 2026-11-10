#pragma once

#include <cstddef>

namespace bedrocktools::core::gamehooks {

bool install();
void uninstall();
void* clientInstance();

// True when the Actor::normalTick detour (the LocalPlayerTickEvent source) was
// actually installed. When it is false the runtime drives the same event from
// the client-instance update hook instead, so modules keep ticking.
bool playerTickHookInstalled();

// Bitmask of the hooks that installed successfully, and how many were
// attempted (see hookReport()). Used by the menu to show which game hooks this
// build of Minecraft accepted, so a version mismatch is visible instead of
// looking like "the module does nothing".
std::size_t installedHookCount();
std::size_t attemptedHookCount();

}

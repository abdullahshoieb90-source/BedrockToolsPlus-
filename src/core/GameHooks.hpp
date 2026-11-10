#pragma once

#include <cstddef>
#include <cstdint>

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
// Number of times the Actor::normalTick detour has actually run. A hook that
// installs but never fires means the signature matched a function this build of
// Minecraft does not call, which must not be mistaken for a working tick.
std::uint64_t actorTickCount();

// Whether LocalPlayerTickEvent is currently being driven by the fallback
// (client-instance update) source because the actor hook produced no ticks.
bool fallbackTickSourceActive();
void setFallbackTickSourceActive(bool active);

std::size_t installedHookCount();
std::size_t attemptedHookCount();

}

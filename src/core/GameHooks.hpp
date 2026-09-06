#pragma once

#include <cstdint>
#include <string>

namespace bedrocktools::core::gamehooks {

bool install();
void uninstall();
void* clientInstance();

// Native container-screen interaction helpers exposed to modules.
//
// BedrockToolsPlus already hooks ContainerScreenController::OnContainerSlotSelected
// and knows the trampoline to the original function. Modules that need to drive
// inventory movement (e.g. the Mouse Tweaks module) must go through these wrappers
// instead of calling the symbol directly, otherwise they would re-enter the hook
// detour and recurse. All three wrappers resolve / call the genuine Minecraft
// functions; none of them touch the inventory memory directly.

// Calls the original ContainerScreenController::OnContainerSlotSelected trampoline
// (the exact function Minecraft itself invokes when a slot in a container UI is
// clicked). Returns 0 when the hook or original is unavailable.
std::uint32_t selectContainerSlot(void* controller, const std::string& collectionName, int slot);

// Calls ContainerScreenController::_handleAutoPlace (quick move / shift-click),
// the native function that moves a stack between the container and the player
// inventory. Returns 0 when the signature could not be resolved.
std::uint32_t autoPlaceContainerSlot(void* controller, const std::string& collectionName, int slot);

// Calls ContainerScreenController::getItemStack and returns the ItemStack of a
// collection/slot, or nullptr when the signature could not be resolved. The
// returned pointer may be null for an empty slot.
const void* getContainerSlotItemStack(void* controller, const std::string& collectionName, int slot);

}

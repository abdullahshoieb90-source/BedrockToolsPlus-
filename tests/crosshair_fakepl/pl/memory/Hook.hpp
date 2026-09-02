#pragma once
// Minimal host-side types for tests/crosshair_test.cpp.
//
// crosshair_test defines its own pl::memory::hook / unhook stubs (so it can
// capture the original cursor-call count), therefore this header intentionally
// declares only the API types and does NOT define those functions.
namespace pl::memory {
using FuncPtr = void*;
enum class HookPriority : int {
    Highest = 0,
    High = 100,
    Normal = 200,
    Low = 300,
    Lowest = 400,
};
}

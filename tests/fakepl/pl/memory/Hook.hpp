#pragma once
#include <cstdint>

namespace pl::memory {
// Host-side fake of the preloader hook API.
//
// The hook is reported as installed (return 0) so modules take their hooked
// code path, but the trampoline is handed back as nullptr: the "target"
// addresses tests pass in are made-up constants, so a module that calls the
// original through the returned pointer would jump into nowhere. Production
// code always null-checks the trampoline before calling it, which is exactly
// the behaviour the tests want to exercise.
inline int hook(void* target, void* detour, void** original) {
    if (original) *original = nullptr;
    (void)target;
    (void)detour;
    return 0;
}
inline int unhook(void* target, void* detour) {
    (void)target; (void)detour;
    return 0;
}
}

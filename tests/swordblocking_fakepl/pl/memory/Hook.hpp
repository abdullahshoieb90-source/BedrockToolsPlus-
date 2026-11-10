#pragma once
// Host-side fake of the preloader hook API for tests/swordblocking_test.cpp.
//
// The hook is reported as installed (return 0) so the module takes its hooked
// code path, but the trampoline is handed back as nullptr: the "target"
// addresses the module resolves are made-up constants, so a detour that called
// the original through that pointer would jump into nowhere. Production code
// always null-checks the trampoline, which is exactly what the test drives.
namespace pl::memory {
inline int hook(void* target, void* detour, void** original) {
    (void)target;
    (void)detour;
    if (original) *original = nullptr;
    return 0;
}
inline int unhook(void* target, void* detour) {
    (void)target;
    (void)detour;
    return 0;
}
}

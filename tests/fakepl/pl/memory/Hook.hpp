#pragma once
#include <cstdint>

namespace pl::memory {
inline int hook(void* target, void* detour, void** original) {
    if (original) *original = target;
    (void)detour;
    return 0;
}
inline int unhook(void* target, void* detour) {
    (void)target; (void)detour;
    return 0;
}
}

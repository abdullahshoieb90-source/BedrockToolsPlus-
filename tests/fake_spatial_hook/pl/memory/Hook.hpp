#pragma once
namespace pl::memory {
inline bool failTrampoline = false;
inline int unhookCalls = 0;
inline void stubOriginal(void*, void*) {}
inline int hook(void*, void*, void** original) {
    if (original) *original = failTrampoline ? nullptr : reinterpret_cast<void*>(&stubOriginal);
    return 0;
}
inline int unhook(void*, void*) {++unhookCalls; return 0;}
}

#pragma once

#include <vector>

namespace pl::memory {

// Host-side fake of the preloader hook API that also remembers what was
// installed, so a test can call a module's detour after onInit() registered
// it. The trampoline is handed back as nullptr: the "target" addresses used by
// tests are made-up constants, so calling the original would jump nowhere.
struct HookRecord {
    void* target;
    void* detour;
    void** original;
};

inline std::vector<HookRecord>& hookRecords() {
    static std::vector<HookRecord> records;
    return records;
}

inline void* detourForTarget(void* target) {
    for (const auto& record : hookRecords()) {
        if (record.target == target) return record.detour;
    }
    return nullptr;
}

inline int hook(void* target, void* detour, void** original) {
    if (original) *original = nullptr;
    hookRecords().push_back({target, detour, original});
    return 0;
}

inline int unhook(void* target, void* detour) {
    (void)target;
    (void)detour;
    return 0;
}

} // namespace pl::memory

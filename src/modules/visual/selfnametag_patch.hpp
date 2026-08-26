#pragma once

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <cstdint>
#include <cstring>

namespace bedrocktools::modules::visual::selfnametag_patch {

namespace detail {
inline void* target = nullptr;
inline std::uint8_t originalBytes[4]{};
inline bool haveOriginalBytes = false;
inline bool patched = false;
inline int references = 0;

inline bool resolveTarget() {
    if (target) return true;

    const std::uintptr_t nametag = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::Nametag
    );
    if (!nametag) return false;

    target = reinterpret_cast<void*>(
        nametag + bedrocktools::sdk::offsets::NameTag::mExtractNameTagsPatchOffset
    );
    std::memcpy(originalBytes, target, sizeof(originalBytes));
    haveOriginalBytes = true;
    return true;
}

inline bool apply() {
    if (patched) return true;
    if (!resolveTarget() || !haveOriginalBytes) return false;

    // ARM64 NOP: skip the local-player nametag rejection in NameTag::extractNameTags.
    const std::uint32_t nop = 0xD503201F;
    bedrocktools::sdk::patchMemory(target, &nop, sizeof(nop));
    patched = true;
    return true;
}

inline void restore() {
    if (!patched || !target || !haveOriginalBytes) return;
    bedrocktools::sdk::patchMemory(target, originalBytes, sizeof(originalBytes));
    patched = false;
}
}  // namespace detail

inline void init() {
    detail::resolveTarget();
}

inline bool acquire() {
    if (detail::references < 0) detail::references = 0;
    ++detail::references;
    if (detail::apply()) return true;

    --detail::references;
    return false;
}

inline void release() {
    if (detail::references <= 0) {
        detail::references = 0;
        return;
    }

    --detail::references;
    if (detail::references == 0) detail::restore();
}

inline bool isPatched() {
    return detail::patched;
}

}  // namespace bedrocktools::modules::visual::selfnametag_patch

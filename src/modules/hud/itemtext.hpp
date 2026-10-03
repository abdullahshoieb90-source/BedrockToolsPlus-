#pragma once

// Reading an item's damage without a byte pattern and without a symbol.
//
// The damage of a damageable stack lives in the stack's CompoundTag user data
// (Item::TAG_DAMAGE, the "Damage" key) — that is what
// ItemStackBase::getDamageValue() reads, and it is why reading a field of the
// stack cannot see it. The way to that value that survives every symbol table
// and every pattern change is the game's own text rendering:
//
//   CompoundTag::toString()  ->  {Damage:143s, ...}
//
// It is a *virtual* function, so RTTI plus its vtable slot reach it on any
// build (the same mechanism the HUD camera hook already uses), and the text is
// then searched for the damage key.
//
// Calling a slot that is not a string returning function with the string ABI
// would make the callee write a std::string through an undefined return slot,
// so a candidate has to pass two checks before it is called:
//
//   * the Itanium ABI hands the returned object to a function in a register
//     (x8 on AArch64, rdi on x86-64, and it is callee saved where needed) that
//     a function returning anything else never touches, so the prologue must
//     be seen using it, and
//   * CompoundTag::getId() — the only other slot worth a look — is recognised
//     by its body (it returns the tag type constant 10), which both keeps it
//     from being called and identifies its neighbour as the string slot.
//
// Whatever comes back is only used when it parses to a value the item's own
// maximum damage accepts.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include <pl/memory/Vtable.hpp>

namespace bedrocktools::huditems::text {
namespace detail {

// Instructions scanned for the return-slot register (64 bytes: the prologue of
// anything that stores it before doing real work).
inline constexpr std::size_t ScanWords = 16;

// A slot may hold a thunk; following a few unconditional branches reaches the
// implementation that was checked.
inline constexpr std::size_t MaxBranchHops = 4;

inline bool isBranch(std::uint32_t word) {
    return (word & 0xFC000000u) == 0x14000000u; // AArch64: b imm26
}

inline std::uint32_t readWord(const std::uint32_t* code, std::size_t index) {
    std::uint32_t word = 0;
    std::memcpy(&word, code + index, sizeof(word));
    return word;
}

// AArch64 encodings of the instructions that carry the return slot (x8):
//
//   mov  Xd, X8        ORR Xd, XZR, X8      0xAA0803E0 | Rd
//   mov  X8, Xm        ORR X8, XZR, Xm      0xAA0003E8 | (Rm << 16)
//   str  X8, [sp, #n]  STR (unsigned off)   0xF90003E8 | (imm12 << 10)
//   str  X8, [Xn, #n]                       0xF9000008 | (imm12 << 10) | (Rn << 5)
//   ldr  Xt, [X8, #n]  LDR (unsigned off)   0xF9400100 | (imm12 << 10) | Rt
//
// The two moves share one mask (both ends of the move are free registers: Rm in
// bits 20:16 and Rd in bits 4:0), so one comparison covers them both.
inline bool usesReturnSlotAArch64(const std::uint32_t* code, std::size_t words) {
    for (std::size_t i = 0; i < words; ++i) {
        const std::uint32_t word = readWord(code, i);
        if ((word & 0xFFE0FFE0u) == 0xAA0003E0u) return true; // mov X8, Xd / Xd, X8
        if ((word & 0xFFC003FFu) == 0xF90003E8u) return true; // str X8, [sp, #n]
        if ((word & 0xFFC0001Fu) == 0xF9000008u) return true; // str X8, [Xn, #n]
        if ((word & 0xFFC003E0u) == 0xF9400100u) return true; // ldr Xt, [X8, #n]
    }
    return false;
}

// mov w0, #10 ; ret — CompoundTag::getId() for a compound tag.
inline bool returnsTagTypeAArch64(const std::uint32_t* code, std::size_t words) {
    if (words < 2) return false;
    return readWord(code, 0) == 0x52800140u && readWord(code, 1) == 0xD65F03C0u;
}

// x86-64 encodings of the same idea: the return slot arrives in rdi, so the
// prologue copies it somewhere (`48 89 F8..FF`: mov r64, rdi) or stores through
// it (`48 89` with a memory ModRM below 0x80).
inline bool usesReturnSlotX86(const std::uint8_t* code, std::size_t bytes) {
    for (std::size_t i = 0; i + 2 < bytes; ++i) {
        if (code[i] != 0x48 || code[i + 1] != 0x89) continue;
        const std::uint8_t modrm = code[i + 2];
        if (modrm >= 0xF8) return true; // mov r64, rdi
        if (modrm < 0x80 && (modrm & 0x38) == 0x00) return true; // [rdi(+disp)]
    }
    return false;
}

// b8 0a 00 00 00 c3 — mov eax, 10 ; ret (the same tag type check on x86-64).
inline bool returnsTagTypeX86(const std::uint8_t* code, std::size_t bytes) {
    if (bytes < 6) return false;
    return code[0] == 0xB8 && code[1] == 0x0A && code[2] == 0x00 && code[3] == 0x00 &&
           code[4] == 0x00 && code[5] == 0xC3;
}

inline bool usesReturnSlot(const void* function) {
    if (!function) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(function);
#if defined(__aarch64__)
    return usesReturnSlotAArch64(reinterpret_cast<const std::uint32_t*>(bytes), ScanWords);
#elif defined(__x86_64__) || defined(_M_X64)
    return usesReturnSlotX86(bytes, ScanWords * 4);
#else
    (void)bytes;
    return false;
#endif
}

inline bool returnsTagType(const void* function) {
    if (!function) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(function);
#if defined(__aarch64__)
    return returnsTagTypeAArch64(reinterpret_cast<const std::uint32_t*>(bytes), 2);
#elif defined(__x86_64__) || defined(_M_X64)
    return returnsTagTypeX86(bytes, 6);
#else
    (void)bytes;
    return false;
#endif
}

// Whether a candidate really is the text renderer. The instruction checks are
// the only verdict here that depends on how a compiler laid a function out, and
// no test can lay one out the way the game's compiler does, so tests substitute
// their own verdict through this hook (the slot tells them which candidate it
// is); production leaves it null and the instructions decide.
inline bool (*slotVerifier)(const void* function, std::size_t slot) = nullptr;

inline bool usableTextSlot(const void* function, std::size_t slot) {
    if (slotVerifier) return slotVerifier(function, slot);
    return usesReturnSlot(function) && !returnsTagType(function);
}

// The address a slot really runs: a thunk in front of the implementation is
// followed (a `b`/`jmp` with no side effects), so the checks above inspect the
// function that would be called.
inline std::uintptr_t followBranches(std::uintptr_t address) {
    for (std::size_t hop = 0; hop < MaxBranchHops && address; ++hop) {
#if defined(__aarch64__)
        const std::uint32_t word = readWord(reinterpret_cast<const std::uint32_t*>(address), 0);
        if (!isBranch(word)) break;
        const std::int32_t offset = static_cast<std::int32_t>(word << 6) >> 6; // imm26 << 2 bytes
        address += static_cast<std::uintptr_t>(static_cast<std::ptrdiff_t>(offset) * 4);
#elif defined(__x86_64__) || defined(_M_X64)
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(address);
        if (bytes[0] != 0xE9) break; // jmp rel32
        std::int32_t offset = 0;
        std::memcpy(&offset, bytes + 1, sizeof(offset));
        address += static_cast<std::uintptr_t>(static_cast<std::ptrdiff_t>(offset)) + 5;
#else
        break;
#endif
    }
    return address;
}

} // namespace detail

// One virtual text source: a class with RTTI and the slots that may hold the
// function returning its text. The layout is resolved once per process; a
// lookup that finds nothing is retried (the game library may still be settling
// during the first frames), at most once a second.
struct Source {
    const char* typeInfo = nullptr;
    std::size_t slots[2] = {0, 0};
    std::uintptr_t address[2] = {0, 0};
    int usable = -1;          // index into `slots` that answered -1 = none
    bool resolved = false;    // a lookup finished and found a usable slot
    std::uint64_t lastAttempt = 0; // milliseconds, 0 = never

    bool available() const { return usable >= 0; }
};

// Resolves `source` in `module`, keeping the candidate slot that really is a
// std::string returning function. Returns whether it is usable.
bool resolve(Source& source, std::string_view module);

// The text of `self` through the resolved slot, empty when unavailable.
std::string text(Source& source, const void* self);

// The integer that follows `key` in `text` — `Damage:143s`, `"Damage": 143`,
// `damage=143` — or -1 when the key is absent or carries no number. Only the
// key itself counts: a name or a lore line that merely contains the word is
// not a match because the separator has to follow the key directly.
int numberAfter(std::string_view text, std::string_view key);

// A bounded single line copy of `text` for the on-screen diagnostics.
std::string snippet(std::string_view text, std::size_t maxLength = 96);

namespace detail {

inline std::uint64_t nowMilliseconds() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

} // namespace detail

inline bool resolve(Source& source, std::string_view module) {
    if (source.resolved) return true;

    // The picture is only incomplete while a lookup never found anything: a
    // library that is still being set up is retried, but not on every frame. A
    // substituted verdict (see slotVerifier) is free, so it skips the wait.
    const std::uint64_t now = detail::nowMilliseconds();
    if (!detail::slotVerifier && source.lastAttempt != 0 && now - source.lastAttempt < 1000) {
        return false;
    }
    source.lastAttempt = now;

    for (std::size_t i = 0; i < 2; ++i) {
        source.address[i] = pl::memory::resolveVtableFunction(source.typeInfo, source.slots[i], module);
    }

    for (std::size_t i = 0; i < 2; ++i) {
        const std::uintptr_t address = detail::followBranches(source.address[i]);
        const auto* function = reinterpret_cast<const void*>(address);
        if (!function) continue;
        // The tag type getter (getId) is the one candidate that is definitely
        // not the text: skipping it also leaves the other candidate for the
        // second round of this loop.
        if (!detail::usableTextSlot(function, source.slots[i])) continue;
        source.address[i] = address;
        source.usable = static_cast<int>(i);
        break;
    }

    source.resolved = source.usable >= 0;
    return source.resolved;
}

inline std::string text(Source& source, const void* self) {
    if (!source.available() || !self) return {};
    // The prototype has to be the real one: the compiler then passes the
    // destination of the returned std::string in the register the ABI reserves
    // for it, which is exactly what the checks in resolve() looked for.
    using TextSlotFn = std::string (*)(const void*);
    return reinterpret_cast<TextSlotFn>(source.address[source.usable])(self);
}

inline int numberAfter(std::string_view text, std::string_view key) {
    std::size_t position = 0;
    while ((position = text.find(key, position)) != std::string_view::npos) {
        std::size_t index = position + key.size();
        // A quoted key writes its closing quote before the separator.
        while (index < text.size() && (text[index] == '"' || text[index] == '\'')) ++index;
        if (index < text.size() && (text[index] == ':' || text[index] == '=')) {
            ++index;
            while (index < text.size() && (text[index] == ' ' || text[index] == '\t')) ++index;
            const std::size_t start = index;
            if (index < text.size() && text[index] == '-') ++index;
            while (index < text.size() && text[index] >= '0' && text[index] <= '9') ++index;
            if (index > start) {
                long value = 0;
                for (std::size_t digit = start; digit < index; ++digit) {
                    value = value * 10 + (text[digit] - '0');
                    if (value > 1000000) return -1; // not a plausible number
                }
                return static_cast<int>(value);
            }
        }
        position += key.size();
    }
    return -1;
}

inline std::string snippet(std::string_view text, std::size_t maxLength) {
    std::string out;
    out.reserve(std::min(maxLength + 3, text.size()));
    for (const char character : text) {
        if (out.size() >= maxLength) {
            out += "...";
            break;
        }
        out += (character == '\n' || character == '\r' || character == '\t') ? ' ' : character;
    }
    return out;
}

} // namespace bedrocktools::huditems::text

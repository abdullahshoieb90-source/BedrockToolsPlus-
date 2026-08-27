// SafeMem — fault-guarded memory reader.
//
// Several modules (InventoryHUD's inventory scan first and foremost) work
// by *probing* memory: they walk pointer-sized words of a live game
// object and treat each word as a candidate address for some other
// structure. A candidate word can be numerically plausible — non-null,
// far above the mmap-min page, perfectly aligned — and still point at
// address space that the process has never mapped. On Android that
// includes heap holes left by freed regions, the guard pages around
// thread stacks, and the holes between the .so mappings of
// libminecraftpe.so. Dereferencing one of those words raises SIGSEGV
// (or SIGBUS when the page is mapped but not backed) and takes the whole
// process down: that is the crash seen the first time the InventoryHUD
// module is enabled and the scan starts dereferencing candidates.
//
// This header makes such a dereference *recoverable*. Every guarded read
// installs a per-thread sigsetjmp/sigaction window around a plain copy;
// if the copy faults on this thread the window longjmps out and the
// caller receives its fallback value instead of a dead process.
//
// Design notes:
//   * One thread_local guard state per thread. The signal handler only
//     claims the fault when the faulting thread is the thread that armed
//     the window — a genuine SIGSEGV in any other thread falls through to
//     the previously installed handler (or SIG_DFL) so a real crash in
//     the game, ART or the crash logger is never swallowed.
//   * SA_ONSTACK | SA_NODEFER (and *not* SA_RESETHAND): SA_NODEFER keeps
//     the delivery from blocking the signal we are about to siglongjmp
//     past, while leaving our handler installed so the next unmapped
//     candidate in the same scan is recoverable too. The `faulted` latch
//     is what prevents a nested fault from looping in the handler.
//   * siglongjmp (not longjmp) so the caller's signal mask — which the
//     kernel has just modified to deliver SIGSEGV/SIGBUS — is restored
//     on the way out.
//   * A cheap filter (null, low page, misalignment, zero length) runs
//     before any of this, so the scan's overwhelmingly common rejections
//     never touch the signal machinery at all.
//   * Header-only. The sigaction state and the previous-handler records
//     are inline variables, so all translation units share one instance
//     even though the guard state itself is thread_local.
//
// What it does not do: it cannot make a read of a *mapped but racy*
// object safe (torn reads are still possible), and it does not protect
// faults raised from inside a hooked game function — only the copy that
// happens between the arm and the disarm.

#pragma once

#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#if !defined(_WIN32)
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace bedrocktools::mem {

// Anything at or below this address is a NULL-with-low-bits sentinel
// (offset arithmetic on a null pointer, a bitfield packed into the low
// half of a word, ...). It is rejected by the filter below without
// ever entering a signal window.
inline constexpr std::uintptr_t kMinValidAddress = 0x1000;

// A read larger than this is refused outright rather than guarded.
// Probes here are single words or small byte runs; a huge request
// means the caller computed a bogus length.
inline constexpr std::size_t kMaxGuardedBytes = 0x1000;

// ---------------------------------------------------------------------------
// Filter: the cheap rejections. Everything the scan rejects most of the
// time (null, the low page, misaligned words) is decided here, so the
// signal machinery below only ever runs for a plausible-looking pointer.
// ---------------------------------------------------------------------------

// True when [addr, addr + size) looks plausible enough to be worth
// dereferencing: non-null, above the mmap-min page, naturally aligned
// for its own size and of a sane length.
inline bool isReadableCandidate(const void* addr, std::size_t size) noexcept {
    if (size == 0) return false;
    if (size > kMaxGuardedBytes) return false;
    const auto v = reinterpret_cast<std::uintptr_t>(addr);
    if (v < kMinValidAddress) return false;          // null + low-page sentinel
    if ((v & (size - 1)) != 0) return false;         // misaligned for this width
    // Reject a range that wraps past the top of the address space.
    if (v + size < v) return false;
    return true;
}

// Same filter, without the alignment requirement — for byte runs.
inline bool isReadableBytes(const void* addr, std::size_t size) noexcept {
    if (size == 0) return false;
    if (size > kMaxGuardedBytes) return false;
    const auto v = reinterpret_cast<std::uintptr_t>(addr);
    if (v < kMinValidAddress) return false;
    if (v + size < v) return false;
    return true;
}

#if defined(_WIN32)

// POSIX signals are not available on this host, so the guard degrades to
// the plain filter + copy. The module ships for Android only; the host
// unit tests run on Linux, where the real implementation below is used.
inline bool tryReadBytes(const void* addr, void* out, std::size_t size) noexcept {
    if (!isReadableBytes(addr, size)) return false;
    std::memcpy(out, addr, size);
    return true;
}

#else // POSIX (Android / Linux / macOS)

// ---------------------------------------------------------------------------
// Per-thread guard state.
//
// `armed` is the switch the signal handler reads: it is true only for
// the narrow window in which this thread is executing a guarded copy,
// so a fault anywhere else — on this thread or on any other — is left
// to the previously installed handler.
//
// `faulted` latches the first recovery. Because SA_RESETHAND is not
// used, a nested fault re-enters this handler; the latch is what stops
// it from longjmping into a jump buffer that is no longer the live one
// (it falls through to SIG_DFL and dies instead).
// ---------------------------------------------------------------------------
struct GuardState {
    bool armed = false;
    bool faulted = false;
    sigjmp_buf buf;
    pthread_t thread{};                    // the thread that armed the window
    const volatile void* canary = nullptr; // address of the arming frame's local
    int lastSignal = 0;                    // diagnostics: SIGSEGV / SIGBUS / 0
    std::uintptr_t lastAddress = 0;        // diagnostics: faulting si_addr
};

inline thread_local GuardState t_guard;

namespace detail {

inline struct sigaction& prevAction(int sig) noexcept {
    // Index 0 -> SIGSEGV, index 1 -> SIGBUS.
    static struct sigaction table[2] = {};
    return table[sig == SIGBUS ? 1 : 0];
}

// Runs on the faulting thread, inside the kernel's signal delivery.
// Only async-signal-safe work happens here; in particular no allocation,
// no logging and no locks.
inline void faultHandler(int sig, siginfo_t* info, void* ucontext) noexcept {
    GuardState& guard = t_guard;

    // Claim the fault only when *this* thread armed a window and has not
    // already recovered from one. A fault in any other thread (or a real
    // bug in this one, outside a guarded copy) is not ours to hide.
    if (guard.armed && !guard.faulted &&
        (sig == SIGSEGV || sig == SIGBUS) &&
        pthread_equal(guard.thread, pthread_self()) &&
        guard.canary != nullptr) {
        guard.faulted = true;
        guard.armed = false;
        guard.lastSignal = sig;
        guard.lastAddress = info ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0;
        siglongjmp(guard.buf, 1);
    }

    // Not ours — hand the fault to whoever owned the signal before us so
    // a genuine crash still reaches the game's crash logger / ART, and
    // otherwise to SIG_DFL so the process dies and core dumps exactly as
    // it would without this header. Swallowing it would turn a real bug
    // into a silent wrong value, which is worse than the crash.
    const struct sigaction& prev = prevAction(sig);
    // sa_handler and sa_sigaction share storage; compare the raw pointer
    // so SIG_DFL / SIG_IGN can be recognised whichever field is live.
    const auto prevFn = reinterpret_cast<void*>(prev.sa_handler);
    const bool isDefaultOrIgnored =
        prevFn == reinterpret_cast<void*>(SIG_DFL) || prevFn == reinterpret_cast<void*>(SIG_IGN);
    if (!isDefaultOrIgnored) {
        if ((prev.sa_flags & SA_SIGINFO) != 0) {
            prev.sa_sigaction(sig, info, ucontext);
        } else {
            prev.sa_handler(sig);
        }
        return;
    }
    struct sigaction dfl;
    std::memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    ::sigaction(sig, &dfl, nullptr);
    ::raise(sig);
}

// Installs our handler for SIGSEGV/SIGBUS, remembering whatever was
// there so faults we do not own can be passed on. Idempotent: if our
// handler is already the installed one it does nothing, so calling it
// again after a recovery never chains us to ourselves.
inline void installHandlers() noexcept {
    static constexpr int kGuardedSignals[2] = {SIGSEGV, SIGBUS};
    for (int i = 0; i < 2; ++i) {
        const int sig = kGuardedSignals[i];
        struct sigaction current;
        std::memset(&current, 0, sizeof(current));
        if (::sigaction(sig, nullptr, &current) == 0 &&
            (current.sa_flags & SA_SIGINFO) != 0 &&
            current.sa_sigaction == &faultHandler) {
            continue; // already ours
        }
        struct sigaction act;
        std::memset(&act, 0, sizeof(act));
        act.sa_sigaction = &faultHandler;
        sigemptyset(&act.sa_mask);
        // SA_SIGINFO -> receive si_addr; SA_ONSTACK -> run on the
        // alternate stack if a stack overflow is what faulted; SA_NODEFER
        // -> do not block the very signal we are about to siglongjmp out
        // of, and let a nested fault reach this handler again (the
        // `faulted` latch above is what stops that from looping).
        //
        // SA_RESETHAND is deliberately *not* used: it would drop our
        // handler after the first recovery, so the second unmapped
        // candidate in the same scan would kill the process.
        act.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
        ::sigaction(sig, &act, &prevAction(sig));
    }
}

inline GuardState& guardState() noexcept {
    // Installed on first touch of the thread-local state, i.e. before the
    // first window is armed, so the very first guarded read is covered.
    static thread_local const bool installed = [] {
        installHandlers();
        return true;
    }();
    (void)installed;
    return t_guard;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Copies `size` bytes from `addr` into `out`. Returns false (and leaves
// `out` untouched) when the filter rejects the range or when the copy
// faulted and was caught.
inline bool tryReadBytes(const void* addr, void* out, std::size_t size) noexcept {
    if (!isReadableBytes(addr, size)) return false;
    if (out == nullptr) return false;

    GuardState& guard = detail::guardState();
    if (guard.armed) {
        // Already inside a guarded copy on this thread. Re-arming would
        // replace the jump buffer the outer window needs, so bail out;
        // the outer call reports the failure and unwinds normally.
        return false;
    }

    // Address of a local in *this* frame. After siglongjmp back into this
    // function nothing above may be trusted, so the buffer is only jumped
    // to while this frame is provably alive; the canary documents that and
    // gives the handler a cheap liveness check.
    volatile int frame = 0;

    guard.thread = pthread_self();
    guard.canary = &frame;
    guard.faulted = false;
    if (sigsetjmp(guard.buf, 1) != 0) {
        // Returned here from faultHandler: the copy faulted.
        guard.canary = nullptr;
        return false;
    }
    guard.armed = true;

    // Read through a volatile pointer so the compiler emits the load(s)
    // here, between the arm and the disarm, instead of hoisting them.
    const volatile std::byte* src = static_cast<const volatile std::byte*>(addr);
    auto* dst = static_cast<std::byte*>(out);
    for (std::size_t i = 0; i < size; ++i) {
        dst[i] = src[i];
    }

    guard.armed = false;
    guard.canary = nullptr;
    return true;
}

// Reads one `T` from `addr`, falling back to `fallback` when the range is
// implausible or the read faulted. `T` must be trivially copyable and
// naturally sized (power of two) so the alignment filter is meaningful.
template <class T>
inline T tryRead(const void* addr, T fallback = T{}) noexcept {
    static_assert(std::is_trivially_copyable<T>::value,
                  "SafeMem::tryRead requires a trivially copyable type");
    static_assert(sizeof(T) > 0 && (sizeof(T) & (sizeof(T) - 1)) == 0,
                  "SafeMem::tryRead requires a power-of-two size");
    if (!isReadableCandidate(addr, sizeof(T))) return fallback;
    T value;
    if (!tryReadBytes(addr, &value, sizeof(T))) return fallback;
    return value;
}

// Convenience wrapper for the pointer walks: reads a pointer-sized word
// and returns it as void* (nullptr on reject or fault).
inline void* tryReadPtr(const void* addr) noexcept {
    const auto v = tryRead<std::uintptr_t>(addr, std::uintptr_t{0});
    return reinterpret_cast<void*>(v);
}

// Signal that terminated the most recent guarded read on this thread
// (SIGSEGV / SIGBUS), or 0 when the last read did not fault. Diagnostics
// only — reading it from a thread that never faulted is meaningless.
inline int lastFaultSignal() noexcept { return t_guard.lastSignal; }

// Address that faulted during the most recent guarded read on this
// thread, or 0 when the last read did not fault. Diagnostics only.
inline std::uintptr_t lastFaultAddress() noexcept { return t_guard.lastAddress; }

#endif // !_WIN32

} // namespace bedrocktools::mem

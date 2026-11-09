#!/usr/bin/env python3
"""Checks a built libBedrockToolsPlus.so for symbols that can never resolve.

Why this exists
---------------
The mod is a shared object that the preloader loads with
``dlopen(..., RTLD_NOW)``. With RTLD_NOW, *one* undefined symbol makes the
whole dlopen() fail - and then the mod never runs at all: no modules are
registered and the launcher menu comes up completely empty. That is exactly
what happened when the Custom Capes module stopped defining
STB_IMAGE_WRITE_IMPLEMENTATION while the Wings module still called
stbi_write_png(): the build succeeded, the levipack installed fine, and every
single module vanished from the menu.

This script fails the build when the shared object references a symbol that is
neither
  * defined inside the shared object,
  * provided by the preloader API (pl::*), or
  * provided by the platform (libc/libm/libdl/liblog/libandroid/libEGL/libGLES/
    libc++_shared/std::*).

Usage:
    python3 scripts/check_undefined_symbols.py build/.../libBedrockToolsPlus.so
"""

import argparse
import re
import subprocess
import sys

# Libraries the preloader and Android guarantee at dlopen() time.
PLATFORM_PREFIXES = (
    "_ZNSt", "_ZSt", "_ZdlPv", "_Znwm", "_ZdaPv", "_Znam",
    "__cxa_", "__gxx_", "_Unwind_", "_ITM_", "__gmon_", "_Jv_",
    "pthread_", "dl", "egl", "gl",
)
# Everything the mod imports from the preloader (PL_EXPORT) starts with this.
PRELOADER_PREFIX = "_ZN2pl"

# Host-only artefacts: the CI check runs against the Android build, where these
# never appear. Kept permissive so the script is also usable locally.
IGNORED_EXACT = {
    "_GLOBAL_OFFSET_TABLE_",
    "__dso_handle",
    "__libc_single_threaded",
    "__tls_get_addr",
    "_ZNSt3__1",  # libc++ on device; covered by the prefix anyway
}

# Platform-only symbols that the host libc cannot answer for (they exist in
# bionic / the NDK but not in glibc): EGL, GLES and the Android log.
def _is_android_platform(symbol: str) -> bool:
    # C++-mangled EGL/GLES entry points too (eglGetCurrentContext() is
    # declared `void* eglGetCurrentContext(void)`): match the demangled name.
    if re.match(r"^(?:egl|gl[A-Z]|__android_log_)", symbol):
        return True
    demangled = _demangle(symbol)
    return bool(re.match(r"^(?:egl|gl[A-Z]|__android_log_)[A-Za-z0-9_]*\(", demangled))


def _resolves_on_host(symbol: str) -> bool:
    """True when the C symbol exists in the host's libc/libm/libdl.

    The mod's C-level imports are the same function names on bionic, so this
    catches typos, missing third-party definitions (stbi_*) and anything else
    that is neither ours nor the platform's, without maintaining a list of
    every libc function.
    """
    try:
        import ctypes
        ctypes.CDLL(None)[symbol]
        return True
    except (OSError, AttributeError, KeyError):
        return False


def undefined_symbols(shared_object: str):
    output = subprocess.run(
        ["nm", "-u", shared_object],
        check=True, capture_output=True, text=True,
    ).stdout.splitlines()
    symbols = []
    for line in output:
        parts = line.split()
        if not parts:
            continue
        symbol = parts[-1]
        # Strip the version tag ("@GLIBC_2.17") and the weak-binding marker.
        symbol = symbol.split("@", 1)[0]
        if symbol:
            symbols.append(symbol)
    return symbols


def defined_symbols(shared_object: str):
    output = subprocess.run(
        ["nm", "--defined-only", shared_object],
        check=True, capture_output=True, text=True,
    ).stdout.splitlines()
    return {line.split()[-1].split("@", 1)[0] for line in output if line.split()}


def is_explained(symbol: str) -> bool:
    if symbol in IGNORED_EXACT:
        return True
    if symbol.startswith(PRELOADER_PREFIX):
        return True
    if symbol.startswith(PLATFORM_PREFIXES):
        return True
    if _is_android_platform(symbol):
        return True
    # C++ compiler runtime helpers and template instantiations that are
    # instantiated inside the mod itself are reported as defined, so anything
    # left here is genuinely missing.
    if symbol.startswith("_Z"):
        # Any template/standard-library symbol the mod references but does not
        # define must come from libc++ (linked shared on Android).
        return _demangles_to_std(symbol)
    # Plain C symbol: it has to come from the platform (libc/libm/libdl/...).
    return _resolves_on_host(symbol)


def _demangle(symbol: str) -> str:
    try:
        return subprocess.run(
            ["c++filt", symbol], check=True, capture_output=True, text=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return symbol


def _demangles_to_std(symbol: str) -> bool:
    demangled = _demangle(symbol)
    if demangled.startswith("std::") or demangled.startswith("__cxxabiv1"):
        return True
    # Typeinfo, vtables and guards for standard-library types live in libc++.
    for prefix in ("typeinfo for std::", "typeinfo for __cxxabiv1",
                   "vtable for std::", "vtable for __cxxabiv1",
                   "guard variable for std::", "construction vtable for std::"):
        if demangled.startswith(prefix):
            return True
    return demangled.startswith(("operator new", "operator delete"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("shared_object", help="path to libBedrockToolsPlus.so")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    undefined = set(undefined_symbols(args.shared_object))
    defined = defined_symbols(args.shared_object)

    missing = sorted(s for s in undefined
                     if s not in defined and not is_explained(s))
    if missing:
        print(f"{args.shared_object}: {len(missing)} symbol(s) can never resolve at dlopen():",
              file=sys.stderr)
        for symbol in missing:
            demangled = subprocess.run(["c++filt", symbol], capture_output=True,
                                       text=True).stdout.strip()
            print(f"  {symbol}\n      {demangled}", file=sys.stderr)
        print("\nThe preloader loads this library with RTLD_NOW, so a single "
              "missing symbol makes dlopen() fail and *every* module disappears "
              "from the launcher menu. Add the definition (check for a "
              "#if !defined(__ANDROID__) that dropped an implementation another "
              "translation unit still calls).", file=sys.stderr)
        return 1

    if not args.quiet:
        print(f"{args.shared_object}: all {len(undefined)} undefined symbols resolve "
              f"inside the library, from the preloader, or from the platform")
    return 0


if __name__ == "__main__":
    sys.exit(main())

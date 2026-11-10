#!/usr/bin/env bash
# Compiles every translation unit for the host, links them into one shared
# object and verifies that no symbol is left unresolvable.
#
# This is the check that catches the class of bug that made every module
# disappear from the launcher: the preloader loads the library with RTLD_NOW,
# so one undefined symbol aborts the whole dlopen().
#
#     ./scripts/fetch_test_headers.sh
#     ./scripts/host_link_check.sh

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
deps="${root}/deps/headers"
work="${TMPDIR:-/tmp}/bedrocktools-link-check"
stubs="${work}/ndkstub"

# Minimal NDK headers: these two are the only ones the host compiler cannot
# provide, and nothing in the check executes the declarations.
mkdir -p "${stubs}/EGL" "${stubs}/android"
# Start from scratch: a stale object left over from a deleted source would
# otherwise be linked in and report symbols nothing defines any more.
rm -rf "${work}/obj"
mkdir -p "${work}/obj"
cat > "${stubs}/EGL/egl.h" <<'HEADER'
#pragma once
typedef void* EGLDisplay;
typedef void* EGLSurface;
typedef int EGLBoolean;
typedef int EGLint;
#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_NO_CONTEXT ((void*)0)
EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface);
void* eglGetCurrentContext(void);
void* eglGetCurrentSurface(EGLint);
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, void*);
HEADER
printf '#pragma once\n#define __NDK_MAJOR__ 28\n' > "${stubs}/android/ndk-version.h"

flags=(-std=c++20 -O0 -fPIC -D__ANDROID__
       -I "${root}/src" -I "${root}/include" -I "${root}/third_party"
       -I "${deps}" -I "${deps}/entt" -I "${root}/tests/fakejni" -I "${root}/tests/fakepl"
       -I "${stubs}")

status=0
while IFS= read -r source; do
    object="${work}/obj/$(printf '%s' "${source#"${root}/"}" | tr '/' '_').o"
    if ! g++ "${flags[@]}" -c "${source}" -o "${object}" 2>"${work}/err.txt"; then
        echo "compile FAILED: ${source#"${root}/"}"
        head -3 "${work}/err.txt"
        status=1
    fi
done < <(find "${root}/src" -name '*.cpp' | sort)

if [ "${status}" -ne 0 ]; then
    echo "some translation units did not compile; not linking"
    exit 1
fi

g++ -shared -o "${work}/libbedrocktools.so" "${work}"/obj/*.o
python3 "${root}/scripts/check_undefined_symbols.py" "${work}/libbedrocktools.so"

#!/usr/bin/env bash
# Builds and runs the host-side unit tests.
#
# These cover the parts of the mod that are plain C++ and do not need Minecraft
# or the Android toolchain, so they can run in CI and locally with any C++20
# compiler.
#
# A few tests additionally include the preloader and nlohmann_json headers
# that xmake normally provides. Their include dirs are auto-detected from the
# xmake package cache and can be overridden with PRE_LOADER_INCLUDE /
# JSON_INCLUDE. When they are missing the affected tests are reported as
# skipped (not failed); each test's header comment documents how to build it
# manually. Tests that touch Android-only JNI code compile against the host
# fake in tests/fakejni instead of the NDK's <jni.h>.
#
#     ./scripts/run_tests.sh

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
outdir="${root}/build/tests"
cxx="${CXX:-g++}"
flags=(-std=c++20 -Wall -Wextra -O1 -I "${root}/src" -I "${root}/include")

preloader_inc="${PRE_LOADER_INCLUDE:-$(echo "${HOME}"/.xmake/packages/p/preloader/main/*/include 2>/dev/null | head -n1)}"
json_inc="${JSON_INCLUDE:-$(echo "${HOME}"/.xmake/packages/n/nlohmann_json/v3.11.3/*/include 2>/dev/null | head -n1)}"
entt_inc="${ENTT_INCLUDE:-$(echo "${HOME}"/.xmake/packages/e/entt/*/include 2>/dev/null | head -n1)}"

mkdir -p "${outdir}"

status=0
for source in "${root}"/tests/*_test.cpp; do
    name="$(basename "${source}" .cpp)"
    printf '\n=== %s ===\n' "${name}"

    # Per-test requirements (see the header comment of each test).
    extra=()
    extra_srcs=()
    skip=""
    case "${name}" in
        jumpcircle_test|damagecircle_test)
            extra+=(-I "${root}/tests/fakepl" -I "${root}/tests/fakejson")
            ;;
        blockoutline_test)
            # Includes the production renderer with host substitutes for the
            # preloader hook API and the small JSON surface used by configs.
            extra+=(-I "${root}/tests/fakepl" -I "${root}/tests/fakejson")
            ;;
        commandhotkey_test)
            if [ -n "${preloader_inc}" ] && [ -d "${preloader_inc}" ] &&
               [ -n "${json_inc}" ] && [ -d "${json_inc}" ]; then
                extra+=(-I "${preloader_inc}" -I "${json_inc}" -I "${root}/tests/fakejni")
            else
                skip="preloader/nlohmann_json headers (set PRE_LOADER_INCLUDE and JSON_INCLUDE)"
            fi
            ;;
        commentkey_test)
            if [ -n "${preloader_inc}" ] && [ -d "${preloader_inc}" ] &&
               [ -n "${json_inc}" ] && [ -d "${json_inc}" ]; then
                extra+=(-I "${preloader_inc}" -I "${json_inc}")
            else
                skip="preloader/nlohmann_json headers (set PRE_LOADER_INCLUDE and JSON_INCLUDE)"
            fi
            ;;
        crosshair_test)
            # This test owns its pl::modmenu / pl::memory stubs, so it only
            # needs the API type declarations plus the host JSON/JNI fakes; a
            # full preloader header would redeclare those stubs.
            extra+=(-I "${root}/tests/crosshair_fakepl" -I "${root}/tests/fakejson"
                    -I "${root}/tests/fakejni")
            ;;
        effectdisplay_test|inventoryhud_test|armorhud_test)
            # Also needs entt from the xmake packages; fmt and <android/log.h>
            # are covered by the host fakes in tests/fakejni.
            if [ -n "${preloader_inc}" ] && [ -d "${preloader_inc}" ] &&
               [ -n "${json_inc}" ] && [ -d "${json_inc}" ] &&
               [ -n "${entt_inc}" ] && [ -d "${entt_inc}" ]; then
                extra+=(-I "${preloader_inc}" -I "${json_inc}" -I "${entt_inc}"
                        -I "${root}/tests/fakejni")
            else
                skip="preloader/nlohmann_json/entt headers (set PRE_LOADER_INCLUDE, JSON_INCLUDE and ENTT_INCLUDE)"
            fi
            ;;
        externalbuttonrefresh_test)
            extra+=(-I "${root}/tests/fakejni")
            ;;
        hitbox_patch_test)
            # Includes the production module; the preloader and
            # nlohmann_json headers come from the host fakes. Drives the
            # render hook frame by frame, so it needs threads for the sleeps.
            extra+=(-I "${root}/tests/fakepl" -I "${root}/tests/fakejson" -pthread)
            ;;
        wings_render_state_test)
            # Includes the real wings renderer to exercise its shared color
            # state with the same host hook/JSON fakes as Block Outline.
            extra+=(-I "${root}/third_party" -I "${root}/tests/fakepl"
                    -I "${root}/tests/fakejson" -pthread)
            ;;
        module_registration_smoke_test)
            # Registers *every* module through the real launcher path, so it
            # needs the full preloader API, a real nlohmann_json and entt, plus
            # the module translation units themselves (including the ones that
            # only exist to be iterated over).
            if [ -n "${preloader_inc}" ] && [ -d "${preloader_inc}" ] &&
               [ -n "${json_inc}" ] && [ -d "${json_inc}" ] &&
               [ -n "${entt_inc}" ] && [ -d "${entt_inc}" ]; then
                extra+=(-I "${root}/third_party" -I "${preloader_inc}" -I "${json_inc}"
                        -I "${entt_inc}" -I "${root}/tests/fakejni")
                extra_srcs+=("${root}/src/launcher/ModuleMenu.cpp"
                             "${root}/src/launcher/ExternalButtonRefresh.cpp"
                             "${root}/src/modules/ModuleRegistry.cpp"
                             "${root}/src/modules/hud/effectdisplay.cpp"
                             "${root}/src/modules/hud/crosshair.cpp"
                             "${root}/src/modules/hud/inventoryhud.cpp"
                             "${root}/src/modules/hud/armorhud.cpp"
                             "${root}/src/modules/hud/huditems.cpp"
                             "${root}/src/modules/visual/hitbox.cpp"
                             "${root}/src/modules/visual/blockoutline.cpp"
                             "${root}/src/modules/visual/jumpcircle.cpp"
                             "${root}/src/modules/visual/damagecircle.cpp"
                             "${root}/src/modules/visual/wings.cpp"
                             "${root}/src/core/StbImageWrite.cpp"
                             "${root}/src/modules/misc/hitsound.cpp"
                             "${root}/src/modules/misc/commentkey.cpp"
                             "${root}/src/modules/misc/commandhotkey.cpp"
                             "${root}/src/config/ConfigManager.cpp")
            else
                skip="preloader/nlohmann_json/entt headers (set PRE_LOADER_INCLUDE, JSON_INCLUDE and ENTT_INCLUDE)"
            fi
            ;;
        wings_patch_test)
            # Builds the real module as a second translation unit; the
            # preloader/nlohmann_json headers it includes come from the
            # xmake packages when available and from the host fakes
            # (tests/fakepl, tests/fakejson) otherwise.
            extra+=(-I "${root}/third_party" -I "${root}/tests/fakepl")
            if [ -n "${json_inc}" ] && [ -d "${json_inc}" ]; then
                extra+=(-I "${json_inc}")
            else
                extra+=(-I "${root}/tests/fakejson")
            fi
            if [ -n "${preloader_inc}" ] && [ -d "${preloader_inc}" ]; then
                extra+=(-I "${preloader_inc}")
            else
                extra+=(-I "${root}/tests/fakepl")
            fi
            # Wings writes its default texture with stb_image_write, whose
            # single implementation now lives in src/core/StbImageWrite.cpp.
            extra_srcs+=("${root}/src/modules/visual/wings.cpp"
                         "${root}/src/core/StbImageWrite.cpp")
            ;;
    esac

    if [ -n "${skip}" ]; then
        echo "  SKIP: needs ${skip}"
        continue
    fi

    if ! "${cxx}" "${flags[@]}" ${extra[@]+"${extra[@]}"} "${source}" "${extra_srcs[@]}" -o "${outdir}/${name}"; then
        echo "  FAIL to compile (see above)"
        status=1
        continue
    fi
    if ! "${outdir}/${name}"; then
        status=1
    fi
done

printf '\n'
if [ "${status}" -eq 0 ]; then
    echo "all test binaries passed"
else
    echo "some tests failed"
fi
exit "${status}"

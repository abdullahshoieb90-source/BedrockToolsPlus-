#!/usr/bin/env bash
# Downloads the headers the host tests need (preloader, nlohmann_json, entt)
# into deps/headers, which is git-excluded.
#
# xmake normally provides these for the Android build; the host test runner can
# only compile the tests that include them when these directories exist. The
# sandbox that builds this repo clears /tmp, so the script keeps them inside the
# checkout instead:
#
#     ./scripts/fetch_test_headers.sh
#     PRE_LOADER_INCLUDE=$PWD/deps/headers \
#     JSON_INCLUDE=$PWD/deps/headers \
#     ENTT_INCLUDE=$PWD/deps/headers ./scripts/run_tests.sh

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
deps="${root}/deps/headers"
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT

mkdir -p "${deps}" "${deps}/nlohmann" "${deps}/entt/entt"

echo "preloader headers"
git clone --depth 1 https://github.com/LiteLDev/preloader-android.git "${tmp}/preloader" >/dev/null 2>&1
cp -r "${tmp}/preloader/include/pl" "${deps}/"

echo "nlohmann_json 3.11.3"
gh api -H "Accept: application/vnd.github.raw" \
    "repos/nlohmann/json/contents/single_include/nlohmann/json.hpp?ref=v3.11.3" \
    > "${deps}/nlohmann/json.hpp"

echo "entt 3.13.2"
git clone --depth 1 --branch v3.13.2 https://github.com/skypjack/entt.git "${tmp}/entt" >/dev/null 2>&1
cp -r "${tmp}/entt/src/entt/." "${deps}/entt/"

grep -qx 'deps/' "${root}/.git/info/exclude" 2>/dev/null || echo 'deps/' >> "${root}/.git/info/exclude"
echo "ready: ${deps}"

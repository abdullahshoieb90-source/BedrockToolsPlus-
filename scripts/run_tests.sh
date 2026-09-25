#!/usr/bin/env bash
# Host-side smoke check for the remaining module sources.
#
# The feature modules (and the unit tests that covered them) were removed;
# only Hitbox remains. This script syntax-compiles Hitbox and the module
# registry against the host fakes (tests/fakejson, tests/fakepl) so plain
# C++ regressions are caught in CI and locally with any C++20 compiler.
#
#     ./scripts/run_tests.sh

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cxx="${CXX:-g++}"
flags=(-std=c++20 -Wall -Wextra -fsyntax-only
       -I "${root}/src" -I "${root}/include"
       -I "${root}/tests/fakejson" -I "${root}/tests/fakepl")

status=0
for source in \
    "${root}/src/modules/visual/hitbox.cpp" \
    "${root}/src/modules/ModuleRegistry.cpp"
do
    printf '\n=== %s ===\n' "${source#"${root}"/}"
    if ! "${cxx}" "${flags[@]}" "${source}"; then
        status=1
    fi
done

printf '\n'
if [ "${status}" -eq 0 ]; then
    echo "all smoke checks passed"
else
    echo "some smoke checks failed"
fi
exit "${status}"

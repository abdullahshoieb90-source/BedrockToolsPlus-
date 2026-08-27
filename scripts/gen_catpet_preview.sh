#!/usr/bin/env bash
# Renders the Cat Pet module's overlay geometry offline into PNGs so the pet
# can be judged without launching Minecraft. The preview uses the exact part
# hierarchy, pose solver, palette and face shading the game runs
# (src/modules/visual/catpet_shape.hpp).
#
#     ./scripts/gen_catpet_preview.sh [outdir]
#
# Output (default build/catpet-preview/): <style>.png (2x3 poses per style)
# and sheet.png (all styles, idle pose).

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
outdir="${1:-${root}/build/catpet-preview}"
cxx="${CXX:-g++}"

mkdir -p "${outdir}"
"${cxx}" -std=c++20 -O2 -w -I "${root}/src" -I "${root}/include" -I "${root}/third_party" \
    "${root}/tools/catpet_preview.cpp" -o "${root}/build/catpet_preview"
"${root}/build/catpet_preview" "${outdir}"

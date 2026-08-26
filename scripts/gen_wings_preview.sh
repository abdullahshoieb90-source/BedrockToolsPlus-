#!/usr/bin/env bash
# Renders the Wings module's overlay geometry offline into PNGs so a wing
# style can be judged without launching Minecraft. The preview uses the exact
# bone tables, rest-pose fan, taper, sweep and face shading the game runs
# (src/modules/visual/wings_shape.hpp + wings_styles.hpp), and also emits the
# legacy renderer for comparison.
#
#     ./scripts/gen_wings_preview.sh [outdir]
#
# Output (default build/wings-preview/): <style>.png (2x2 poses per style),
# legacy_dragon.png and compare_dragon.png (legacy vs current).

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
outdir="${1:-${root}/build/wings-preview}"
cxx="${CXX:-g++}"

mkdir -p "${outdir}"
"${cxx}" -std=c++20 -O2 -w -I "${root}/src" -I "${root}/include" -I "${root}/third_party" \
    "${root}/tools/wings_preview.cpp" -o "${root}/build/wings_preview"
"${root}/build/wings_preview" "${outdir}"

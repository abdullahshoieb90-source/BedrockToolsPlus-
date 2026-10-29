#!/usr/bin/env bash
# Renders the Wings module's geometry offline into PNGs so a wing style can be
# judged without launching Minecraft. The preview uses the exact design table,
# membrane builder and shading the game runs (src/modules/visual/wings_design.hpp
# + wings_surface.hpp + wings_shape.hpp).
#
#     ./scripts/gen_wings_preview.sh [outdir]
#
# Output (default build/wings-preview/): <style>.png (2x2 poses per style),
# <style>_hero.png (3/4 close-up) and showcase.png (every style at once).

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
outdir="${1:-${root}/build/wings-preview}"
cxx="${CXX:-g++}"

mkdir -p "${outdir}"
"${cxx}" -std=c++20 -O2 -w -I "${root}/src" -I "${root}/include" -I "${root}/third_party" \
    "${root}/tools/wings_preview.cpp" -o "${root}/build/wings_preview"
"${root}/build/wings_preview" "${outdir}"

#!/usr/bin/env bash
# Renders the Cape Physics module's cloth offline into PNGs so the cape's
# look and motion can be judged without launching Minecraft. The preview
# runs the exact simulation the game runs (src/modules/visual/
# capephysics_sim.hpp: Verlet cloth, gravity/wind/drag, body collision,
# per-cell palette sampling and face shading) through a small software
# rasterizer.
#
#     ./scripts/gen_capephysics_preview.sh [outdir]
#
# Output (default build/capephysics-preview/): rest.png, run.png (sprinting
# phases), wind.png (max gust), sizes.png (22x23/64x32/128x64/704x736
# sources through the any-size pipeline) and detail.png (Native vs Fine
# cloth grids).

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
outdir="${1:-${root}/build/capephysics-preview}"
cxx="${CXX:-g++}"

mkdir -p "${outdir}"
"${cxx}" -std=c++20 -O2 -w -I "${root}/src" -I "${root}/include" -I "${root}/third_party" \
    "${root}/tools/capephysics_preview.cpp" -o "${root}/build/capephysics_preview"
"${root}/build/capephysics_preview" "${outdir}"

#!/usr/bin/env python3
"""Generate the default Wings assets.

Produces three files:

* resources/wings/wings_geometry.json - a Minecraft Bedrock skin-pack
  geometry (identifier "geometry.wings") that keeps the standard player bones
  and adds two bat-wing bones attached to the back of the body.
* resources/wings/wings.png - the matching 64x64 RGBA skin texture. It uses
  the standard 64x64 box-unwrap layout for the body and puts a black bat-wing
  silhouette in the two free regions referenced by the geometry.
* include/bedrocktools/modules/visual/wings_default.hpp - the same assets
  embedded as code so the WingsModule can work immediately after install,
  even when no external files are present on the phone.

Run from the repository root (or anywhere; paths are resolved relative to
this file):
    python3 scripts/gen_wings_assets.py
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RES = ROOT / "resources" / "wings"
INC = ROOT / "include" / "bedrocktools" / "modules" / "visual"

TEX_W = 64
TEX_H = 64

IDENTIFIER = "geometry.wings"

# ---------------------------------------------------------------------------
# Bedrock geometry. Standard humanoid bones keep the default player body
# rendering and animating; the two extra wing bones are thin slabs attached to
# the back of the body. UV regions:
#   head  (0,0) 8x8x8    body (16,16) 8x12x4
#   right arm (40,16)    left arm (32,48)
#   right leg (0,16)     left leg (16,48)
#   right wing (48,16) 6x10x1    left wing (48,32) 6x10x1
# ---------------------------------------------------------------------------
GEOMETRY = r'''{
  "format_version": "1.12.0",
  "minecraft:geometry": [
    {
      "description": {
        "identifier": "geometry.wings",
        "texture_width": 64,
        "texture_height": 64,
        "visible_bounds_width": 3,
        "visible_bounds_height": 3.5,
        "visible_bounds_offset": [0, 1.25, 0]
      },
      "bones": [
        { "name": "root", "pivot": [0, 0, 0] },
        { "name": "waist", "parent": "root", "pivot": [0, 12, 0] },
        {
          "name": "body",
          "parent": "waist",
          "pivot": [0, 24, 0],
          "cubes": [ { "origin": [-4, 12, -2], "size": [8, 12, 4], "uv": [16, 16] } ]
        },
        {
          "name": "head",
          "parent": "body",
          "pivot": [0, 24, 0],
          "cubes": [ { "origin": [-4, 24, -4], "size": [8, 8, 8], "uv": [0, 0] } ]
        },
        {
          "name": "rightArm",
          "parent": "body",
          "pivot": [5, 22, 0],
          "cubes": [ { "origin": [-9, 12, -2], "size": [4, 12, 4], "uv": [40, 16] } ]
        },
        {
          "name": "leftArm",
          "parent": "body",
          "pivot": [-5, 22, 0],
          "cubes": [ { "origin": [5, 12, -2], "size": [4, 12, 4], "uv": [32, 48] } ]
        },
        {
          "name": "rightLeg",
          "parent": "waist",
          "pivot": [1.9, 12, 0],
          "cubes": [ { "origin": [-3.9, 0, -2], "size": [4, 12, 4], "uv": [0, 16] } ]
        },
        {
          "name": "leftLeg",
          "parent": "waist",
          "pivot": [-1.9, 12, 0],
          "cubes": [ { "origin": [-0.1, 0, -2], "size": [4, 12, 4], "uv": [16, 48] } ]
        },
        {
          "name": "wingRight",
          "parent": "body",
          "pivot": [4, 20, -2],
          "rotation": [0.0, 0.0, 0.0],
          "cubes": [ { "origin": [2, 12, -3], "size": [6, 10, 1], "uv": [48, 16] } ]
        },
        {
          "name": "wingLeft",
          "parent": "body",
          "pivot": [-4, 20, -2],
          "rotation": [0.0, 0.0, 0.0],
          "cubes": [ { "origin": [-8, 12, -3], "size": [6, 10, 1], "uv": [48, 32] } ]
        }
      ]
    }
  ]
}'''


# ---------------------------------------------------------------------------
# 64x64 skin texture generation.
# ---------------------------------------------------------------------------

def set_px(data: bytearray, x: int, y: int, rgba: tuple[int, int, int, int]) -> None:
    i = (y * TEX_W + x) * 4
    data[i:i + 4] = bytes(rgba)


def fill_rect(data: bytearray, x: int, y: int, w: int, h: int,
              rgba: tuple[int, int, int, int]) -> None:
    for yy in range(y, y + h):
        for xx in range(x, x + w):
            set_px(data, xx, yy, rgba)


def paint_cube(data: bytearray, u: int, v: int, sx: int, sy: int, sz: int,
               colors: dict[str, tuple[int, int, int, int]]) -> None:
    """Paints the box-unwrap faces of a cube whose top-left UV is (u,v).

    Face placement follows the Bedrock cube mapping:
      top    (u+sz, v)              (sx, sz)
      bottom (u+sz+sx, v)           (sx, sz)
      right  (u, v+sz)              (sz, sy)
      front  (u+sz, v+sz)           (sx, sy)
      left   (u+sz+sx, v+sz)        (sz, sy)
      back   (u+sz+sx+sz, v+sz)     (sx, sy)
    """
    fill_rect(data, u + sz, v, sx, sz, colors["top"])
    fill_rect(data, u + sz + sx, v, sx, sz, colors["bottom"])
    fill_rect(data, u, v + sz, sz, sy, colors["right"])
    fill_rect(data, u + sz, v + sz, sx, sy, colors["front"])
    fill_rect(data, u + sz + sx, v + sz, sz, sy, colors["left"])
    fill_rect(data, u + sz + sx + sz, v + sz, sx, sy, colors["back"])


HEAD = (224, 174, 131, 255)   # face / hands
HAIR = (94, 62, 36, 255)      # brown hair
SHIRT = (56, 160, 164, 255)   # teal shirt
PANTS = (70, 70, 148, 255)    # dark blue pants
SHOE = (48, 48, 52, 255)
WING = (18, 18, 24, 255)
WING_INSIDE = (28, 28, 36, 255)


def make_texture() -> bytearray:
    data = bytearray(TEX_W * TEX_H * 4)  # transparent by default

    # Head (8x8x8 @0,0). Hair on top/sides/back, skin face with a fringe,
    # eyes and a small mouth.
    paint_cube(data, 0, 0, 8, 8, 8, {
        "top": HAIR, "bottom": HAIR, "right": HAIR, "left": HAIR,
        "front": HEAD, "back": HAIR,
    })
    fill_rect(data, 8, 8, 8, 2, HAIR)              # hair fringe on the face
    for x in (10, 13):                             # eyes
        set_px(data, x, 12, (40, 30, 26, 255))
    fill_rect(data, 10, 14, 4, 1, (140, 86, 60, 255))  # mouth

    # Body (8x12x4 @16,16). Teal shirt.
    paint_cube(data, 16, 16, 8, 12, 4, {
        "top": SHIRT, "bottom": SHIRT, "right": SHIRT, "left": SHIRT,
        "front": SHIRT, "back": (44, 120, 124, 255),
    })
    # Simple dark shirt stripe down the torso front.
    fill_rect(data, 22, 22, 4, 8, (52, 132, 136, 255))

    # Right arm (4x12x4 @40,16): teal sleeve up to the elbow, skin hand.
    arm = {"top": SHIRT, "bottom": SHIRT, "right": SHIRT, "left": SHIRT,
           "front": SHIRT, "back": SHIRT}
    paint_cube(data, 40, 16, 4, 12, 4, arm)
    for face_x in (44, 48, 52, 40):  # front/back/left/right visible columns
        fill_rect(data, face_x, 24, 4, 8, HEAD)
    fill_rect(data, 44, 16, 4, 4, SHIRT)   # sleeve top
    fill_rect(data, 48, 16, 4, 4, SHIRT)

    # Left arm (4x12x4 @32,48).
    paint_cube(data, 32, 48, 4, 12, 4, arm)
    for face_x in (36, 40, 44, 32):
        fill_rect(data, face_x, 56, 4, 8, HEAD)
    fill_rect(data, 36, 48, 4, 4, SHIRT)
    fill_rect(data, 40, 48, 4, 4, SHIRT)

    # Right leg (4x12x4 @0,16): dark pants + shoes.
    paint_cube(data, 0, 16, 4, 12, 4, {
        "top": PANTS, "bottom": PANTS, "right": PANTS, "left": PANTS,
        "front": PANTS, "back": PANTS,
    })
    for face_x in (4, 8, 12, 0):
        fill_rect(data, face_x, 28, 4, 4, SHOE)

    # Left leg (4x12x4 @16,48).
    paint_cube(data, 16, 48, 4, 12, 4, {
        "top": PANTS, "bottom": PANTS, "right": PANTS, "left": PANTS,
        "front": PANTS, "back": PANTS,
    })
    for face_x in (20, 24, 28, 16):
        fill_rect(data, face_x, 60, 4, 4, SHOE)

    # ------------------------------------------------------------------
    # Wings. Cube 6x8x1; the visible outer face is the box-unwrap "back"
    # face at (u+1+6+1, v+1) = (u+8, v+1) 6x8. We paint the bat silhouette
    # there on transparent ground and give the thin edges a solid colour.
    # ------------------------------------------------------------------
    wing_shape = [
        [0, 1, 1, 1, 1, 1],
        [1, 1, 1, 1, 1, 1],
        [1, 1, 1, 1, 1, 0],
        [1, 1, 1, 1, 0, 0],
        [1, 1, 1, 0, 1, 0],
        [1, 1, 0, 0, 1, 0],
        [1, 1, 0, 1, 1, 0],
        [1, 0, 0, 1, 0, 0],
        [1, 0, 1, 0, 0, 0],
        [0, 0, 1, 0, 0, 0],
    ]

    def paint_wing(u: int, v: int, mirror: bool) -> None:
        # Solid thin edges (top, bottom, both 1px sides).
        fill_rect(data, u + 1, v, 6, 1, WING)
        fill_rect(data, u + 7, v, 6, 1, WING)
        fill_rect(data, u, v + 1, 1, 10, WING)
        fill_rect(data, u + 6, v + 1, 1, 10, WING)
        # Inner (front-facing) side is a soft dark backing.
        fill_rect(data, u + 1, v + 1, 6, 10, WING_INSIDE)
        # Outer (back-facing) face keeps the transparent "bat" silhouette.
        bx = u + 8
        by = v + 1
        for row in range(10):
            for col in range(6):
                filled = wing_shape[row][col] if not mirror else wing_shape[row][5 - col]
                if filled:
                    set_px(data, bx + col, by + row, WING)

    paint_wing(48, 16, mirror=False)   # right wing
    paint_wing(48, 32, mirror=True)    # left wing

    return data


# ---------------------------------------------------------------------------
# Minimal PNG encoder (RGBA8, no external deps).
# ---------------------------------------------------------------------------

def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    def chunk(kind: bytes, payload: bytes) -> bytes:
        blob = kind + payload
        return struct.pack(">I", len(payload)) + blob + struct.pack(">I", zlib.crc32(blob) & 0xffffffff)

    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)  # filter type: none
        raw.extend(rgba[y * stride:(y + 1) * stride])

    payload = (b"\x89PNG\r\n\x1a\n"
               + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
               + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
               + chunk(b"IEND", b""))
    path.write_bytes(payload)


# ---------------------------------------------------------------------------
# C++ header embedding.
# ---------------------------------------------------------------------------

def cpp_bytes_array(data: bytes, per_line: int = 16) -> list[str]:
    lines: list[str] = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    return lines


def write_header(path: Path) -> None:
    texture = make_texture()
    lines: list[str] = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("// Auto-generated by scripts/gen_wings_assets.py - do not edit by hand.")
    lines.append("// Default Bedrock skin geometry + 64x64 RGBA texture for the Wings module.")
    lines.append("#include <cstddef>")
    lines.append("")
    lines.append("namespace wings_default {")
    lines.append("")
    lines.append("inline constexpr const char* GeometryIdentifier = \"" + IDENTIFIER + "\";")
    lines.append("")
    lines.append("inline constexpr const char* GeometryJson = R\"json(" + GEOMETRY + ")json\";")
    lines.append("")
    lines.append("inline constexpr std::size_t TextureWidth = " + str(TEX_W) + ";")
    lines.append("inline constexpr std::size_t TextureHeight = " + str(TEX_H) + ";")
    lines.append("")
    lines.append("inline constexpr unsigned char TexturePixels[] = {")
    lines.extend(cpp_bytes_array(bytes(texture)))
    lines.append("};")
    lines.append("")
    lines.append("} // namespace wings_default")
    lines.append("")
    path.write_text("\n".join(lines))


def main() -> int:
    RES.mkdir(parents=True, exist_ok=True)
    INC.mkdir(parents=True, exist_ok=True)

    (RES / "wings_geometry.json").write_text(GEOMETRY, encoding="utf-8")
    write_png(RES / "wings.png", TEX_W, TEX_H, bytes(make_texture()))
    write_header(INC / "wings_default.hpp")

    print(f"wrote {RES / 'wings_geometry.json'}")
    print(f"wrote {RES / 'wings.png'}")
    print(f"wrote {INC / 'wings_default.hpp'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

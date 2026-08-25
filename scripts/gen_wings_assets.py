#!/usr/bin/env python3
"""Generate the default Wings assets.

Produces five files:

* resources/wings/wings_geometry.json - a Minecraft Bedrock geometry
  (identifier "geometry.wings") that keeps the standard player bones and
  adds a fully articulated 3D wing hierarchy on the back:

      bone_wings
      +-- bone_wing_right      (shoulder joint box)
      |   +-- bone_wing_right_upper
      |   |   +-- bone_wing_right_feather_1
      |   |   +-- bone_wing_right_feather_2
      |   |   +-- bone_wing_right_tip
      |   |       +-- bone_wing_right_feather_3
      |   |       +-- bone_wing_right_feather_4
      +-- bone_wing_left       ("mirror": true, same UV regions)
          +-- ... same children, mirrored

  Every wing part is a real 3D box (thickness in Z), not a flat quad.
  Coordinates follow the same convention as the vanilla humanoid bones
  already in this file: right side = negative X, back (cape side) = -Z.

* resources/wings/wings_animation.json - Bedrock animations for the new
  bones: "animation.wings.idle", "animation.wings.flap" and
  "animation.wings.glide" (looping, molang driven).

* resources/wings/wings_animation_controllers.json - an animation
  controller ("controller.animation.wings") that blends idle/flap/glide
  based on the player's movement
  (query.modified_move_speed / query.vertical_speed / query.is_gliding).

* resources/wings/wings.png - the matching 64x64 RGBA skin texture. The
  standard body layout is untouched; the wings use the free UV band
  (x 0..47, y 32..47) with per-face art: dark outer membrane, lighter
  inner membrane, brown frame/bone edges and highlighted feather tips,
  so the 3D model shows outer and inner details correctly.

* include/bedrocktools/modules/visual/wings_default.hpp - the same
  assets embedded as code (geometry + animations + controller + texture)
  so the WingsModule works immediately after install and can write the
  pack files next to config.json even when no external files exist.

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
# Wing palette. These exact RGB values are painted into wings.png and are
# also embedded in the generated header so the C++ world-space renderer can
# colour each box face the same way the texture maps onto the geometry
# (tests cross-check the two copies to keep them in sync).
# ---------------------------------------------------------------------------
FRAME = (94, 62, 36)        # brown frame / bones / edges
MEMBRANE_OUTER = (18, 18, 24)    # dark outer membrane (back of the wing)
MEMBRANE_INNER = (28, 28, 36)    # softer inner membrane (faces the body)
FEATHER_TIP = (46, 46, 60)       # highlighted feather tips
JOINT_INNER = (76, 52, 32)       # lighter brown for the shoulder's inner face


def shade(c: tuple[int, int, int], f: float) -> tuple[int, int, int]:
    return (min(255, int(c[0] * f)), min(255, int(c[1] * f)), min(255, int(c[2] * f)))


# ---------------------------------------------------------------------------
# Bedrock geometry. Standard humanoid bones keep the default player body
# rendering and animating; the wing hierarchy replaces the old flat
# 6x10x1 quads with articulated 3D boxes.
#
# All sizes are integers (Bedrock-safe); pivots sit exactly on the joints
# so animation rotations bend the wing like a real limb.
#
# UV regions (64x64):
#   head  (0,0) 8x8x8      body (16,16) 8x12x4
#   right arm (40,16)      left arm (32,48)
#   right leg (0,16)       left leg (16,48)
#   shoulder 3x3x2  (0, 32)   upper 6x3x1 (10, 32)   tip 5x2x1 (24, 32)
#   feathers 2x6x1 (0,38) (6,38) (12,38)   feather 2x5x1 (18,38)
# Left-side bones reuse the same regions with "mirror": true.
# ---------------------------------------------------------------------------
GEOMETRY = r'''{
  "format_version": "1.12.0",
  "minecraft:geometry": [
    {
      "description": {
        "identifier": "geometry.wings",
        "texture_width": 64,
        "texture_height": 64,
        "visible_bounds_width": 4,
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
        { "name": "bone_wings", "parent": "body", "pivot": [0, 24, 0] },
        {
          "name": "bone_wing_right",
          "parent": "bone_wings",
          "pivot": [-3, 21, -2],
          "rotation": [0.0, 0.0, 0.0],
          "cubes": [ { "origin": [-4.5, 19.5, -4], "size": [3, 3, 2], "uv": [0, 32] } ]
        },
        {
          "name": "bone_wing_right_upper",
          "parent": "bone_wing_right",
          "pivot": [-5, 21, -3],
          "rotation": [0.0, 0.0, 0.0],
          "cubes": [ { "origin": [-11, 19.5, -3.5], "size": [6, 3, 1], "uv": [10, 32] } ]
        },
        {
          "name": "bone_wing_right_feather_1",
          "parent": "bone_wing_right_upper",
          "pivot": [-7, 19.5, -3],
          "rotation": [0.0, 0.0, 0.0],
          "cubes": [ { "origin": [-8, 13.5, -3.5], "size": [2, 6, 1], "uv": [0, 38] } ]
        },
        {
          "name": "bone_wing_right_feather_2",
          "parent": "bone_wing_right_upper",
          "pivot": [-9.5, 19.5, -3],
          "rotation": [0.0, 0.0, 0.0],
          "cubes": [ { "origin": [-10.5, 13.5, -3.5], "size": [2, 6, 1], "uv": [6, 38] } ]
        },
        {
          "name": "bone_wing_right_tip",
          "parent": "bone_wing_right_upper",
          "pivot": [-11, 21, -3],
          "rotation": [0.0, 0.0, 0.0],
          "cubes": [ { "origin": [-16, 20, -3.5], "size": [5, 2, 1], "uv": [24, 32] } ]
        },
        {
          "name": "bone_wing_right_feather_3",
          "parent": "bone_wing_right_tip",
          "pivot": [-12.5, 20, -3],
          "rotation": [0.0, 0.0, 0.0],
          "cubes": [ { "origin": [-13.5, 14, -3.5], "size": [2, 6, 1], "uv": [12, 38] } ]
        },
        {
          "name": "bone_wing_right_feather_4",
          "parent": "bone_wing_right_tip",
          "pivot": [-15, 20, -3],
          "rotation": [0.0, 0.0, 0.0],
          "cubes": [ { "origin": [-16, 15, -3.5], "size": [2, 5, 1], "uv": [18, 38] } ]
        },
        {
          "name": "bone_wing_left",
          "parent": "bone_wings",
          "pivot": [3, 21, -2],
          "rotation": [0.0, 0.0, 0.0],
          "mirror": true,
          "cubes": [ { "origin": [1.5, 19.5, -4], "size": [3, 3, 2], "uv": [0, 32] } ]
        },
        {
          "name": "bone_wing_left_upper",
          "parent": "bone_wing_left",
          "pivot": [5, 21, -3],
          "rotation": [0.0, 0.0, 0.0],
          "mirror": true,
          "cubes": [ { "origin": [5, 19.5, -3.5], "size": [6, 3, 1], "uv": [10, 32] } ]
        },
        {
          "name": "bone_wing_left_feather_1",
          "parent": "bone_wing_left_upper",
          "pivot": [7, 19.5, -3],
          "rotation": [0.0, 0.0, 0.0],
          "mirror": true,
          "cubes": [ { "origin": [6, 13.5, -3.5], "size": [2, 6, 1], "uv": [0, 38] } ]
        },
        {
          "name": "bone_wing_left_feather_2",
          "parent": "bone_wing_left_upper",
          "pivot": [9.5, 19.5, -3],
          "rotation": [0.0, 0.0, 0.0],
          "mirror": true,
          "cubes": [ { "origin": [8.5, 13.5, -3.5], "size": [2, 6, 1], "uv": [6, 38] } ]
        },
        {
          "name": "bone_wing_left_tip",
          "parent": "bone_wing_left_upper",
          "pivot": [11, 21, -3],
          "rotation": [0.0, 0.0, 0.0],
          "mirror": true,
          "cubes": [ { "origin": [11, 20, -3.5], "size": [5, 2, 1], "uv": [24, 32] } ]
        },
        {
          "name": "bone_wing_left_feather_3",
          "parent": "bone_wing_left_tip",
          "pivot": [12.5, 20, -3],
          "rotation": [0.0, 0.0, 0.0],
          "mirror": true,
          "cubes": [ { "origin": [11.5, 14, -3.5], "size": [2, 6, 1], "uv": [12, 38] } ]
        },
        {
          "name": "bone_wing_left_feather_4",
          "parent": "bone_wing_left_tip",
          "pivot": [15, 20, -3],
          "rotation": [0.0, 0.0, 0.0],
          "mirror": true,
          "cubes": [ { "origin": [14, 15, -3.5], "size": [2, 5, 1], "uv": [18, 38] } ]
        }
      ]
    }
  ]
}'''


# ---------------------------------------------------------------------------
# Bedrock animations for the new bone hierarchy.
#
# Positive Z-roll lifts a wing whose span runs along +X (left side), so
# right-side bones use the negated expression of the lift angle.
# math.sin/math.cos take degrees. Lag offsets (upper 50, tip 105,
# feathers 140+) make the wave travel from the shoulder out to the
# feather tips, exactly like the runtime renderer in wings.cpp (which uses
# the same constants) - keep the two in sync.
# ---------------------------------------------------------------------------
ANIMATION = r'''{
  "format_version": "1.8.0",
  "animations": {
    "animation.wings.idle": {
      "loop": true,
      "animation_length": 4.0,
      "bones": {
        "bone_wings": {
          "rotation": ["1.5 * math.sin(query.anim_time * 90)", 0, 0]
        },
        "bone_wing_right": {
          "rotation": [0, 0, "-(20 + 6 * math.sin(query.anim_time * 90))"]
        },
        "bone_wing_left": {
          "rotation": [0, 0, "20 + 6 * math.sin(query.anim_time * 90)"]
        },
        "bone_wing_right_upper": {
          "rotation": [0, 0, "-(6 + 4 * math.sin(query.anim_time * 90 - 45))"]
        },
        "bone_wing_left_upper": {
          "rotation": [0, 0, "6 + 4 * math.sin(query.anim_time * 90 - 45)"]
        },
        "bone_wing_right_tip": {
          "rotation": [0, 0, "-(8 + 3 * math.sin(query.anim_time * 90 - 90))"]
        },
        "bone_wing_left_tip": {
          "rotation": [0, 0, "8 + 3 * math.sin(query.anim_time * 90 - 90)"]
        },
        "bone_wing_right_feather_1": { "rotation": [0, 0, "-(2.5 * math.sin(query.anim_time * 90 - 120))"] },
        "bone_wing_right_feather_2": { "rotation": [0, 0, "-(2.5 * math.sin(query.anim_time * 90 - 135))"] },
        "bone_wing_right_feather_3": { "rotation": [0, 0, "-(2.5 * math.sin(query.anim_time * 90 - 150))"] },
        "bone_wing_right_feather_4": { "rotation": [0, 0, "-(2.5 * math.sin(query.anim_time * 90 - 165))"] },
        "bone_wing_left_feather_1": { "rotation": [0, 0, "2.5 * math.sin(query.anim_time * 90 - 120)"] },
        "bone_wing_left_feather_2": { "rotation": [0, 0, "2.5 * math.sin(query.anim_time * 90 - 135)"] },
        "bone_wing_left_feather_3": { "rotation": [0, 0, "2.5 * math.sin(query.anim_time * 90 - 150)"] },
        "bone_wing_left_feather_4": { "rotation": [0, 0, "2.5 * math.sin(query.anim_time * 90 - 165)"] }
      }
    },
    "animation.wings.flap": {
      "loop": true,
      "animation_length": 1.0,
      "bones": {
        "bone_wings": {
          "rotation": ["4 * math.sin(query.anim_time * 360 - 30)", 0, 0]
        },
        "bone_wing_right": {
          "rotation": ["6 * math.sin(query.anim_time * 360 - 20)", 0, "-(25 + 35 * math.sin(query.anim_time * 360))"]
        },
        "bone_wing_left": {
          "rotation": ["6 * math.sin(query.anim_time * 360 - 20)", 0, "25 + 35 * math.sin(query.anim_time * 360)"]
        },
        "bone_wing_right_upper": {
          "rotation": [0, 0, "-(14 * math.sin(query.anim_time * 360 - 50))"]
        },
        "bone_wing_left_upper": {
          "rotation": [0, 0, "14 * math.sin(query.anim_time * 360 - 50)"]
        },
        "bone_wing_right_tip": {
          "rotation": [0, 0, "-(18 * math.sin(query.anim_time * 360 - 105))"]
        },
        "bone_wing_left_tip": {
          "rotation": [0, 0, "18 * math.sin(query.anim_time * 360 - 105)"]
        },
        "bone_wing_right_feather_1": { "rotation": [0, 0, "-(10 * math.sin(query.anim_time * 360 - 140))"] },
        "bone_wing_right_feather_2": { "rotation": [0, 0, "-(10 * math.sin(query.anim_time * 360 - 160))"] },
        "bone_wing_right_feather_3": { "rotation": [0, 0, "-(10 * math.sin(query.anim_time * 360 - 180))"] },
        "bone_wing_right_feather_4": { "rotation": [0, 0, "-(10 * math.sin(query.anim_time * 360 - 200))"] },
        "bone_wing_left_feather_1": { "rotation": [0, 0, "10 * math.sin(query.anim_time * 360 - 140)"] },
        "bone_wing_left_feather_2": { "rotation": [0, 0, "10 * math.sin(query.anim_time * 360 - 160)"] },
        "bone_wing_left_feather_3": { "rotation": [0, 0, "10 * math.sin(query.anim_time * 360 - 180)"] },
        "bone_wing_left_feather_4": { "rotation": [0, 0, "10 * math.sin(query.anim_time * 360 - 200)"] }
      }
    },
    "animation.wings.glide": {
      "loop": true,
      "animation_length": 3.0,
      "bones": {
        "bone_wings": {
          "rotation": ["2 * math.sin(query.anim_time * 120)", 0, 0]
        },
        "bone_wing_right": {
          "rotation": ["3 * math.sin(query.anim_time * 120)", 0, "-(50 + 3 * math.sin(query.anim_time * 120))"]
        },
        "bone_wing_left": {
          "rotation": ["3 * math.sin(query.anim_time * 120)", 0, "50 + 3 * math.sin(query.anim_time * 120)"]
        },
        "bone_wing_right_upper": {
          "rotation": [0, 0, "15 - 3 * math.sin(query.anim_time * 120 - 30)"]
        },
        "bone_wing_left_upper": {
          "rotation": [0, 0, "-(15 - 3 * math.sin(query.anim_time * 120 - 30))"]
        },
        "bone_wing_right_tip": {
          "rotation": [0, 0, "15 - 2 * math.sin(query.anim_time * 120 - 60)"]
        },
        "bone_wing_left_tip": {
          "rotation": [0, 0, "-(15 - 2 * math.sin(query.anim_time * 120 - 60))"]
        },
        "bone_wing_right_feather_1": { "rotation": [0, 0, "-(4 + 2 * math.sin(query.anim_time * 120 - 90))"] },
        "bone_wing_right_feather_2": { "rotation": [0, 0, "-(4 + 2 * math.sin(query.anim_time * 120 - 105))"] },
        "bone_wing_right_feather_3": { "rotation": [0, 0, "-(4 + 2 * math.sin(query.anim_time * 120 - 120))"] },
        "bone_wing_right_feather_4": { "rotation": [0, 0, "-(4 + 2 * math.sin(query.anim_time * 120 - 135))"] },
        "bone_wing_left_feather_1": { "rotation": [0, 0, "4 + 2 * math.sin(query.anim_time * 120 - 90)"] },
        "bone_wing_left_feather_2": { "rotation": [0, 0, "4 + 2 * math.sin(query.anim_time * 120 - 105)"] },
        "bone_wing_left_feather_3": { "rotation": [0, 0, "4 + 2 * math.sin(query.anim_time * 120 - 120)"] },
        "bone_wing_left_feather_4": { "rotation": [0, 0, "4 + 2 * math.sin(query.anim_time * 120 - 135)"] }
      }
    }
  }
}'''


# ---------------------------------------------------------------------------
# Animation controller: picks idle / flap / glide from the player's actual
# movement state (speed and height loss), mirroring the velocity-driven
# blending the native module does in wings.cpp.
# ---------------------------------------------------------------------------
ANIMATION_CONTROLLERS = r'''{
  "format_version": "1.10.0",
  "animation_controllers": {
    "controller.animation.wings": {
      "initial_state": "idle",
      "states": {
        "idle": {
          "animations": ["wings.idle"],
          "transitions": [
            { "glide": "query.is_gliding" },
            { "fly": "query.modified_move_speed > 0.18 || query.vertical_speed < -0.2" }
          ]
        },
        "fly": {
          "animations": ["wings.flap"],
          "transitions": [
            { "glide": "query.is_gliding" },
            { "idle": "query.modified_move_speed <= 0.18 && query.is_on_ground" }
          ]
        },
        "glide": {
          "animations": ["wings.glide"],
          "transitions": [
            { "fly": "!query.is_gliding && query.modified_move_speed > 0.18" },
            { "idle": "!query.is_gliding && query.is_on_ground" }
          ]
        }
      }
    }
  }
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


def vline(data: bytearray, x: int, y: int, h: int, rgba: tuple[int, int, int, int]) -> None:
    for yy in range(y, y + h):
        set_px(data, x, yy, rgba)


def paint_cube(data: bytearray, u: int, v: int, sx: int, sy: int, sz: int,
               colors: dict[str, tuple[int, int, int, int]]) -> None:
    """Paints the box-unwrap faces of a cube whose top-left UV is (u,v).

    Face placement follows the Bedrock cube mapping:
      top    (u+sz, v)              (sx, sz)
      bottom (u+sz+sx, v)           (sx, sz)
      right  (u, v+sz)              (sz, sy)
      front  (u+sz, v+sz)           (sx, sy)   <- faces the body (inner)
      left   (u+sz+sx, v+sz)        (sz, sy)
      back   (u+sz+sx+sz, v+sz)     (sx, sy)   <- faces away (outer)
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

A = 255
FRAME_A = FRAME + (A,)
OUTER_A = MEMBRANE_OUTER + (A,)
INNER_A = MEMBRANE_INNER + (A,)
TIP_A = FEATHER_TIP + (A,)


def paint_wing_cube(data: bytearray, u: int, v: int, sx: int, sy: int, sz: int,
                    fingers: list[float]) -> None:
    """Paints a membrane segment (upper arm / tip): dark outer face with
    brown 'finger' bone stripes, lighter inner face, brown thin edges.

    fingers: stripe positions as fractions of the face width (0..1).
    """
    paint_cube(data, u, v, sx, sy, sz, {
        "top": FRAME_A, "bottom": FRAME_A, "right": FRAME_A, "left": FRAME_A,
        "front": INNER_A, "back": OUTER_A,
    })
    outer_x = u + sz + sx + sz
    inner_x = u + sz
    face_y = v + sz
    # Frame border rows on the big faces.
    fill_rect(data, outer_x, face_y, sx, 1, FRAME_A)
    fill_rect(data, inner_x, face_y, sx, 1, FRAME_A)
    for f in fingers:
        stripe_x = outer_x + min(sx - 1, max(0, int(round(f * (sx - 1)))))
        vline(data, stripe_x, face_y, sy, FRAME_A)


def paint_feather(data: bytearray, u: int, v: int, sy: int, light: float) -> None:
    """Paints one feather cube (2 x sy x 1): dark outer membrane with a brown
    rib, lighter back edge, brighter lower band (feather tip highlight).
    """
    outer = shade(MEMBRANE_OUTER, light) + (A,)
    inner = shade(MEMBRANE_INNER, light) + (A,)
    tip = shade(FEATHER_TIP, light) + (A,)
    paint_cube(data, u, v, 2, sy, 1, {
        "top": FRAME_A, "bottom": tip, "right": FRAME_A, "left": FRAME_A,
        "front": inner, "back": outer,
    })
    outer_x = u + 1 + 2 + 1
    inner_x = u + 1
    face_y = v + 1
    # Frame row at the quill and the tip highlight band on both faces.
    fill_rect(data, outer_x, face_y, 2, 1, FRAME_A)
    fill_rect(data, outer_x, face_y + sy - 2, 2, 2, tip)
    fill_rect(data, inner_x, face_y + sy - 2, 2, 2, tip)


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
    # Articulated 3D wings. UV band x 0..47, y 32..47 (kept free above).
    #   shoulder 3x3x2 (0,32) - mostly frame/bone, membrane patch on faces
    #   upper 6x3x1 (10,32)   - membrane with 2 finger stripes
    #   tip 5x2x1 (24,32)     - membrane with 1 finger stripe
    #   feathers 2x6x1 (0,38) (6,38) (12,38), 2x5x1 (18,38)
    # Both wings share these regions (left bones are "mirror": true).
    # ------------------------------------------------------------------
    # Shoulder joint box. Outer (back) face is at (u + 2*sz + sx, v + sz).
    paint_cube(data, 0, 32, 3, 3, 2, {
        "top": FRAME_A, "bottom": FRAME_A, "right": FRAME_A, "left": FRAME_A,
        "front": JOINT_INNER + (A,), "back": FRAME_A,
    })
    fill_rect(data, 0 + 2 * 2 + 3, 32 + 2 + 1, 3, 1, shade(FRAME, 0.75) + (A,))

    # Upper segment: two "finger" bone stripes at 1/3 and 2/3 of the span.
    paint_wing_cube(data, 10, 32, 6, 3, 1, fingers=[1.0 / 3.0, 2.0 / 3.0])

    # Tip segment: one stripe near the outer end.
    paint_wing_cube(data, 24, 32, 5, 2, 1, fingers=[0.75])

    # Feathers: identical except the outermost one is slightly lighter so
    # the wing silhouette reads with depth.
    paint_feather(data, 0, 38, 6, 1.0)
    paint_feather(data, 6, 38, 6, 0.96)
    paint_feather(data, 12, 38, 6, 0.92)
    paint_feather(data, 18, 38, 5, 1.06)

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


def cpp_color(name: str, c: tuple[int, int, int]) -> str:
    return ("inline constexpr unsigned char " + name + "[3] = { "
            + str(c[0]) + ", " + str(c[1]) + ", " + str(c[2]) + " };")


def write_header(path: Path) -> None:
    texture = make_texture()
    lines: list[str] = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("// Auto-generated by scripts/gen_wings_assets.py - do not edit by hand.")
    lines.append("// Articulated 3D wings for the Wings module: Bedrock geometry +")
    lines.append("// wing animations + animation controller + 64x64 RGBA texture.")
    lines.append("#include <cstddef>")
    lines.append("")
    lines.append("namespace wings_default {")
    lines.append("")
    lines.append("inline constexpr const char* GeometryIdentifier = \"" + IDENTIFIER + "\";")
    lines.append("")
    lines.append("inline constexpr const char* GeometryJson = R\"json(" + GEOMETRY + ")json\";")
    lines.append("")
    lines.append("inline constexpr const char* AnimationJson = R\"json(" + ANIMATION + ")json\";")
    lines.append("")
    lines.append("inline constexpr const char* AnimationControllerJson = R\"json(" + ANIMATION_CONTROLLERS + ")json\";")
    lines.append("")
    lines.append("// Wing paint palette baked into the texture. The world-space renderer")
    lines.append("// colours each 3D wing box face with these same values (see wings.cpp),")
    lines.append("// so the overlay matches what the texture maps onto the geometry.")
    lines.append(cpp_color("kColorFrame", FRAME))
    lines.append(cpp_color("kColorMembraneOuter", MEMBRANE_OUTER))
    lines.append(cpp_color("kColorMembraneInner", MEMBRANE_INNER))
    lines.append(cpp_color("kColorFeatherTip", FEATHER_TIP))
    lines.append(cpp_color("kColorJointInner", JOINT_INNER))
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
    (RES / "wings_animation.json").write_text(ANIMATION, encoding="utf-8")
    (RES / "wings_animation_controllers.json").write_text(ANIMATION_CONTROLLERS, encoding="utf-8")
    write_png(RES / "wings.png", TEX_W, TEX_H, bytes(make_texture()))
    write_header(INC / "wings_default.hpp")

    print(f"wrote {RES / 'wings_geometry.json'}")
    print(f"wrote {RES / 'wings_animation.json'}")
    print(f"wrote {RES / 'wings_animation_controllers.json'}")
    print(f"wrote {RES / 'wings.png'}")
    print(f"wrote {INC / 'wings_default.hpp'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

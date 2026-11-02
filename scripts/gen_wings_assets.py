#!/usr/bin/env python3
"""Generate the default Wings assets - Demon Wings / Vampire Bat Wings edition.

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

* resources/wings/wings_animation.json
* resources/wings/wings_animation_controllers.json
* resources/wings/wings.png - 64x64 RGBA texture with Demon Wings spec:
  - Frame/Bones: solid black #000000
  - Membrane: dramatic gradient from glowing red #FF0000/#E60000 in middle
    and edges to dark bloody #800000 at bone contact
  - Glow effect: red crimson aura saturated between black separators
  - Webbed bat wing pattern with serrated lower edges

* include/bedrocktools/modules/visual/wings_default.hpp

Run:
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
# Demon Wings / Vampire Bat Wings palette - per user spec
# ---------------------------------------------------------------------------
# Frame/Bones: black #000000
# Membrane: gradient from #FF0000/#E60000 glowing to #800000 dark bloody
# ---------------------------------------------------------------------------
FRAME = (0, 0, 0)              # #000000 black - bones/frame
MEMBRANE_OUTER = (230, 0, 0)    # #E60000 glowing red - outer membrane bright
MEMBRANE_INNER = (128, 0, 0)    # #800000 dark bloody - near bones
FEATHER_TIP = (255, 0, 0)       # #FF0000 bright tip / glow
JOINT_INNER = (0, 0, 0)         # black joint

# Additional gradient colors for dramatic effect
GLOW_BRIGHT = (255, 0, 0)       # #FF0000 center glow
GLOW_MID = (230, 0, 0)          # #E60000 mid
GLOW_DARK = (128, 0, 0)         # #800000 near bones
GLOW_EDGE = (255, 26, 26)       # slightly brighter edge for aura


def lerp(a: int, b: int, t: float) -> int:
    return int(round(a + (b - a) * t))


def lerp_color(c1: tuple[int, int, int], c2: tuple[int, int, int], t: float) -> tuple[int, int, int]:
    t = max(0.0, min(1.0, t))
    return (lerp(c1[0], c2[0], t), lerp(c1[1], c2[1], t), lerp(c1[2], c2[2], t))


def shade(c: tuple[int, int, int], f: float) -> tuple[int, int, int]:
    return (min(255, int(c[0] * f)), min(255, int(c[1] * f)), min(255, int(c[2] * f)))


# ---------------------------------------------------------------------------
# Bedrock geometry - same as before, but wings now represent bat/demon wings
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
          "pivot": [-3, 21, 2.5],
          "rotation": [0.0, 0.0, 0.0],
          "cubes": [ { "origin": [-4.5, 19.5, 2.5], "size": [3, 3, 2], "uv": [0, 32] } ]
        },
        {
          "name": "bone_wing_right_upper",
          "parent": "bone_wing_right",
          "pivot": [-5, 21, 3.5],
          "rotation": [0.0, 0.0, 0.0],
          "cubes": [ { "origin": [-11, 19.5, 3], "size": [6, 3, 1], "uv": [10, 32] } ]
        },
        {
          "name": "bone_wing_right_feather_1",
          "parent": "bone_wing_right_upper",
          "pivot": [-7, 19.5, 3.5],
          "rotation": [0.0, 0.0, 0.0],
          "cubes": [ { "origin": [-8, 13.5, 3], "size": [2, 6, 1], "uv": [0, 38] } ]
        },
        {
          "name": "bone_wing_right_feather_2",
          "parent": "bone_wing_right_upper",
          "pivot": [-9.5, 19.5, 3.5],
          "rotation": [0.0, 0.0, 0.0],
          "cubes": [ { "origin": [-10.5, 13.5, 3], "size": [2, 6, 1], "uv": [6, 38] } ]
        },
        {
          "name": "bone_wing_right_tip",
          "parent": "bone_wing_right_upper",
          "pivot": [-11, 21, 3.5],
          "rotation": [0.0, 0.0, 0.0],
          "cubes": [ { "origin": [-16, 20, 3], "size": [5, 2, 1], "uv": [24, 32] } ]
        },
        {
          "name": "bone_wing_right_feather_3",
          "parent": "bone_wing_right_tip",
          "pivot": [-12.5, 20, 3.5],
          "rotation": [0.0, 0.0, 0.0],
          "cubes": [ { "origin": [-13.5, 14, 3], "size": [2, 6, 1], "uv": [12, 38] } ]
        },
        {
          "name": "bone_wing_right_feather_4",
          "parent": "bone_wing_right_tip",
          "pivot": [-15, 20, 3.5],
          "rotation": [0.0, 0.0, 0.0],
          "cubes": [ { "origin": [-16, 15, 3], "size": [2, 5, 1], "uv": [18, 38] } ]
        },
        {
          "name": "bone_wing_left",
          "parent": "bone_wings",
          "pivot": [3, 21, 2.5],
          "rotation": [0.0, 0.0, 0.0],
          "mirror": true,
          "cubes": [ { "origin": [1.5, 19.5, 2.5], "size": [3, 3, 2], "uv": [0, 32] } ]
        },
        {
          "name": "bone_wing_left_upper",
          "parent": "bone_wing_left",
          "pivot": [5, 21, 3.5],
          "rotation": [0.0, 0.0, 0.0],
          "mirror": true,
          "cubes": [ { "origin": [5, 19.5, 3], "size": [6, 3, 1], "uv": [10, 32] } ]
        },
        {
          "name": "bone_wing_left_feather_1",
          "parent": "bone_wing_left_upper",
          "pivot": [7, 19.5, 3.5],
          "rotation": [0.0, 0.0, 0.0],
          "mirror": true,
          "cubes": [ { "origin": [6, 13.5, 3], "size": [2, 6, 1], "uv": [0, 38] } ]
        },
        {
          "name": "bone_wing_left_feather_2",
          "parent": "bone_wing_left_upper",
          "pivot": [9.5, 19.5, 3.5],
          "rotation": [0.0, 0.0, 0.0],
          "mirror": true,
          "cubes": [ { "origin": [8.5, 13.5, 3], "size": [2, 6, 1], "uv": [6, 38] } ]
        },
        {
          "name": "bone_wing_left_tip",
          "parent": "bone_wing_left_upper",
          "pivot": [11, 21, 3.5],
          "rotation": [0.0, 0.0, 0.0],
          "mirror": true,
          "cubes": [ { "origin": [11, 20, 3], "size": [5, 2, 1], "uv": [24, 32] } ]
        },
        {
          "name": "bone_wing_left_feather_3",
          "parent": "bone_wing_left_tip",
          "pivot": [12.5, 20, 3.5],
          "rotation": [0.0, 0.0, 0.0],
          "mirror": true,
          "cubes": [ { "origin": [11.5, 14, 3], "size": [2, 6, 1], "uv": [12, 38] } ]
        },
        {
          "name": "bone_wing_left_feather_4",
          "parent": "bone_wing_left_tip",
          "pivot": [15, 20, 3.5],
          "rotation": [0.0, 0.0, 0.0],
          "mirror": true,
          "cubes": [ { "origin": [14, 15, 3], "size": [2, 5, 1], "uv": [18, 38] } ]
        }
      ]
    }
  ]
}'''

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
# 64x64 skin texture generation - Demon Wings edition
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
    fill_rect(data, u + sz, v, sx, sz, colors["top"])
    fill_rect(data, u + sz + sx, v, sx, sz, colors["bottom"])
    fill_rect(data, u, v + sz, sz, sy, colors["right"])
    fill_rect(data, u + sz, v + sz, sx, sy, colors["front"])
    fill_rect(data, u + sz + sx, v + sz, sz, sy, colors["left"])
    fill_rect(data, u + sz + sx + sz, v + sz, sx, sy, colors["back"])


HEAD = (224, 174, 131, 255)
HAIR = (94, 62, 36, 255)
SHIRT = (56, 160, 164, 255)
PANTS = (70, 70, 148, 255)
SHOE = (48, 48, 52, 255)

A = 255
FRAME_A = FRAME + (A,)
OUTER_A = MEMBRANE_OUTER + (A,)
INNER_A = MEMBRANE_INNER + (A,)
TIP_A = FEATHER_TIP + (A,)
GLOW_BRIGHT_A = GLOW_BRIGHT + (A,)
GLOW_MID_A = GLOW_MID + (A,)
GLOW_DARK_A = GLOW_DARK + (A,)


def paint_wing_cube_demon(data: bytearray, u: int, v: int, sx: int, sy: int, sz: int,
                          fingers: list[float]) -> None:
    """
    Demon Wings membrane: black frame, red glowing membrane with gradient
    - Outer face (back): gradient from dark #800000 near bones to bright #FF0000 in middle
    - Inner face (front): dark #800000 with glow
    - Top/bottom/right/left: black #000000
    - Finger stripes: black prominent extended bones
    """
    # Base cube with black edges and red membranes
    paint_cube(data, u, v, sx, sy, sz, {
        "top": FRAME_A, "bottom": FRAME_A, "right": FRAME_A, "left": FRAME_A,
        "front": INNER_A, "back": OUTER_A,
    })
    outer_x = u + sz + sx + sz
    inner_x = u + sz
    face_y = v + sz

    # Frame border row - black (only top, bottom kept as membrane for small sy to show glow)
    fill_rect(data, outer_x, face_y, sx, 1, FRAME_A)
    fill_rect(data, inner_x, face_y, sx, 1, FRAME_A)
    if sy > 2:
        fill_rect(data, outer_x, face_y + sy - 1, sx, 1, FRAME_A)
        fill_rect(data, inner_x, face_y + sy - 1, sx, 1, FRAME_A)

    # Finger bone stripes - black prominent
    for f in fingers:
        stripe_x = outer_x + min(sx - 1, max(0, int(round(f * (sx - 1)))))
        vline(data, stripe_x, face_y, sy, FRAME_A)
        # Also on inner face
        vline(data, inner_x + (stripe_x - outer_x), face_y, sy, FRAME_A)

    # Now apply gradient to outer membrane (back face) - dramatic red glow
    # Gradient: dark near bones (frame and finger stripes) -> bright in middle
    # Compute bone positions including borders
    bone_xs = [0, sx - 1]
    for f in fingers:
        bone_xs.append(int(round(f * (sx - 1))))
    bone_xs = sorted(set(bone_xs))

    for yy in range(sy):
        for xx in range(sx):
            # Distance to nearest bone (frame border or finger)
            min_dist = min(abs(xx - bx) for bx in bone_xs)
            # Also distance to top/bottom border
            border_dist = min(yy, sy - 1 - yy)
            # Combined distance factor
            # Normalize by half segment width
            # Find segment width between bones
            # For gradient, use distance to nearest bone
            # t = 0 at bone (dark), 1 in middle (bright)
            # Assume max distance ~ sx / (len(bone_xs)) /2
            max_seg = sx / (len(bone_xs)) if len(bone_xs) > 1 else sx
            if max_seg < 1:
                max_seg = 1
            t = min_dist / (max_seg * 0.6)
            t = max(0.0, min(1.0, t))
            # Also factor in vertical border (serrated lower edge effect)
            v_t = border_dist / (sy * 0.5) if sy > 1 else 1.0
            v_t = max(0.0, min(1.0, v_t))
            # Combine: use min to keep dark near any bone
            combined_t = min(t, v_t)
            # Smoothstep for glow effect
            # Glow: dark #800000 at bone, bright #FF0000 in middle
            # Use power curve for more dramatic glow
            glow_t = combined_t ** 0.7
            # Outer membrane: gradient dark->bright
            r = lerp(GLOW_DARK[0], GLOW_BRIGHT[0], glow_t)
            g = lerp(GLOW_DARK[1], GLOW_BRIGHT[1], glow_t)
            b = lerp(GLOW_DARK[2], GLOW_BRIGHT[2], glow_t)
            # For inner membrane, darker overall but still gradient
            # We'll paint outer face here, inner face separately below
            set_px(data, outer_x + xx, face_y + yy, (r, g, b, 255))

            # Inner face: slightly darker gradient (dark bloody #800000 base, but with glow)
            inner_glow_t = glow_t * 0.8
            ir = lerp(GLOW_DARK[0], GLOW_MID[0], inner_glow_t)
            ig = lerp(GLOW_DARK[1], GLOW_MID[1], inner_glow_t)
            ib = lerp(GLOW_DARK[2], GLOW_MID[2], inner_glow_t)
            set_px(data, inner_x + xx, face_y + yy, (ir, ig, ib, 255))

    # Re-apply black bone stripes on top of gradient (prominent)
    for f in fingers:
        stripe_x = outer_x + min(sx - 1, max(0, int(round(f * (sx - 1)))))
        vline(data, stripe_x, face_y, sy, FRAME_A)
        vline(data, inner_x + (stripe_x - outer_x), face_y, sy, FRAME_A)
    fill_rect(data, outer_x, face_y, sx, 1, FRAME_A)
    fill_rect(data, inner_x, face_y, sx, 1, FRAME_A)
    if sy > 2:
        fill_rect(data, outer_x, face_y + sy - 1, sx, 1, FRAME_A)
        fill_rect(data, inner_x, face_y + sy - 1, sx, 1, FRAME_A)


def paint_feather_demon(data: bytearray, u: int, v: int, sy: int, light: float) -> None:
    """
    Demon bat finger / feather: black frame, red membrane with gradient
    and serrated lower edge effect.
    2 x sy x 1 cube
    """
    # Base cube
    outer_base = shade(MEMBRANE_OUTER, light)
    inner_base = shade(MEMBRANE_INNER, light)
    tip_base = shade(FEATHER_TIP, light)

    paint_cube(data, u, v, 2, sy, 1, {
        "top": FRAME_A, "bottom": (tip_base[0], tip_base[1], tip_base[2], 255),
        "right": FRAME_A, "left": FRAME_A,
        "front": (inner_base[0], inner_base[1], inner_base[2], 255),
        "back": (outer_base[0], outer_base[1], outer_base[2], 255),
    })
    outer_x = u + 1 + 2 + 1
    inner_x = u + 1
    face_y = v + 1

    # Gradient for finger membrane: dark at quill (top), bright in middle, bright tip at bottom
    # Serrated/wavy lower edge: create wave pattern in tip highlight
    for yy in range(sy):
        # Vertical gradient factor: 0 at top (near bone), 1 at bottom (tip)
        # But also dark near top bone, bright in middle
        # Use distance from top
        dist_top = yy
        # For serrated effect, vary tip edge
        # Gradient: dark #800000 at top, bright #FF0000 in middle, glowing edge at bottom
        if yy == 0:
            t = 0.0
        elif yy >= sy - 2:
            t = 1.0
        else:
            t = (yy / (sy - 1)) ** 0.8
            # Boost middle to be bright
            if t < 0.5:
                t = t * 1.5
                t = min(1.0, t)

        # Outer face gradient
        r = lerp(GLOW_DARK[0], GLOW_BRIGHT[0], t)
        g = lerp(GLOW_DARK[1], GLOW_BRIGHT[1], t)
        b = lerp(GLOW_DARK[2], GLOW_BRIGHT[2], t)

        # Inner face slightly darker
        ir = lerp(GLOW_DARK[0], GLOW_MID[0], t * 0.9)
        ig = lerp(GLOW_DARK[1], GLOW_MID[1], t * 0.9)
        ib = lerp(GLOW_DARK[2], GLOW_MID[2], t * 0.9)

        # Paint 2-pixel wide membrane
        for xx in range(2):
            # Horizontal gradient: dark near black bone edges (left/right), bright in center
            # For 2px wide, left edge near bone = darker, right edge = brighter? Actually both edges are frame?
            # For finger, left/right are frame, so middle of 2px is bright
            # We'll make xx=0 slightly darker, xx=1 brighter for depth
            h_t = 0.3 if xx == 0 else 0.9
            hr = lerp(r, GLOW_BRIGHT[0], h_t * 0.3)
            hg = lerp(g, GLOW_BRIGHT[1], h_t * 0.3)
            hb = lerp(b, GLOW_BRIGHT[2], h_t * 0.3)

            hir = lerp(ir, GLOW_MID[0], h_t * 0.2)
            hig = lerp(ig, GLOW_MID[1], h_t * 0.2)
            hib = lerp(ib, GLOW_MID[2], h_t * 0.2)

            set_px(data, outer_x + xx, face_y + yy, (hr, hg, hb, 255))
            set_px(data, inner_x + xx, face_y + yy, (hir, hig, hib, 255))

    # Frame row at quill (top) - black prominent
    fill_rect(data, outer_x, face_y, 2, 1, FRAME_A)
    fill_rect(data, inner_x, face_y, 2, 1, FRAME_A)

    # Tip highlight band - bright glowing red #FF0000 with serrated effect
    # Bottom 2 rows bright, but with wavy pattern
    for yy in range(sy - 2, sy):
        for xx in range(2):
            # Serrated: alternate brightness for wavy edge
            wave = 1.0 if (xx + yy) % 2 == 0 else 0.85
            r = int(tip_base[0] * wave)
            g = int(tip_base[1] * wave)
            b = int(tip_base[2] * wave)
            r = min(255, r)
            set_px(data, outer_x + xx, face_y + yy, (r, g, b, 255))
            set_px(data, inner_x + xx, face_y + yy, (r, g, b, 255))


def make_texture() -> bytearray:
    data = bytearray(TEX_W * TEX_H * 4)

    # Head
    paint_cube(data, 0, 0, 8, 8, 8, {
        "top": HAIR, "bottom": HAIR, "right": HAIR, "left": HAIR,
        "front": HEAD, "back": HAIR,
    })
    fill_rect(data, 8, 8, 8, 2, HAIR)
    for x in (10, 13):
        set_px(data, x, 12, (40, 30, 26, 255))
    fill_rect(data, 10, 14, 4, 1, (140, 86, 60, 255))

    # Body
    paint_cube(data, 16, 16, 8, 12, 4, {
        "top": SHIRT, "bottom": SHIRT, "right": SHIRT, "left": SHIRT,
        "front": SHIRT, "back": (44, 120, 124, 255),
    })
    fill_rect(data, 22, 22, 4, 8, (52, 132, 136, 255))

    # Arms
    arm = {"top": SHIRT, "bottom": SHIRT, "right": SHIRT, "left": SHIRT,
           "front": SHIRT, "back": SHIRT}
    paint_cube(data, 40, 16, 4, 12, 4, arm)
    for face_x in (44, 48, 52, 40):
        fill_rect(data, face_x, 24, 4, 8, HEAD)
    fill_rect(data, 44, 16, 4, 4, SHIRT)
    fill_rect(data, 48, 16, 4, 4, SHIRT)

    paint_cube(data, 32, 48, 4, 12, 4, arm)
    for face_x in (36, 40, 44, 32):
        fill_rect(data, face_x, 56, 4, 8, HEAD)
    fill_rect(data, 36, 48, 4, 4, SHIRT)
    fill_rect(data, 40, 48, 4, 4, SHIRT)

    # Legs
    paint_cube(data, 0, 16, 4, 12, 4, {
        "top": PANTS, "bottom": PANTS, "right": PANTS, "left": PANTS,
        "front": PANTS, "back": PANTS,
    })
    for face_x in (4, 8, 12, 0):
        fill_rect(data, face_x, 28, 4, 4, SHOE)

    paint_cube(data, 16, 48, 4, 12, 4, {
        "top": PANTS, "bottom": PANTS, "right": PANTS, "left": PANTS,
        "front": PANTS, "back": PANTS,
    })
    for face_x in (20, 24, 28, 16):
        fill_rect(data, face_x, 60, 4, 4, SHOE)

    # ------------------------------------------------------------------
    # Demon Wings / Vampire Bat Wings - UV band x 0..47, y 32..47
    #   shoulder 3x3x2 (0,32) - black frame, dark red inner
    #   upper 6x3x1 (10,32)   - black bones, red glowing membrane gradient
    #   tip 5x2x1 (24,32)     - black bones, red glow
    #   fingers 2x6x1 (0,38) (6,38) (12,38), 2x5x1 (18,38) - bat fingers
    # ------------------------------------------------------------------
    # Shoulder joint - mostly black frame, membrane patch with dark red
    paint_cube(data, 0, 32, 3, 3, 2, {
        "top": FRAME_A, "bottom": FRAME_A, "right": FRAME_A, "left": FRAME_A,
        "front": JOINT_INNER + (A,), "back": FRAME_A,
    })
    # Slight red glow in joint inner
    fill_rect(data, 0 + 2 * 2 + 3, 32 + 2 + 1, 3, 1, (64, 0, 0, 255))

    # Upper segment: black bones with red glowing membrane, 2 finger stripes
    paint_wing_cube_demon(data, 10, 32, 6, 3, 1, fingers=[1.0 / 3.0, 2.0 / 3.0])

    # Tip segment: black bones, red glow, 1 stripe near outer end
    paint_wing_cube_demon(data, 24, 32, 5, 2, 1, fingers=[0.75])

    # Bat fingers / membrane - serrated lower edges, black frame, red glow
    paint_feather_demon(data, 0, 38, 6, 1.0)
    paint_feather_demon(data, 6, 38, 6, 0.96)
    paint_feather_demon(data, 12, 38, 6, 0.92)
    paint_feather_demon(data, 18, 38, 5, 1.06)

    return data


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    def chunk(kind: bytes, payload: bytes) -> bytes:
        blob = kind + payload
        return struct.pack(">I", len(payload)) + blob + struct.pack(">I", zlib.crc32(blob) & 0xffffffff)

    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)
        raw.extend(rgba[y * stride:(y + 1) * stride])

    payload = (b"\x89PNG\r\n\x1a\n"
               + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
               + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
               + chunk(b"IEND", b""))
    path.write_bytes(payload)


def cpp_bytes_array(data: bytes, per_line: int = 16) -> list[str]:
    lines: list[str] = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    return lines


def cpp_color(name: str, c: tuple[int, int, int]) -> str:
    return (f"inline constexpr unsigned char {name}[3] = {{ {c[0]}, {c[1]}, {c[2]} }};")


def write_header(path: Path) -> None:
    texture = make_texture()
    lines: list[str] = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("// Auto-generated by scripts/gen_wings_assets.py - Demon Wings edition")
    lines.append("// Articulated 3D bat/demon wings: black frame #000000, red glowing membrane")
    lines.append("// Gradient #FF0000/#E60000 -> #800000 with crimson glow aura")
    lines.append("#include <cstddef>")
    lines.append("")
    lines.append("namespace wings_default {")
    lines.append("")
    lines.append(f"inline constexpr const char* GeometryIdentifier = \"{IDENTIFIER}\";")
    lines.append("")
    lines.append("inline constexpr const char* GeometryJson = R\"json(" + GEOMETRY + ")json\";")
    lines.append("")
    lines.append("inline constexpr const char* AnimationJson = R\"json(" + ANIMATION + ")json\";")
    lines.append("")
    lines.append("inline constexpr const char* AnimationControllerJson = R\"json(" + ANIMATION_CONTROLLERS + ")json\";")
    lines.append("")
    lines.append("// Demon Wings palette - black bones, red glowing membrane")
    lines.append("// Matches wings.png 64x64 with black frame and red gradient")
    lines.append(cpp_color("kColorFrame", FRAME))
    lines.append(cpp_color("kColorMembraneOuter", MEMBRANE_OUTER))
    lines.append(cpp_color("kColorMembraneInner", MEMBRANE_INNER))
    lines.append(cpp_color("kColorFeatherTip", FEATHER_TIP))
    lines.append(cpp_color("kColorJointInner", JOINT_INNER))
    lines.append("")
    lines.append(f"inline constexpr std::size_t TextureWidth = {TEX_W};")
    lines.append(f"inline constexpr std::size_t TextureHeight = {TEX_H};")
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

#!/usr/bin/env python3
"""Regenerates src/modules/visual/esp_pixel_font.hpp -- the 5x7 pixel face of the
game's own font (resources/minecraft.ttf), as the merged rectangles the Esp
module billboards nametags and the distance readout with in the level render pass.

Why a generator instead of a hand-drawn table: a world-space label has to be
*centered*, and a center is only knowable for a font whose cells are known
exactly. The only such font here is the one this package ships -- so its cells
are read out of the file rather than guessed (the same reason the HUD label
metrics in esp_geometry.hpp were measured out of that TTF, and the same trick
scripts/gen_effect_translations.py uses for the font's coverage flags). The
game's glyphs are orthogonal polygons on a grid of 192 units (= 1536-unit em /
8), which the generator asserts, so sampling the center of every grid cell
reproduces a glyph exactly: no anti-aliasing, no approximation.

The rectangles are merged offline and emitted as flat tables, so the module
only has to walk them -- and so the vertex cost of a nametag is a property of
this file rather than of a runtime pass.

Example:
    python3 scripts/gen_esp_pixel_font.py                  # write the header
    python3 scripts/gen_esp_pixel_font.py --check           # fail if it is stale
    python3 scripts/gen_esp_pixel_font.py --preview Notch   # ASCII art of a name
"""

import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FONT = ROOT / "resources" / "minecraft.ttf"
OUTPUT = ROOT / "src" / "modules" / "visual" / "esp_pixel_font.hpp"

FIRST_CODE_POINT = 0x20
LAST_CODE_POINT = 0x7E

# The unit grid of this font: every contour point of every printable glyph sits
# on a multiple of 192 units, and 1536 / 192 = 8, so the em is eight device
# pixels and the caps are seven of them.
PIXEL = 192
EM_ROWS = 1536 // PIXEL
CAP_ROWS = 7
ACCENT_ROWS = 1        # '^, ` and friends need one row above the caps
DESCENT_ROWS = 1       # g, j, p, q and y dip one row below the baseline
CELL_ROWS = CAP_ROWS + ACCENT_ROWS + DESCENT_ROWS
CELL_TOP = 960 + CAP_ROWS * PIXEL + ACCENT_ROWS * PIXEL   # font-space y of row 0
MAX_COLS = 6           # the widest printable glyph in this face
MAX_ADVANCE = 7        # '@' and '~' are billed a full cell plus one


# ---------------------------------------------------------------------------
# The smallest TrueType reader that can answer "which pixels are lit"
# ---------------------------------------------------------------------------

class Font:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        count, = struct.unpack(">H", self.data[4:6])
        self.tables = {}
        for i in range(count):
            entry = 12 + i * 16
            tag = self.data[entry:entry + 4].decode("latin1")
            _, offset, length = struct.unpack(">III", self.data[entry + 4:entry + 16])
            self.tables[tag] = (offset, length)

        head = self.tables["head"][0]
        self.units_per_em, = struct.unpack(">H", self.data[head + 18:head + 20])
        if self.units_per_em != 1536:
            raise SystemExit("resources/minecraft.ttf is not the font this script knows: "
                             "unitsPerEm is %d, expected 1536" % self.units_per_em)
        loc_format, = struct.unpack(">h", self.data[head + 50:head + 52])

        maxp = self.tables["maxp"][0]
        self.num_glyphs, = struct.unpack(">H", self.data[maxp + 4:maxp + 6])

        loca = self.tables["loca"][0]
        if loc_format == 1:
            self.loca = list(struct.unpack(">%dI" % (self.num_glyphs + 1),
                                           self.data[loca:loca + 4 * (self.num_glyphs + 1)]))
        else:
            self.loca = [v * 2 for v in struct.unpack(">%dH" % (self.num_glyphs + 1),
                                                       self.data[loca:loca + 2 * (self.num_glyphs + 1)])]

        self.glyf = self.tables["glyf"][0]

        hhea = self.tables["hhea"][0]
        metrics, = struct.unpack(">H", self.data[hhea + 34:hhea + 36])
        hmtx = self.tables["hmtx"][0]
        self.advances = []
        advance = 0
        for gid in range(self.num_glyphs):
            if gid < metrics:
                advance, = struct.unpack(">H", self.data[hmtx + gid * 4:hmtx + gid * 4 + 2])
            self.advances.append(advance)

        self.cmap = self._cmap()

    def _cmap(self):
        """The unicode format-4 subtable, as its segment arrays."""
        offset, _ = self.tables["cmap"]
        count, = struct.unpack(">H", self.data[offset + 2:offset + 4])
        subtable = None
        for i in range(count):
            pid, eid, off = struct.unpack(">HHI", self.data[offset + 4 + i * 8:offset + 12 + i * 8])
            fmt, = struct.unpack(">H", self.data[offset + off:offset + off + 2])
            if fmt == 4 and (pid, eid) in ((3, 1), (0, 3), (0, 4), (3, 10)):
                subtable = offset + off
        if subtable is None:
            raise SystemExit("no unicode format-4 cmap subtable in %s" % FONT)
        segments, = struct.unpack(">H", self.data[subtable + 6:subtable + 8])
        segments //= 2
        ends = struct.unpack(">%dH" % segments, self.data[subtable + 14:subtable + 14 + 2 * segments])
        starts = struct.unpack(">%dH" % segments,
                               self.data[subtable + 16 + 2 * segments:subtable + 16 + 4 * segments])
        deltas = struct.unpack(">%dh" % segments,
                               self.data[subtable + 16 + 4 * segments:subtable + 16 + 6 * segments])
        ranges = struct.unpack(">%dH" % segments,
                               self.data[subtable + 16 + 6 * segments:subtable + 16 + 8 * segments])
        return segments, starts, ends, deltas, ranges, subtable + 16 + 8 * segments

    def glyph_id(self, code_point):
        segments, starts, ends, deltas, ranges, range_at = self.cmap
        for i in range(segments):
            if starts[i] <= code_point <= ends[i]:
                if ranges[i] == 0:
                    return (code_point + deltas[i]) & 0xFFFF
                at = range_at + i * 2 + ranges[i] + (code_point - starts[i]) * 2
                gid, = struct.unpack(">H", self.data[at:at + 2])
                return 0 if gid == 0 else (gid + deltas[i]) & 0xFFFF
        return 0

    def simple_contours(self, gid):
        """The contours of a simple glyph, as closed point lists."""
        start, end = self.loca[gid], self.loca[gid + 1]
        if start == end:
            return []
        g = self.data[self.glyf + start:self.glyf + end]
        count, = struct.unpack(">h", g[0:2])
        if count <= 0:                       # composite glyphs: this face has none
            return []
        ends = struct.unpack(">%dH" % count, g[10:10 + 2 * count])
        at = 10 + 2 * count
        instructions, = struct.unpack(">H", g[at:at + 2])
        at += 2 + instructions
        total = ends[-1] + 1
        flags = []
        while len(flags) < total:
            flag = g[at]
            at += 1
            flags.append(flag)
            if flag & 0x08:                  # REPEAT_FLAG
                repeat = g[at]
                at += 1
                flags.extend([flag] * repeat)
        axis = []
        # (X_SHORT 0x02 / Y_SHORT 0x04, X_IS_SAME 0x10 / Y_IS_SAME 0x20,
        #  the sign bit of a short delta)
        for short, same, sign in ((0x02, 0x10, 0x10), (0x04, 0x20, 0x20)):
            values = []
            previous = 0
            for i in range(total):
                flag = flags[i]
                if flag & short:
                    delta = g[at]
                    at += 1
                    previous += delta if flag & sign else -delta
                elif not (flag & same):
                    delta, = struct.unpack(">h", g[at:at + 2])
                    at += 2
                    previous += delta
                values.append(previous)
            axis.append(values)
        points = list(zip(axis[0], axis[1]))
        contours = []
        first = 0
        for i in range(count):
            contours.append(points[first:ends[i] + 1])
            first = ends[i] + 1
        return contours

    def is_lit(self, contours, x, y):
        """Even-odd point-in-polygon, at the center of one grid cell."""
        inside = False
        for contour in contours:
            length = len(contour)
            for i in range(length):
                x0, y0 = contour[i]
                x1, y1 = contour[(i + 1) % length]
                if (y0 > y) != (y1 > y) and x0 + (y - y0) * (x1 - x0) / (y1 - y0) > x:
                    inside = not inside
        return inside

    def check(self):
        """Assert what the rasterizer assumes about this font, or say so loudly."""
        for cp in range(FIRST_CODE_POINT, LAST_CODE_POINT + 1):
            gid = self.glyph_id(cp)
            if not gid:
                raise SystemExit("the packaged font lost U+%04X (%s); the label path "
                                 "that used it has to fall back" % (cp, chr(cp)))
            rows, advance = self.rows_and_advance(cp)
            for contour in self.simple_contours(gid):
                for x, y in contour:
                    if x % PIXEL or y % PIXEL:
                        raise SystemExit("U+%04X left the %d-unit pixel grid at (%d, %d)"
                                         % (cp, PIXEL, x, y))
            if advance > MAX_ADVANCE or advance < 1:
                raise SystemExit("U+%04X advances %d pixels, outside the table's range"
                                 % (cp, advance))
            for r, mask in enumerate(rows):
                if mask >> MAX_COLS:
                    raise SystemExit("U+%04X row %d is wider than %d pixels"
                                     % (cp, r, MAX_COLS))
            if rebuild(rows, merge_rects(rows)) != rows:
                raise SystemExit("the merged rectangles of U+%04X do not redraw its cells" % cp)

    def rows_and_advance(self, code_point):
        """(one mask per row, top row first, bit 0 the leftmost pixel; the advance
        in font pixels straight out of hmtx)."""
        gid = self.glyph_id(code_point)
        contours = self.simple_contours(gid)
        rows = []
        for r in range(CELL_ROWS):
            y = CELL_TOP - PIXEL * r - PIXEL // 2
            mask = 0
            for c in range(MAX_COLS):
                if contours and self.is_lit(contours, PIXEL * c + PIXEL // 2, y):
                    mask |= 1 << c
            rows.append(mask)
        return rows, self.advances[gid] // PIXEL


# ---------------------------------------------------------------------------
# Cells -> rectangles
# ---------------------------------------------------------------------------

def merge_rects(rows):
    """A run of lit pixels in a row, extended down while that very run is still
    there, and every cell a rectangle covers is spent on it. Greedy rather than
    optimal, and good enough to keep a letter like 'm' at a few quads instead of
    one per pixel."""
    remaining = list(rows)
    rects = []
    for r in range(CELL_ROWS):
        c = 0
        while c < MAX_COLS:
            if not (remaining[r] >> c) & 1:
                c += 1
                continue
            width = 0
            while c + width < MAX_COLS and (remaining[r] >> (c + width)) & 1:
                width += 1
            span = ((1 << width) - 1) << c
            height = 1
            while r + height < CELL_ROWS and (remaining[r + height] & span) == span:
                height += 1
            for row in range(r, r + height):
                remaining[row] &= ~span
            rects.append((c, r, width, height))
            c += width
    return rects


def rebuild(rows, rects):
    """The masks a set of rectangles paints back -- the round trip the generator
    checks, so a table can never disagree with the font it came from."""
    out = [0] * CELL_ROWS
    for x, y, w, h in rects:
        for r in range(y, y + h):
            for c in range(x, x + w):
                out[r] |= 1 << c
    return out


# ---------------------------------------------------------------------------
# The header
# ---------------------------------------------------------------------------

def describe(code_point):
    if code_point == 0x20:
        return "space"
    if code_point == 0x5C:
        return "backslash"
    if code_point == 0x27:
        return "apostrophe"
    if 0x21 <= code_point <= 0x7E:
        return "'%s'" % chr(code_point)
    return "0x%02X" % code_point


def render(font):
    glyphs = []
    rects = []
    for cp in range(FIRST_CODE_POINT, LAST_CODE_POINT + 1):
        rows, advance = font.rows_and_advance(cp)
        merged = merge_rects(rows)
        glyphs.append((cp, len(rects), len(merged), advance))
        rects.extend(merged)

    l = []
    add = l.append
    add("#pragma once")
    add("")
    add("// GENERATED FILE -- do not edit by hand.")
    add("//")
    add("// `python3 scripts/gen_esp_pixel_font.py` writes this out of")
    add("// resources/minecraft.ttf (unitsPerEm %d, so one font pixel is %d units and" % (1536, PIXEL))
    add("// the em is %d of them); `--check` re-derives it and fails on a difference," % EM_ROWS)
    add("// and `--preview Notch` prints any name as lit cells.")
    add("")
    add("""
/*
The pixel face of the game's own font, for the text the Esp module draws as
*world-space geometry*: the nametag, the health value and the distance readout.

The level render pass has no text -- it has a tessellator -- so a label there is
built out of filled quads. That is the only way to pin a label to an entity at
all: a HUD-projected one sits on the module's model of the camera (the Fov
slider, the game's sprint FOV, one frame of look latency) and slides off the
head whenever that model is off, while quads handed to the game are placed by
the very matrices that drew the hitbox and cannot move relative to it.

Which is why this file exists rather than a table of guesses: centering a label
needs a width, a width needs the font's cells, and the only font whose cells are
known exactly here is the one this package ships. Reading them out of it also
makes the overlay read as the game's own nameplate instead of a foreign element.

Layout of one cell:

  * the font is a pixel font on a %d-unit grid, and the generator asserts that
    every contour point of every printable glyph sits on it, so sampling the
    center of a grid cell reproduces a glyph exactly;
  * a cell is %d rows of up to %d pixels, row 0 at the top, and `kPixelRects'
    entries are (x, y, width, height) in those pixels -- merged runs, so a
    vertical stroke is one quad and not five;
  * %d rows of that is cap height, %d above it is the accent row and %d below
    the baseline is the descender row (g, j, p, q, y), so a name's descenders
    never cross the baseline the next label is stacked on;
  * `advance' is the character's own cell width out of hmtx, which is why an `i'
    costs a third of an `M' and a name comes out with the game's spacing;
  * the covered range is 0x20..0x7E, the range the face really has. Anything
    else -- an Arabic or CJK name, a leftover markup sign -- has no cell, and
    Esp keeps those labels on the launcher's HUD font, which has the glyphs and
    shapes right-to-left text.

The rectangles are merged here, at generation time, so the module only walks a
table when it builds a frame, and so the cost of a nametag in vertices is a
property of this file (see kPixelRectCount).
*/""" % (PIXEL, CELL_ROWS, MAX_COLS, CAP_ROWS, ACCENT_ROWS, DESCENT_ROWS))
    add("")
    add("#include <cstddef>")
    add("#include <cstdint>")
    add("")
    add("namespace esp::world {")
    add("")
    add("// The cell grid of this face, in its own pixels.")
    add("inline constexpr int kPixelRows = %d;" % CELL_ROWS)
    add("inline constexpr int kPixelColumns = %d;" % MAX_COLS)
    add("inline constexpr int kPixelCapHeight = %d;" % CAP_ROWS)
    add("inline constexpr int kPixelAscent = %d;" % (CAP_ROWS + ACCENT_ROWS))
    add("inline constexpr int kPixelDescent = %d;" % DESCENT_ROWS)
    add("inline constexpr int kPixelEm = %d;" % EM_ROWS)
    add("")
    add("// The code points the face carries, inclusive, and the entry count.")
    add("inline constexpr std::uint32_t kPixelFirstCodePoint = 0x%X;" % FIRST_CODE_POINT)
    add("inline constexpr std::uint32_t kPixelLastCodePoint = 0x%X;" % LAST_CODE_POINT)
    add("inline constexpr std::size_t kPixelGlyphCount = %d;" % len(glyphs))
    add("inline constexpr std::size_t kPixelRectCount = %d;" % len(rects))
    add("")
    add("// One filled run of the cell. x grows to the right and y downwards, both in")
    add("// the font's own pixels, and the block is %d rows tall." % CELL_ROWS)
    add("struct PixelRect {")
    add("    std::uint8_t x;")
    add("    std::uint8_t y;")
    add("    std::uint8_t width;")
    add("    std::uint8_t height;")
    add("};")
    add("")
    add("// One character: the run of kPixelRects it is made of (empty for a space),")
    add("// and the cell width the font bills for it.")
    add("struct PixelGlyph {")
    add("    std::uint16_t firstRect;")
    add("    std::uint8_t rectCount;")
    add("    std::uint8_t advance;")
    add("};")
    add("")
    add("inline constexpr PixelRect kPixelRects[] = {")
    for x, y, w, h in rects:
        add("    {%d, %d, %d, %d}," % (x, y, w, h))
    add("};")
    add("")
    add("inline constexpr PixelGlyph kPixelGlyphs[kPixelGlyphCount] = {")
    for cp, first, count, advance in glyphs:
        add("    {%5d, %d, %d}, // 0x%02X %-13s %3d rects" % (first, count, advance, cp,
                                                              describe(cp), count))
    add("};")
    add("")
    add("// True when this face can draw one code point on its own. The Esp module")
    add("// takes that as the whole test for whether a label belongs in the mesh pass:")
    add("// a name it cannot spell is left to the launcher's font instead of being")
    add("// drawn as gaps.")
    add("inline constexpr bool pixelGlyphHas(std::uint32_t cp) {")
    add("    return cp >= kPixelFirstCodePoint && cp <= kPixelLastCodePoint;")
    add("}")
    add("")
    add("// The cell of one covered code point, or nullptr.")
    add("inline constexpr const PixelGlyph* pixelGlyph(std::uint32_t cp) {")
    add("    if (!pixelGlyphHas(cp)) return nullptr;")
    add("    return &kPixelGlyphs[cp - kPixelFirstCodePoint];")
    add("}")
    add("")
    add("} // namespace esp::world")
    add("")
    return "\n".join(l)


def preview(font, text):
    """A string as lit cells, with the baseline marked, for reviewing the face."""
    advances = []
    rows_of = []
    for ch in text:
        rows, advance = font.rows_and_advance(ord(ch))
        rows_of.append(rows)
        advances.append(advance)
    out = []
    for r in range(CELL_ROWS):
        line = ""
        for index, rows in enumerate(rows_of):
            line += "".join("#" if rows[r] & (1 << c) else "." for c in range(advances[index]))
        out.append(line)
    width = sum(advances)
    out.insert(CELL_ROWS - DESCENT_ROWS, "-" * width)
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--font", type=Path, default=FONT)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--check", action="store_true",
                        help="fail when the committed header is not what the font says")
    parser.add_argument("--preview", metavar="TEXT",
                        help="print a string as the cell grid instead of writing anything")
    args = parser.parse_args()

    if not args.font.exists():
        raise SystemExit("missing %s" % args.font)
    font = Font(args.font)
    font.check()

    if args.preview:
        print(preview(font, args.preview))
        return 0

    text = render(font)
    if args.check:
        current = args.output.read_text() if args.output.exists() else ""
        if current != text:
            raise SystemExit("%s is stale; run scripts/gen_esp_pixel_font.py" % args.output)
        print("%s matches %s" % (args.output.name, args.font.name))
        return 0
    args.output.write_text(text)
    print("wrote %s" % args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())

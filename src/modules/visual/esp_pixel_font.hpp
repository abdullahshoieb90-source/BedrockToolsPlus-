#pragma once

// GENERATED FILE -- do not edit by hand.
//
// `python3 scripts/gen_esp_pixel_font.py` writes this out of
// resources/minecraft.ttf (unitsPerEm 1536, so one font pixel is 192 units and
// the em is 8 of them); `--check` re-derives it and fails on a difference,
// and `--preview Notch` prints any name as lit cells.


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

  * the font is a pixel font on a 192-unit grid, and the generator asserts that
    every contour point of every printable glyph sits on it, so sampling the
    center of a grid cell reproduces a glyph exactly;
  * a cell is 9 rows of up to 6 pixels, row 0 at the top, and `kPixelRects'
    entries are (x, y, width, height) in those pixels -- merged runs, so a
    vertical stroke is one quad and not five;
  * 7 rows of that is cap height, 1 above it is the accent row and 1 below
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
*/

#include <cstddef>
#include <cstdint>

namespace esp::world {

// The cell grid of this face, in its own pixels.
inline constexpr int kPixelRows = 9;
inline constexpr int kPixelColumns = 6;
inline constexpr int kPixelCapHeight = 7;
inline constexpr int kPixelAscent = 8;
inline constexpr int kPixelDescent = 1;
inline constexpr int kPixelEm = 8;

// The code points the face carries, inclusive, and the entry count.
inline constexpr std::uint32_t kPixelFirstCodePoint = 0x20;
inline constexpr std::uint32_t kPixelLastCodePoint = 0x7E;
inline constexpr std::size_t kPixelGlyphCount = 95;
inline constexpr std::size_t kPixelRectCount = 430;

// One filled run of the cell. x grows to the right and y downwards, both in
// the font's own pixels, and the block is 9 rows tall.
struct PixelRect {
    std::uint8_t x;
    std::uint8_t y;
    std::uint8_t width;
    std::uint8_t height;
};

// One character: the run of kPixelRects it is made of (empty for a space),
// and the cell width the font bills for it.
struct PixelGlyph {
    std::uint16_t firstRect;
    std::uint8_t rectCount;
    std::uint8_t advance;
};

inline constexpr PixelRect kPixelRects[] = {
    {0, 1, 1, 5},
    {0, 7, 1, 1},
    {0, 1, 1, 2},
    {2, 1, 1, 2},
    {1, 1, 1, 7},
    {3, 1, 1, 7},
    {0, 3, 1, 1},
    {2, 3, 1, 1},
    {4, 3, 1, 1},
    {0, 5, 1, 1},
    {2, 5, 1, 1},
    {4, 5, 1, 1},
    {2, 1, 1, 2},
    {1, 2, 1, 1},
    {3, 2, 2, 1},
    {0, 3, 1, 1},
    {1, 4, 3, 1},
    {4, 5, 1, 1},
    {0, 6, 4, 1},
    {2, 7, 1, 1},
    {0, 1, 1, 2},
    {4, 1, 1, 1},
    {3, 2, 1, 2},
    {2, 4, 1, 1},
    {1, 5, 1, 2},
    {4, 6, 1, 2},
    {0, 7, 1, 1},
    {2, 1, 1, 1},
    {1, 2, 1, 1},
    {3, 2, 1, 1},
    {2, 3, 1, 3},
    {1, 4, 1, 1},
    {4, 4, 1, 1},
    {0, 5, 1, 2},
    {3, 5, 1, 2},
    {1, 7, 2, 1},
    {4, 7, 1, 1},
    {0, 1, 1, 2},
    {2, 1, 2, 1},
    {1, 2, 1, 1},
    {0, 3, 1, 3},
    {1, 6, 1, 1},
    {2, 7, 2, 1},
    {0, 1, 2, 1},
    {2, 2, 1, 1},
    {3, 3, 1, 3},
    {2, 6, 1, 1},
    {0, 7, 2, 1},
    {0, 1, 1, 1},
    {3, 1, 1, 1},
    {1, 2, 2, 1},
    {0, 3, 1, 1},
    {3, 3, 1, 1},
    {2, 3, 1, 5},
    {0, 5, 2, 1},
    {3, 5, 2, 1},
    {0, 6, 1, 3},
    {0, 5, 5, 1},
    {0, 6, 1, 2},
    {4, 1, 1, 1},
    {3, 2, 1, 2},
    {2, 4, 1, 1},
    {1, 5, 1, 2},
    {0, 7, 1, 1},
    {1, 1, 3, 1},
    {0, 2, 1, 5},
    {4, 2, 1, 5},
    {3, 3, 1, 1},
    {2, 4, 1, 1},
    {1, 5, 1, 1},
    {1, 7, 3, 1},
    {2, 1, 1, 7},
    {1, 2, 1, 1},
    {0, 7, 2, 1},
    {3, 7, 2, 1},
    {1, 1, 3, 1},
    {0, 2, 1, 1},
    {4, 2, 1, 2},
    {2, 4, 2, 1},
    {1, 5, 1, 1},
    {0, 6, 1, 2},
    {1, 7, 4, 1},
    {1, 1, 3, 1},
    {0, 2, 1, 1},
    {4, 2, 1, 2},
    {2, 4, 2, 1},
    {4, 5, 1, 2},
    {0, 6, 1, 1},
    {1, 7, 3, 1},
    {3, 1, 2, 1},
    {2, 2, 1, 1},
    {4, 2, 1, 6},
    {1, 3, 1, 1},
    {0, 4, 1, 2},
    {1, 5, 3, 1},
    {0, 1, 5, 1},
    {0, 2, 1, 2},
    {1, 3, 3, 1},
    {4, 4, 1, 3},
    {0, 6, 1, 1},
    {1, 7, 3, 1},
    {2, 1, 2, 1},
    {1, 2, 1, 1},
    {0, 3, 1, 4},
    {1, 4, 3, 1},
    {4, 5, 1, 2},
    {1, 7, 3, 1},
    {0, 1, 5, 1},
    {0, 2, 1, 1},
    {4, 2, 1, 2},
    {3, 4, 1, 1},
    {2, 5, 1, 3},
    {1, 1, 3, 1},
    {0, 2, 1, 2},
    {4, 2, 1, 2},
    {1, 4, 3, 1},
    {0, 5, 1, 2},
    {4, 5, 1, 2},
    {1, 7, 3, 1},
    {1, 1, 3, 1},
    {0, 2, 1, 2},
    {4, 2, 1, 4},
    {1, 4, 3, 1},
    {3, 6, 1, 1},
    {1, 7, 2, 1},
    {0, 2, 1, 2},
    {0, 6, 1, 2},
    {0, 2, 1, 2},
    {0, 6, 1, 3},
    {3, 1, 1, 1},
    {2, 2, 1, 1},
    {1, 3, 1, 1},
    {0, 4, 1, 1},
    {1, 5, 1, 1},
    {2, 6, 1, 1},
    {3, 7, 1, 1},
    {0, 3, 5, 1},
    {0, 6, 5, 1},
    {0, 1, 1, 1},
    {1, 2, 1, 1},
    {2, 3, 1, 1},
    {3, 4, 1, 1},
    {2, 5, 1, 1},
    {1, 6, 1, 1},
    {0, 7, 1, 1},
    {1, 1, 3, 1},
    {0, 2, 1, 1},
    {4, 2, 1, 2},
    {3, 4, 1, 1},
    {2, 5, 1, 1},
    {2, 7, 1, 1},
    {1, 1, 4, 1},
    {0, 2, 1, 5},
    {5, 2, 1, 4},
    {2, 3, 2, 3},
    {4, 5, 1, 1},
    {1, 7, 5, 1},
    {1, 1, 3, 1},
    {0, 2, 1, 6},
    {4, 2, 1, 6},
    {1, 3, 3, 1},
    {0, 1, 4, 1},
    {0, 2, 1, 6},
    {4, 2, 1, 1},
    {1, 3, 3, 1},
    {4, 4, 1, 3},
    {1, 7, 3, 1},
    {1, 1, 3, 1},
    {0, 2, 1, 5},
    {4, 2, 1, 1},
    {4, 6, 1, 1},
    {1, 7, 3, 1},
    {0, 1, 4, 1},
    {0, 2, 1, 6},
    {4, 2, 1, 5},
    {1, 7, 3, 1},
    {0, 1, 5, 1},
    {0, 2, 1, 6},
    {1, 3, 2, 1},
    {1, 7, 4, 1},
    {0, 1, 5, 1},
    {0, 2, 1, 6},
    {1, 3, 2, 1},
    {1, 1, 4, 1},
    {0, 2, 1, 5},
    {2, 3, 3, 1},
    {4, 4, 1, 3},
    {1, 7, 3, 1},
    {0, 1, 1, 7},
    {4, 1, 1, 7},
    {1, 3, 3, 1},
    {0, 1, 3, 1},
    {1, 2, 1, 6},
    {0, 7, 1, 1},
    {2, 7, 1, 1},
    {4, 1, 1, 6},
    {0, 6, 1, 1},
    {1, 7, 3, 1},
    {0, 1, 1, 7},
    {4, 1, 1, 1},
    {3, 2, 1, 1},
    {1, 3, 2, 1},
    {3, 4, 1, 1},
    {4, 5, 1, 3},
    {0, 1, 1, 7},
    {1, 7, 4, 1},
    {0, 1, 1, 7},
    {4, 1, 1, 7},
    {1, 2, 1, 1},
    {3, 2, 1, 1},
    {2, 3, 1, 1},
    {0, 1, 1, 7},
    {4, 1, 1, 7},
    {1, 2, 1, 1},
    {2, 3, 1, 1},
    {3, 4, 1, 1},
    {1, 1, 3, 1},
    {0, 2, 1, 5},
    {4, 2, 1, 5},
    {1, 7, 3, 1},
    {0, 1, 4, 1},
    {0, 2, 1, 6},
    {4, 2, 1, 1},
    {1, 3, 3, 1},
    {1, 1, 3, 1},
    {0, 2, 1, 5},
    {4, 2, 1, 4},
    {3, 6, 1, 1},
    {1, 7, 2, 1},
    {4, 7, 1, 1},
    {0, 1, 4, 1},
    {0, 2, 1, 6},
    {4, 2, 1, 1},
    {1, 3, 3, 1},
    {4, 4, 1, 4},
    {1, 1, 4, 1},
    {0, 2, 1, 1},
    {1, 3, 3, 1},
    {4, 4, 1, 3},
    {0, 6, 1, 1},
    {1, 7, 3, 1},
    {0, 1, 5, 1},
    {2, 2, 1, 6},
    {0, 1, 1, 6},
    {4, 1, 1, 6},
    {1, 7, 3, 1},
    {0, 1, 1, 4},
    {4, 1, 1, 4},
    {1, 5, 1, 2},
    {3, 5, 1, 2},
    {2, 7, 1, 1},
    {0, 1, 1, 7},
    {4, 1, 1, 7},
    {2, 5, 1, 1},
    {1, 6, 1, 1},
    {3, 6, 1, 1},
    {0, 1, 1, 1},
    {4, 1, 1, 1},
    {1, 2, 1, 1},
    {3, 2, 1, 1},
    {2, 3, 1, 1},
    {1, 4, 1, 1},
    {3, 4, 1, 1},
    {0, 5, 1, 3},
    {4, 5, 1, 3},
    {0, 1, 1, 1},
    {4, 1, 1, 1},
    {1, 2, 1, 1},
    {3, 2, 1, 1},
    {2, 3, 1, 5},
    {0, 1, 5, 1},
    {4, 2, 1, 1},
    {3, 3, 1, 1},
    {2, 4, 1, 1},
    {1, 5, 1, 1},
    {0, 6, 1, 2},
    {1, 7, 4, 1},
    {0, 1, 3, 1},
    {0, 2, 1, 6},
    {1, 7, 2, 1},
    {0, 1, 1, 1},
    {1, 2, 1, 2},
    {2, 4, 1, 1},
    {3, 5, 1, 2},
    {4, 7, 1, 1},
    {0, 1, 3, 1},
    {2, 2, 1, 6},
    {0, 7, 2, 1},
    {2, 1, 1, 1},
    {1, 2, 1, 1},
    {3, 2, 1, 1},
    {0, 3, 1, 1},
    {4, 3, 1, 1},
    {0, 8, 5, 1},
    {0, 0, 1, 1},
    {1, 1, 1, 1},
    {1, 3, 3, 1},
    {4, 4, 1, 4},
    {1, 5, 3, 1},
    {0, 6, 1, 1},
    {1, 7, 3, 1},
    {0, 1, 1, 7},
    {2, 3, 2, 1},
    {1, 4, 1, 1},
    {4, 4, 1, 3},
    {1, 7, 3, 1},
    {1, 3, 3, 1},
    {0, 4, 1, 3},
    {4, 4, 1, 1},
    {4, 6, 1, 1},
    {1, 7, 3, 1},
    {4, 1, 1, 7},
    {1, 3, 2, 1},
    {0, 4, 1, 3},
    {3, 4, 1, 1},
    {1, 7, 3, 1},
    {1, 3, 3, 1},
    {0, 4, 1, 3},
    {4, 4, 1, 2},
    {1, 5, 3, 1},
    {1, 7, 4, 1},
    {2, 1, 2, 1},
    {1, 2, 1, 6},
    {0, 3, 1, 1},
    {2, 3, 2, 1},
    {1, 3, 4, 1},
    {0, 4, 1, 2},
    {4, 4, 1, 4},
    {1, 6, 3, 1},
    {0, 8, 4, 1},
    {0, 1, 1, 7},
    {2, 3, 2, 1},
    {1, 4, 1, 1},
    {4, 4, 1, 4},
    {0, 1, 1, 1},
    {0, 3, 1, 5},
    {4, 1, 1, 1},
    {4, 3, 1, 5},
    {0, 6, 1, 2},
    {1, 8, 3, 1},
    {0, 1, 1, 7},
    {3, 3, 1, 1},
    {2, 4, 1, 1},
    {1, 5, 1, 1},
    {2, 6, 1, 1},
    {3, 7, 1, 1},
    {0, 1, 1, 6},
    {1, 7, 1, 1},
    {0, 3, 2, 1},
    {3, 3, 1, 1},
    {0, 4, 1, 4},
    {2, 4, 1, 2},
    {4, 4, 1, 4},
    {0, 3, 4, 1},
    {0, 4, 1, 4},
    {4, 4, 1, 4},
    {1, 3, 3, 1},
    {0, 4, 1, 3},
    {4, 4, 1, 3},
    {1, 7, 3, 1},
    {0, 3, 1, 6},
    {2, 3, 2, 1},
    {1, 4, 1, 1},
    {4, 4, 1, 2},
    {1, 6, 3, 1},
    {1, 3, 2, 1},
    {4, 3, 1, 6},
    {0, 4, 1, 2},
    {3, 4, 1, 1},
    {1, 6, 3, 1},
    {0, 3, 1, 5},
    {2, 3, 2, 1},
    {1, 4, 1, 1},
    {4, 4, 1, 1},
    {1, 3, 4, 1},
    {0, 4, 1, 1},
    {1, 5, 3, 1},
    {4, 6, 1, 1},
    {0, 7, 4, 1},
    {1, 1, 1, 6},
    {0, 2, 1, 1},
    {2, 2, 1, 1},
    {2, 7, 1, 1},
    {0, 3, 1, 4},
    {4, 3, 1, 5},
    {1, 7, 3, 1},
    {0, 3, 1, 3},
    {4, 3, 1, 3},
    {1, 6, 1, 1},
    {3, 6, 1, 1},
    {2, 7, 1, 1},
    {0, 3, 1, 4},
    {4, 3, 1, 5},
    {2, 5, 1, 3},
    {1, 7, 1, 1},
    {3, 7, 1, 1},
    {0, 3, 1, 1},
    {4, 3, 1, 1},
    {1, 4, 1, 1},
    {3, 4, 1, 1},
    {2, 5, 1, 1},
    {1, 6, 1, 1},
    {3, 6, 1, 1},
    {0, 7, 1, 1},
    {4, 7, 1, 1},
    {0, 3, 1, 3},
    {4, 3, 1, 5},
    {1, 6, 3, 1},
    {0, 8, 4, 1},
    {0, 3, 5, 1},
    {3, 4, 1, 1},
    {2, 5, 1, 1},
    {1, 6, 1, 2},
    {0, 7, 1, 1},
    {2, 7, 3, 1},
    {2, 1, 2, 1},
    {1, 2, 1, 2},
    {0, 4, 1, 1},
    {1, 5, 1, 2},
    {2, 7, 2, 1},
    {0, 1, 1, 8},
    {0, 1, 2, 1},
    {2, 2, 1, 2},
    {3, 4, 1, 1},
    {2, 5, 1, 2},
    {0, 7, 2, 1},
    {1, 1, 2, 1},
    {5, 1, 1, 1},
    {0, 2, 1, 1},
    {3, 2, 2, 1},
};

inline constexpr PixelGlyph kPixelGlyphs[kPixelGlyphCount] = {
    {    0, 0, 2}, // 0x20 space           0 rects
    {    0, 2, 2}, // 0x21 '!'             2 rects
    {    2, 2, 4}, // 0x22 '"'             2 rects
    {    4, 8, 6}, // 0x23 '#'             8 rects
    {   12, 8, 6}, // 0x24 '$'             8 rects
    {   20, 7, 6}, // 0x25 '%'             7 rects
    {   27, 10, 6}, // 0x26 '&'            10 rects
    {   37, 1, 2}, // 0x27 apostrophe      1 rects
    {   38, 5, 5}, // 0x28 '('             5 rects
    {   43, 5, 5}, // 0x29 ')'             5 rects
    {   48, 5, 5}, // 0x2A '*'             5 rects
    {   53, 3, 6}, // 0x2B '+'             3 rects
    {   56, 1, 2}, // 0x2C ','             1 rects
    {   57, 1, 6}, // 0x2D '-'             1 rects
    {   58, 1, 2}, // 0x2E '.'             1 rects
    {   59, 5, 6}, // 0x2F '/'             5 rects
    {   64, 7, 6}, // 0x30 '0'             7 rects
    {   71, 4, 6}, // 0x31 '1'             4 rects
    {   75, 7, 6}, // 0x32 '2'             7 rects
    {   82, 7, 6}, // 0x33 '3'             7 rects
    {   89, 6, 6}, // 0x34 '4'             6 rects
    {   95, 6, 6}, // 0x35 '5'             6 rects
    {  101, 6, 6}, // 0x36 '6'             6 rects
    {  107, 5, 6}, // 0x37 '7'             5 rects
    {  112, 7, 6}, // 0x38 '8'             7 rects
    {  119, 6, 6}, // 0x39 '9'             6 rects
    {  125, 2, 2}, // 0x3A ':'             2 rects
    {  127, 2, 2}, // 0x3B ';'             2 rects
    {  129, 7, 5}, // 0x3C '<'             7 rects
    {  136, 2, 6}, // 0x3D '='             2 rects
    {  138, 7, 5}, // 0x3E '>'             7 rects
    {  145, 6, 6}, // 0x3F '?'             6 rects
    {  151, 6, 7}, // 0x40 '@'             6 rects
    {  157, 4, 6}, // 0x41 'A'             4 rects
    {  161, 6, 6}, // 0x42 'B'             6 rects
    {  167, 5, 6}, // 0x43 'C'             5 rects
    {  172, 4, 6}, // 0x44 'D'             4 rects
    {  176, 4, 6}, // 0x45 'E'             4 rects
    {  180, 3, 6}, // 0x46 'F'             3 rects
    {  183, 5, 6}, // 0x47 'G'             5 rects
    {  188, 3, 6}, // 0x48 'H'             3 rects
    {  191, 4, 4}, // 0x49 'I'             4 rects
    {  195, 3, 6}, // 0x4A 'J'             3 rects
    {  198, 6, 6}, // 0x4B 'K'             6 rects
    {  204, 2, 6}, // 0x4C 'L'             2 rects
    {  206, 5, 6}, // 0x4D 'M'             5 rects
    {  211, 5, 6}, // 0x4E 'N'             5 rects
    {  216, 4, 6}, // 0x4F 'O'             4 rects
    {  220, 4, 6}, // 0x50 'P'             4 rects
    {  224, 6, 6}, // 0x51 'Q'             6 rects
    {  230, 5, 6}, // 0x52 'R'             5 rects
    {  235, 6, 6}, // 0x53 'S'             6 rects
    {  241, 2, 6}, // 0x54 'T'             2 rects
    {  243, 3, 6}, // 0x55 'U'             3 rects
    {  246, 5, 6}, // 0x56 'V'             5 rects
    {  251, 5, 6}, // 0x57 'W'             5 rects
    {  256, 9, 6}, // 0x58 'X'             9 rects
    {  265, 5, 6}, // 0x59 'Y'             5 rects
    {  270, 7, 6}, // 0x5A 'Z'             7 rects
    {  277, 3, 4}, // 0x5B '['             3 rects
    {  280, 5, 6}, // 0x5C backslash       5 rects
    {  285, 3, 4}, // 0x5D ']'             3 rects
    {  288, 5, 6}, // 0x5E '^'             5 rects
    {  293, 1, 6}, // 0x5F '_'             1 rects
    {  294, 2, 3}, // 0x60 '`'             2 rects
    {  296, 5, 6}, // 0x61 'a'             5 rects
    {  301, 5, 6}, // 0x62 'b'             5 rects
    {  306, 5, 6}, // 0x63 'c'             5 rects
    {  311, 5, 6}, // 0x64 'd'             5 rects
    {  316, 5, 6}, // 0x65 'e'             5 rects
    {  321, 4, 5}, // 0x66 'f'             4 rects
    {  325, 5, 6}, // 0x67 'g'             5 rects
    {  330, 4, 6}, // 0x68 'h'             4 rects
    {  334, 2, 2}, // 0x69 'i'             2 rects
    {  336, 4, 6}, // 0x6A 'j'             4 rects
    {  340, 6, 5}, // 0x6B 'k'             6 rects
    {  346, 2, 3}, // 0x6C 'l'             2 rects
    {  348, 5, 6}, // 0x6D 'm'             5 rects
    {  353, 3, 6}, // 0x6E 'n'             3 rects
    {  356, 4, 6}, // 0x6F 'o'             4 rects
    {  360, 5, 6}, // 0x70 'p'             5 rects
    {  365, 5, 6}, // 0x71 'q'             5 rects
    {  370, 4, 6}, // 0x72 'r'             4 rects
    {  374, 5, 6}, // 0x73 's'             5 rects
    {  379, 4, 4}, // 0x74 't'             4 rects
    {  383, 3, 6}, // 0x75 'u'             3 rects
    {  386, 5, 6}, // 0x76 'v'             5 rects
    {  391, 5, 6}, // 0x77 'w'             5 rects
    {  396, 9, 6}, // 0x78 'x'             9 rects
    {  405, 4, 6}, // 0x79 'y'             4 rects
    {  409, 6, 6}, // 0x7A 'z'             6 rects
    {  415, 5, 5}, // 0x7B '{'             5 rects
    {  420, 1, 2}, // 0x7C '|'             1 rects
    {  421, 5, 5}, // 0x7D '}'             5 rects
    {  426, 4, 7}, // 0x7E '~'             4 rects
};

// True when this face can draw one code point on its own. The Esp module
// takes that as the whole test for whether a label belongs in the mesh pass:
// a name it cannot spell is left to the launcher's font instead of being
// drawn as gaps.
inline constexpr bool pixelGlyphHas(std::uint32_t cp) {
    return cp >= kPixelFirstCodePoint && cp <= kPixelLastCodePoint;
}

// The cell of one covered code point, or nullptr.
inline constexpr const PixelGlyph* pixelGlyph(std::uint32_t cp) {
    if (!pixelGlyphHas(cp)) return nullptr;
    return &kPixelGlyphs[cp - kPixelFirstCodePoint];
}

} // namespace esp::world

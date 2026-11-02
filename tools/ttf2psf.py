#!/usr/bin/env python3
"""Convert pixel TTF fonts into PSF2 console fonts for VeeTee 2350.

Run from tools/:  uv run ttf2psf.py            (builds every font in fonts.toml)
                  uv run ttf2psf.py --preview notjam-clean-16 'Hello'

Why PSF2?  It is the Linux console font format: a fixed-cell bitmap font
with an optional Unicode table. Using it for our built-in fonts means the
same loader also accepts the large existing body of console fonts
(Terminus, Spleen, the kbd collection, ...), which covers the "loadable
fonts" requirement without inventing a format.

The one thing PSF2 lacks is a baseline, which the renderer needs to place
underlines. PSF2's header has a `headersize` field (offset of the glyph
data), so we append an 8-byte extension after the standard 32-byte header:

    b"VTEX" | version (1) | baseline row | underline row | reserved (0)

Readers that don't know about it skip it by honouring headersize, and our
loader falls back to a heuristic for fonts that lack it.

Pipeline per font (see fonts.toml for the knobs):
 1. FreeType renders each mapped codepoint monochrome, unhinted, at the
    font's native ppem. At that size one design pixel is exactly one
    output pixel, so the result is lossless.
 2. The bitmap is placed in a cell_w x cell_h box relative to the baseline.
    Glyphs whose ink falls outside the cell are dropped (and counted);
    the terminal will show the "missing" glyph for them instead.
 3. Box drawing, block elements and DEC special-graphics extras the font
    lacks are synthesized procedurally so line drawing always works.
 4. Identical bitmaps are de-duplicated (PSF2 maps many codepoints to one
    glyph), glyph 0 is a "missing glyph" box, and the result is written
    as PSF2 files plus one C source embedding all of them.
"""

from __future__ import annotations

import argparse
import sys
import tomllib
import unicodedata
from dataclasses import dataclass, field
from pathlib import Path

import freetype

TOOLS_DIR = Path(__file__).resolve().parent
REPO_DIR = TOOLS_DIR.parent
PSF_OUT_DIR = REPO_DIR / "fonts" / "psf"
C_OUT_PATH = REPO_DIR / "lib" / "font" / "fonts_builtin.c"

PSF2_MAGIC = b"\x72\xb5\x4a\x86"
PSF2_HAS_UNICODE_TABLE = 0x01
PSF2_SEPARATOR = 0xFF
EXT_MAGIC = b"VTEX"
EXT_VERSION = 1
HEADER_SIZE = 32 + 8

# A glyph is a tuple of rows; each row is a tuple of bools (True = ink).
Bitmap = tuple[tuple[bool, ...], ...]


@dataclass
class FontRecipe:
    name: str
    source: Path
    ppem: int
    cell_w: int
    cell_h: int
    baseline: int
    align: str
    synth: list[str]
    license: str
    default: bool = False
    # Fix fonts whose cmap points at the wrong drawings: {target: source}
    # means "codepoint `target` uses the drawing currently at `source`".
    remap: dict[str, str] = field(default_factory=dict)


@dataclass
class BuiltFont:
    recipe: FontRecipe
    # codepoint -> bitmap, before de-duplication
    glyphs: dict[int, Bitmap] = field(default_factory=dict)
    dropped: list[int] = field(default_factory=list)
    synthesized: list[int] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Bitmap helpers


class Canvas:
    """Mutable cell-sized pixel grid used while building one glyph."""

    def __init__(self, width: int, height: int):
        self.width = width
        self.height = height
        self.pixels = [[False] * width for _ in range(height)]

    def set(self, x: int, y: int) -> None:
        if 0 <= x < self.width and 0 <= y < self.height:
            self.pixels[y][x] = True

    def hline(self, x0: int, x1: int, y: int) -> None:
        for x in range(min(x0, x1), max(x0, x1) + 1):
            self.set(x, y)

    def vline(self, x: int, y0: int, y1: int) -> None:
        for y in range(min(y0, y1), max(y0, y1) + 1):
            self.set(x, y)

    def rect(self, x0: int, y0: int, x1: int, y1: int) -> None:
        """Fill the half-open rectangle [x0, x1) x [y0, y1)."""
        for y in range(y0, y1):
            for x in range(x0, x1):
                self.set(x, y)

    def frozen(self) -> Bitmap:
        return tuple(tuple(row) for row in self.pixels)


def blank(width: int, height: int) -> Bitmap:
    return Canvas(width, height).frozen()


# ---------------------------------------------------------------------------
# FreeType rasterization


def rasterize_font(recipe: FontRecipe) -> BuiltFont:
    """Render every mapped, printable codepoint of the TTF into cells."""
    built = BuiltFont(recipe)
    face = freetype.Face(str(recipe.source))
    face.set_pixel_sizes(recipe.ppem, recipe.ppem)
    load_flags = (
        freetype.FT_LOAD_RENDER
        | freetype.FT_LOAD_TARGET_MONO
        | freetype.FT_LOAD_NO_HINTING
    )

    for codepoint, glyph_index in face.get_chars():
        # Control characters never reach the renderer, and the C1 range
        # (0x80-0x9F) is control in a terminal even if a font draws it.
        if codepoint < 0x20 or 0x7F <= codepoint < 0xA0:
            continue
        face.load_glyph(glyph_index, load_flags)
        slot = face.glyph
        bitmap = slot.bitmap
        advance_px = slot.advance.x / 64.0

        if recipe.align == "center":
            origin_x = round((recipe.cell_w - advance_px) / 2)
        else:
            origin_x = 0

        ink = []
        for row in range(bitmap.rows):
            for col in range(bitmap.width):
                byte = bitmap.buffer[row * bitmap.pitch + col // 8]
                if byte & (0x80 >> (col % 8)):
                    ink.append(
                        (
                            origin_x + slot.bitmap_left + col,
                            recipe.baseline - slot.bitmap_top + row,
                        )
                    )

        # Centring a proportional glyph can push ink off one edge even when
        # it would fit; slide it back inside rather than dropping it.
        # Never applied vertically: that would break the baseline.
        if ink and recipe.align == "center":
            min_x = min(x for x, _ in ink)
            max_x = max(x for x, _ in ink)
            if max_x - min_x < recipe.cell_w:
                shift = max(0, -min_x) - max(0, max_x - (recipe.cell_w - 1))
                ink = [(x + shift, y) for x, y in ink]

        canvas = Canvas(recipe.cell_w, recipe.cell_h)
        lost_ink = False
        for x, y in ink:
            if 0 <= x < recipe.cell_w and 0 <= y < recipe.cell_h:
                canvas.set(x, y)
            else:
                lost_ink = True

        if lost_ink:
            built.dropped.append(codepoint)
        else:
            built.glyphs[codepoint] = canvas.frozen()

    if recipe.remap:
        original = dict(built.glyphs)
        for target, source in recipe.remap.items():
            built.glyphs[ord(target)] = original[ord(source)]

    # Some fonts map U+0020 to an empty outline FreeType skips; make sure
    # space exists, since the terminal fills cleared cells with it.
    built.glyphs.setdefault(0x20, blank(recipe.cell_w, recipe.cell_h))
    return built


# ---------------------------------------------------------------------------
# Box drawing (U+2500-U+257F), generated from Unicode character names.
#
# Rather than hand-typing a 128-entry table of (up, right, down, left)
# weights -- easy to get subtly wrong -- we parse the official names, e.g.
#   "BOX DRAWINGS LIGHT DOWN AND RIGHT"
#   "BOX DRAWINGS DOWN LIGHT AND RIGHT HEAVY"
#   "BOX DRAWINGS LEFT DOWN HEAVY AND RIGHT UP LIGHT"
#   "BOX DRAWINGS VERTICAL SINGLE AND LEFT DOUBLE"
# The name splits on "AND" into segments; each segment names directions and
# usually a weight (before or after them). A segment without a weight
# inherits the previous segment's ("LIGHT DOWN AND RIGHT").

WEIGHTS = {"LIGHT": 1, "SINGLE": 1, "HEAVY": 2, "DOUBLE": 3}
DIRECTIONS = {
    "UP": ("up",),
    "DOWN": ("down",),
    "LEFT": ("left",),
    "RIGHT": ("right",),
    "VERTICAL": ("up", "down"),
    "HORIZONTAL": ("left", "right"),
}


def parse_box_name(name: str) -> tuple[dict[str, int], str]:
    """Return ({arm: weight}, style) where style is plain/dashed/arc/diag."""
    words = name.removeprefix("BOX DRAWINGS ").split()
    style = "plain"
    if "DASH" in words:
        # "LIGHT DOUBLE DASH HORIZONTAL": the word before DASH is a dash
        # count, not a line weight, so drop both.
        index = words.index("DASH")
        del words[index - 1 : index + 1]
        style = "dashed"
    if "ARC" in words:
        words.remove("ARC")
        style = "arc"
    if "DIAGONAL" in words:
        return {}, "diag:" + " ".join(words)

    arms: dict[str, int] = {}
    previous_weight = 1
    for segment in " ".join(words).split(" AND "):
        tokens = segment.split()
        weight = next((WEIGHTS[t] for t in tokens if t in WEIGHTS), None)
        if weight is None:
            weight = previous_weight
        previous_weight = weight
        for token in tokens:
            for arm in DIRECTIONS.get(token, ()):
                arms[arm] = weight
    return arms, style


def draw_box(width: int, height: int, codepoint: int) -> Bitmap | None:
    name = unicodedata.name(chr(codepoint), "")
    if not name.startswith("BOX DRAWINGS"):
        return None
    arms, style = parse_box_name(name)
    canvas = Canvas(width, height)
    cx = (width - 1) // 2
    cy = (height - 1) // 2

    if style.startswith("diag:"):
        # One or two full-cell diagonals, Bresenham-free: step in y.
        upper_right = "UPPER RIGHT TO LOWER LEFT" in style or "CROSS" in style
        upper_left = "UPPER LEFT TO LOWER RIGHT" in style or "CROSS" in style
        for y in range(height):
            x = round(y * (width - 1) / max(height - 1, 1))
            if upper_left:
                canvas.set(x, y)
            if upper_right:
                canvas.set(width - 1 - x, y)
        return canvas.frozen()

    # Offsets of the parallel strokes making up one arm, by weight.
    strokes = {1: (0,), 2: (0, 1), 3: (-1, 1)}
    # How far an arm reaches past the centre so junctions close up.
    reach = {1: 0, 2: 1, 3: 1}
    join = max((reach[w] for w in arms.values()), default=0)

    def dashed_skip(position: int, length: int) -> bool:
        return style == "dashed" and (position * 4 // max(length, 1)) % 2 == 1

    for arm, weight in arms.items():
        for offset in strokes[weight]:
            if arm in ("up", "down"):
                x = cx + offset
                y_range = range(0, cy + join + 1) if arm == "up" else range(
                    cy - join, height
                )
                for y in y_range:
                    if not dashed_skip(y, height):
                        canvas.set(x, y)
            else:
                y = cy + offset
                x_range = range(0, cx + join + 1) if arm == "left" else range(
                    cx - join, width
                )
                for x in x_range:
                    if not dashed_skip(x, width):
                        canvas.set(x, y)
    # Arcs are drawn as square corners; at these cell sizes the difference
    # is one pixel and square corners join cleanly with neighbouring lines.
    return canvas.frozen()


# ---------------------------------------------------------------------------
# Block elements (U+2580-U+259F), also driven by names.

FRACTION_WORDS = {
    "ONE EIGHTH": 1,
    "ONE QUARTER": 2,
    "THREE EIGHTHS": 3,
    "HALF": 4,
    "FIVE EIGHTHS": 5,
    "THREE QUARTERS": 6,
    "SEVEN EIGHTHS": 7,
}


def draw_block(width: int, height: int, codepoint: int) -> Bitmap | None:
    name = unicodedata.name(chr(codepoint), "")
    canvas = Canvas(width, height)

    if name == "FULL BLOCK":
        canvas.rect(0, 0, width, height)
    elif name.endswith(" SHADE"):
        # 25 / 50 / 75 % ordered dither.
        for y in range(height):
            for x in range(width):
                light = x % 2 == 0 and y % 2 == 0
                medium = (x + y) % 2 == 0
                if (
                    (name == "LIGHT SHADE" and light)
                    or (name == "MEDIUM SHADE" and medium)
                    or (name == "DARK SHADE" and not light)
                ):
                    canvas.set(x, y)
    elif name.startswith("QUADRANT "):
        half_w = (width + 1) // 2
        half_h = (height + 1) // 2
        boxes = {
            "UPPER LEFT": (0, 0, half_w, half_h),
            "UPPER RIGHT": (half_w, 0, width, half_h),
            "LOWER LEFT": (0, half_h, half_w, height),
            "LOWER RIGHT": (half_w, half_h, width, height),
        }
        for part in name.removeprefix("QUADRANT ").split(" AND "):
            canvas.rect(*boxes[part])
    elif name.endswith(" BLOCK"):
        side, _, rest = name.removesuffix(" BLOCK").partition(" ")
        eighths = FRACTION_WORDS.get(rest)
        if eighths is None:
            return None
        rows = round(height * eighths / 8)
        cols = round(width * eighths / 8)
        if side == "UPPER":
            canvas.rect(0, 0, width, rows)
        elif side == "LOWER":
            canvas.rect(0, height - rows, width, height)
        elif side == "LEFT":
            canvas.rect(0, 0, cols, height)
        elif side == "RIGHT":
            canvas.rect(width - cols, 0, width, height)
        else:
            return None
    else:
        return None
    return canvas.frozen()


# ---------------------------------------------------------------------------
# DEC special graphics extras (VT100 line-drawing set, bytes 0x5F-0x7E).
# Box pieces and the checkerboard come from the groups above; this adds the
# scan lines, the diamond, and the two-letter control pictures. The VT100
# drew control pictures as two tiny letters on a diagonal; we do the same
# with a 3x5 micro font.

MICRO_FONT = {
    "C": ["###", "#..", "#..", "#..", "###"],
    "F": ["###", "#..", "##.", "#..", "#.."],
    "H": ["#.#", "#.#", "###", "#.#", "#.#"],
    "L": ["#..", "#..", "#..", "#..", "###"],
    "N": ["#.#", "###", "###", "###", "#.#"],
    "R": ["##.", "#.#", "##.", "#.#", "#.#"],
    "T": ["###", ".#.", ".#.", ".#.", ".#."],
    "V": ["#.#", "#.#", "#.#", "#.#", ".#."],
}
CONTROL_PICTURES = {
    0x2409: "HT",
    0x240A: "LF",
    0x240B: "VT",
    0x240C: "FF",
    0x240D: "CR",
    0x2424: "NL",
}
SCAN_LINES = {0x23BA: 1, 0x23BB: 3, 0x23BC: 7, 0x23BD: 9}


def draw_dec(width: int, height: int, codepoint: int) -> Bitmap | None:
    canvas = Canvas(width, height)
    if codepoint in SCAN_LINES:
        # The VT100 had 10 scan lines per character; line 5 is U+2500.
        row = (SCAN_LINES[codepoint] - 1) * height // 9
        canvas.hline(0, width - 1, min(row, height - 1))
    elif codepoint == 0x25C6:  # BLACK DIAMOND
        cx = (width - 1) / 2
        cy = (height - 1) / 2
        radius = min(width, height) / 2 - 0.5
        for y in range(height):
            for x in range(width):
                if abs(x - cx) / radius + abs(y - cy) / radius <= 1.0:
                    canvas.set(x, y)
    elif codepoint in CONTROL_PICTURES and width >= 7 and height >= 10:
        first, second = CONTROL_PICTURES[codepoint]
        for letter, x0, y0 in ((first, 0, 0), (second, width - 3, height - 5)):
            for dy, line in enumerate(MICRO_FONT[letter]):
                for dx, mark in enumerate(line):
                    if mark == "#":
                        canvas.set(x0 + dx, y0 + dy)
    else:
        return None
    return canvas.frozen()


SYNTH_GROUPS = {
    "box": (range(0x2500, 0x2580), draw_box),
    "block": (range(0x2580, 0x25A0), draw_block),
    "dec": (
        list(SCAN_LINES) + [0x25C6] + list(CONTROL_PICTURES),
        draw_dec,
    ),
}


def synthesize_missing(built: BuiltFont) -> None:
    recipe = built.recipe
    for group in recipe.synth:
        codepoints, drawer = SYNTH_GROUPS[group]
        for codepoint in codepoints:
            if codepoint in built.glyphs:
                continue
            bitmap = drawer(recipe.cell_w, recipe.cell_h, codepoint)
            if bitmap is not None:
                built.glyphs[codepoint] = bitmap
                built.synthesized.append(codepoint)


def missing_glyph(width: int, height: int) -> Bitmap:
    """Glyph 0: a hollow box, shown for codepoints the font can't draw."""
    canvas = Canvas(width, height)
    top, bottom = height // 6, height - 1 - height // 6
    canvas.hline(1, width - 2, top)
    canvas.hline(1, width - 2, bottom)
    canvas.vline(1, top, bottom)
    canvas.vline(width - 2, top, bottom)
    return canvas.frozen()


# ---------------------------------------------------------------------------
# PSF2 output


def encode_psf2(built: BuiltFont) -> bytes:
    recipe = built.recipe
    width, height = recipe.cell_w, recipe.cell_h
    row_bytes = (width + 7) // 8

    # Glyph 0 is the missing glyph; then unique bitmaps in codepoint order
    # (keeps ASCII at low indices, which helps when eyeballing dumps).
    bitmaps: list[Bitmap] = [missing_glyph(width, height)]
    mappings: list[list[int]] = [[]]
    index_of: dict[Bitmap, int] = {}
    for codepoint in sorted(built.glyphs):
        bitmap = built.glyphs[codepoint]
        if bitmap not in index_of:
            index_of[bitmap] = len(bitmaps)
            bitmaps.append(bitmap)
            mappings.append([])
        mappings[index_of[bitmap]].append(codepoint)

    glyph_bytes = bytearray()
    for bitmap in bitmaps:
        for row in bitmap:
            packed = 0
            for col, ink in enumerate(row):
                if ink:
                    packed |= 1 << (row_bytes * 8 - 1 - col)
            glyph_bytes += packed.to_bytes(row_bytes, "big")

    unicode_table = bytearray()
    for codepoints in mappings:
        for codepoint in codepoints:
            unicode_table += chr(codepoint).encode("utf-8")
        unicode_table.append(PSF2_SEPARATOR)

    underline = min(recipe.baseline + 1, height - 1)
    header = (
        PSF2_MAGIC
        + (0).to_bytes(4, "little")  # version
        + HEADER_SIZE.to_bytes(4, "little")
        + PSF2_HAS_UNICODE_TABLE.to_bytes(4, "little")
        + len(bitmaps).to_bytes(4, "little")
        + (row_bytes * height).to_bytes(4, "little")
        + height.to_bytes(4, "little")
        + width.to_bytes(4, "little")
        + EXT_MAGIC
        + bytes([EXT_VERSION, recipe.baseline, underline, 0])
    )
    assert len(header) == HEADER_SIZE
    return bytes(header + glyph_bytes + unicode_table)


def c_identifier(name: str) -> str:
    return "font_psf2_" + name.replace("-", "_")


def write_c_source(fonts: list[tuple[FontRecipe, bytes]]) -> None:
    """Emit every built font as a PSF2 image plus a lookup table.

    All fonts are compiled in (~110 KB of a 16 MB flash) so they can be
    switched at runtime; the default is listed first. They are stored as
    complete PSF2 images so built-in fonts go through exactly the same
    loader as fonts loaded at runtime.
    """
    fonts = sorted(fonts, key=lambda item: not item[0].default)
    lines = [
        "/*",
        " * fonts_builtin.c -- GENERATED by tools/ttf2psf.py; do not edit.",
        " * Regenerate with: cd tools && uv run ttf2psf.py",
        " *",
        " * Licenses of the embedded fonts:",
    ]
    for recipe, _ in fonts:
        lines.append(f" *   {recipe.name}: {recipe.license}")
    lines += [" */", '#include "font.h"', ""]
    for recipe, psf in fonts:
        lines.append(
            f"static const unsigned char {c_identifier(recipe.name)}[] = {{"
        )
        for start in range(0, len(psf), 12):
            chunk = psf[start : start + 12]
            lines.append("\t" + " ".join(f"0x{b:02x}," for b in chunk))
        lines += ["};", ""]
    lines.append("const struct font_builtin font_builtins[] = {")
    for recipe, psf in fonts:
        lines.append(
            f'\t{{ "{recipe.name}", {c_identifier(recipe.name)}, {len(psf)} }},'
        )
    lines += [
        "};",
        "",
        "const unsigned int font_builtin_count =",
        "    sizeof(font_builtins) / sizeof(font_builtins[0]);",
        "",
    ]
    C_OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    C_OUT_PATH.write_text("\n".join(lines))


# ---------------------------------------------------------------------------
# CLI


def load_recipes() -> list[FontRecipe]:
    config = tomllib.loads((TOOLS_DIR / "fonts.toml").read_text())
    recipes = []
    for entry in config["font"]:
        entry = dict(entry)
        entry["source"] = (TOOLS_DIR / entry["source"]).resolve()
        recipes.append(FontRecipe(**entry))
    return recipes


def build(recipe: FontRecipe) -> BuiltFont:
    built = rasterize_font(recipe)
    synthesize_missing(built)
    return built


def preview(built: BuiltFont, text: str) -> None:
    missing = missing_glyph(built.recipe.cell_w, built.recipe.cell_h)
    cells = [built.glyphs.get(ord(ch), missing) for ch in text]
    for row in range(built.recipe.cell_h):
        marker = "-" if row == built.recipe.baseline else " "
        line = "".join(
            "".join("#" if ink else "." for ink in cell[row]) + "|"
            for cell in cells
        )
        print(f"{row:2}{marker}{line}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--preview", nargs=2, metavar=("FONT", "TEXT"))
    args = parser.parse_args()
    recipes = load_recipes()

    if args.preview:
        name, text = args.preview
        recipe = next((r for r in recipes if r.name == name), None)
        if recipe is None:
            print(f"unknown font {name!r}", file=sys.stderr)
            return 1
        preview(build(recipe), text)
        return 0

    PSF_OUT_DIR.mkdir(parents=True, exist_ok=True)
    outputs = []
    for recipe in recipes:
        built = build(recipe)
        psf = encode_psf2(built)
        (PSF_OUT_DIR / f"{recipe.name}.psf").write_bytes(psf)
        cols, rows = 640 // recipe.cell_w, 480 // recipe.cell_h
        print(
            f"{recipe.name:20} {recipe.cell_w}x{recipe.cell_h} -> {cols}x{rows} "
            f"glyphs={len(built.glyphs):4} synth={len(built.synthesized):3} "
            f"dropped={len(built.dropped):4} bytes={len(psf)}"
        )
        outputs.append((recipe, psf))
    write_c_source(outputs)
    return 0


if __name__ == "__main__":
    sys.exit(main())

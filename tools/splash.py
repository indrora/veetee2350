#!/usr/bin/env python3
"""
splash.py -- the boot splash bitmap: make it, and bake it into firmware.

Why a splash at all: some HDMI capture cards (the ShadowCast 2 here) are
slow to lock onto a fresh boot whose screen is nearly all black, but lock
at once onto a busy picture such as SET-UP. So the first three seconds
after power-on show a small test card in the middle of the screen, on a
dark (not black) background (devlog 032).

Two steps, both from this script:

  uv run splash.py --generate   draw the default test card into
                                assets/splash.png (overwrites it)
  uv run splash.py              convert assets/splash.png into
                                lib/render/splash_image.c

The PNG is the source of truth: replace it with any 192x128 image and
re-run the second step. The converter quantises to at most 16 colours
(4 bits a pixel, so the image is 12 KB of flash) and puts the
top-left pixel's colour in palette entry 0: the firmware fills the rest
of the screen with it, so the image's border decides the background.

The firmware draws it 1:1 on the 480- and 540-line modes and doubled at
720p (lib/render/splash.c), centred.
"""

import argparse
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
PNG = ROOT / "assets" / "splash.png"
OUT_C = ROOT / "lib" / "render" / "splash_image.c"
FONT = ROOT / "fonts" / "psf" / "fmtowns-8x16.psf"

WIDTH, HEIGHT = 192, 128
COLOURS = 16

# The test card's palette. Entry 0 is the letterbox (see module comment).
NAVY = (16, 24, 72)
BLACK = (0, 0, 0)
WHITE = (255, 255, 255)
# SMPTE-style 75% bars, left to right.
BARS = [
    (192, 192, 192),  # grey (75% white)
    (192, 192, 0),  # yellow
    (0, 192, 192),  # cyan
    (0, 192, 0),  # green
    (192, 0, 192),  # magenta
    (192, 0, 0),  # red
    (0, 0, 192),  # blue
]


def load_psf2(path):
    """
    A PSF2 font as {codepoint: rows}, each row a list of bools. PSF2:
    a header (magic, version, header size, flags, glyph count, bytes per
    glyph, height, width), the glyph bitmaps, then -- with flag 1 -- a
    Unicode table: per glyph, UTF-8 strings separated by 0xFE (sequence)
    and ended by 0xFF.
    """
    data = path.read_bytes()
    (magic, _version, header_size, flags, count, glyph_bytes, height,
     width) = struct.unpack_from("<8I", data)
    if magic != 0x864AB572:
        sys.exit(f"{path}: not a PSF2 font")
    row_bytes = (width + 7) // 8
    glyphs = []
    for index in range(count):
        offset = header_size + index * glyph_bytes
        rows = []
        for row in range(height):
            bits = data[offset + row * row_bytes:offset + (row + 1) * row_bytes]
            value = int.from_bytes(bits, "big")
            rows.append([bool(value >> (row_bytes * 8 - 1 - col) & 1)
                         for col in range(width)])
        glyphs.append(rows)

    mapping = {}
    if flags & 1:
        table = data[header_size + count * glyph_bytes:]
        index = 0
        for entry in table.split(b"\xff")[:count]:
            # Single codepoints come before the first 0xFE sequence.
            singles = entry.split(b"\xfe")[0].decode("utf-8", "replace")
            for char in singles:
                mapping.setdefault(ord(char), glyphs[index])
            index += 1
    else:
        mapping = {index: glyph for index, glyph in enumerate(glyphs)}
    return mapping


def draw_text(draw, font, text, x, y, scale, colour):
    """Blit `text` in the PSF font at (x, y), each font pixel scale x scale."""
    for char in text:
        rows = font.get(ord(char), font.get(ord("?")))
        for row_index, row in enumerate(rows):
            for col_index, lit in enumerate(row):
                if lit:
                    left = x + col_index * scale
                    top = y + row_index * scale
                    draw.rectangle([left, top, left + scale - 1,
                                    top + scale - 1], fill=colour)
        x += len(rows[0]) * scale


def text_width(font, text, scale):
    return sum(len(font.get(ord(c), font[ord("?")])[0]) for c in text) * scale


def generate():
    """The default test card: bars, a castellated strip, a title panel."""
    image = Image.new("RGB", (WIDTH, HEIGHT), NAVY)
    draw = ImageDraw.Draw(image)
    font = load_psf2(FONT)

    # A navy frame (so the corner pixel, and the screen around the card,
    # is navy), then two thirds of the rest in seven bars. Integer edges
    # spread the remainder.
    frame = 4
    inner = WIDTH - 2 * frame
    bars_top = frame
    bars_bottom = 84
    for index, colour in enumerate(BARS):
        left = frame + index * inner // len(BARS)
        right = frame + (index + 1) * inner // len(BARS) - 1
        draw.rectangle([left, bars_top, right, bars_bottom - 1],
                       fill=colour)

    # A thin strip of the bars reversed, alternating with black, as on
    # the SMPTE card: hard vertical edges for the receiver to lock on.
    strip_bottom = bars_bottom + 8
    for index in range(len(BARS)):
        colour = BARS[len(BARS) - 1 - index] if index % 2 == 0 else BLACK
        left = frame + index * inner // len(BARS)
        right = frame + (index + 1) * inner // len(BARS) - 1
        draw.rectangle([left, bars_bottom, right, strip_bottom - 1],
                       fill=colour)

    # The title on the navy panel below, at the font's own size.
    title = "VeeTee 2350"
    scale = 1
    x = (WIDTH - text_width(font, title, scale)) // 2
    y = strip_bottom + (HEIGHT - strip_bottom - 16 * scale) // 2
    draw_text(draw, font, title, x, y, scale, WHITE)

    PNG.parent.mkdir(parents=True, exist_ok=True)
    image.save(PNG)
    print(f"wrote {PNG.relative_to(ROOT)}")


def rgb565(red, green, blue):
    return (red >> 3) << 11 | (green >> 2) << 5 | blue >> 3


def convert():
    """assets/splash.png -> lib/render/splash_image.c (4bpp + palette)."""
    image = Image.open(PNG).convert("RGB")
    if image.size != (WIDTH, HEIGHT):
        sys.exit(f"{PNG}: must be {WIDTH}x{HEIGHT}, is "
                 f"{image.size[0]}x{image.size[1]}")

    # Exact palette if the image already has <= 16 colours (the generated
    # card does), so entry order is first-seen from the top-left: the
    # letterbox colour is whatever the corner pixel is. Otherwise let
    # Pillow quantise, then move the corner's colour to entry 0.
    colours = image.getcolors(maxcolors=COLOURS)
    if colours is not None:
        seen = []
        for pixel in image.get_flattened_data():
            if pixel not in seen:
                seen.append(pixel)
        palette = seen
        pixels = [palette.index(p) for p in image.get_flattened_data()]
    else:
        quantised = image.quantize(colors=COLOURS, dither=Image.Dither.NONE)
        flat = quantised.getpalette()[:COLOURS * 3]
        palette = [tuple(flat[i:i + 3]) for i in range(0, len(flat), 3)]
        pixels = list(quantised.get_flattened_data())
        corner = pixels[0]
        order = [corner] + [i for i in range(len(palette)) if i != corner]
        remap = {old: new for new, old in enumerate(order)}
        palette = [palette[i] for i in order]
        pixels = [remap[p] for p in pixels]
    palette += [(0, 0, 0)] * (COLOURS - len(palette))

    # Two pixels a byte, the left one in the low nibble (as render.h's
    # tiles pack LSB-first).
    packed = bytearray()
    for index in range(0, len(pixels), 2):
        packed.append(pixels[index] | pixels[index + 1] << 4)

    lines = [
        "/*",
        " * splash_image.c -- GENERATED by tools/splash.py from",
        " * assets/splash.png. Do not edit: edit the PNG and re-run it.",
        " */",
        '#include "splash.h"',
        "",
        "const uint16_t splash_palette[SPLASH_COLOURS] = {",
    ]
    for red, green, blue in palette:
        lines.append(f"\t0x{rgb565(red, green, blue):04x},"
                     f"\t/* {red}, {green}, {blue} */")
    lines += ["};", "",
              "const uint8_t splash_pixels[SPLASH_HEIGHT * SPLASH_WIDTH / 2]"
              " = {"]
    for offset in range(0, len(packed), 16):
        chunk = packed[offset:offset + 16]
        lines.append("\t" + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines += ["};", ""]
    OUT_C.write_text("\n".join(lines))
    print(f"wrote {OUT_C.relative_to(ROOT)}: "
          f"{len(set(pixels))} colours, {len(packed)} bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--generate", action="store_true",
                        help="draw the default test card into the PNG first")
    args = parser.parse_args()
    if args.generate:
        generate()
    convert()


if __name__ == "__main__":
    main()

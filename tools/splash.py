#!/usr/bin/env python3
"""
splash.py -- the boot splash bitmap: make it, and bake it into firmware.

Why a splash at all: some HDMI capture cards (the ShadowCast 2 here) are
slow to lock onto a fresh boot whose screen is nearly all black, but lock
at once onto a busy picture such as SET-UP. So the first three seconds
after power-on show a test card in the middle of the screen, on a dark
(not black) background (devlog 032).

Two steps, both from this script:

  uv run splash.py --generate   draw the default test card into
                                assets/splash.png (overwrites it)
  uv run splash.py              convert assets/splash.png into
                                lib/render/splash_image.c

The PNG is the source of truth: replace it with any 256x256 image and
re-run the second step. It is stored as RGB565, exactly as the scanline
wants it (128 KB of flash, read straight from flash by the painter: no
palette, no copy). The top-right pixel's colour fills the rest of the
screen, so the image's border decides the background.

The firmware draws it 1:1 on the 480- and 540-line modes and doubled
(512x512) at 720p (lib/render/splash.c), centred.
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

WIDTH, HEIGHT = 256, 256

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
        for index, entry in enumerate(table.split(b"\xff")[:count]):
            # Single codepoints come before the first 0xFE sequence.
            singles = entry.split(b"\xfe")[0].decode("utf-8", "replace")
            for char in singles:
                mapping.setdefault(ord(char), glyphs[index])
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


def hue(position):
    """A fully saturated hue for position 0..1 (red, yellow, ... back)."""
    sector = position * 6
    fraction = sector % 1
    rising, falling = int(255 * fraction), int(255 * (1 - fraction))
    return [(255, rising, 0), (falling, 255, 0), (0, 255, rising),
            (0, falling, 255), (rising, 0, 255), (255, 0, falling)][
        int(sector) % 6]


def generate():
    """
    The default test card, top to bottom inside a navy frame: seven bars,
    the SMPTE reversed strip, a grey ramp, a hue sweep, and the title.
    Hard edges and smooth gradients both, so every line has detail.
    """
    image = Image.new("RGB", (WIDTH, HEIGHT), NAVY)
    draw = ImageDraw.Draw(image)
    font = load_psf2(FONT)

    frame = 8
    inner = WIDTH - 2 * frame

    def columns(index, count):
        """Integer column edges that spread the remainder."""
        return (frame + index * inner // count,
                frame + (index + 1) * inner // count - 1)

    bars_bottom = 136
    for index, colour in enumerate(BARS):
        left, right = columns(index, len(BARS))
        draw.rectangle([left, frame, right, bars_bottom - 1], fill=colour)

    strip_bottom = bars_bottom + 12
    for index in range(len(BARS)):
        colour = BARS[len(BARS) - 1 - index] if index % 2 == 0 else BLACK
        left, right = columns(index, len(BARS))
        draw.rectangle([left, bars_bottom, right, strip_bottom - 1],
                       fill=colour)

    # A black-to-white ramp, then a hue sweep: every column different.
    ramp_bottom = strip_bottom + 16
    sweep_bottom = ramp_bottom + 16
    for x in range(inner):
        level = round(255 * x / (inner - 1))
        draw.line([frame + x, strip_bottom, frame + x, ramp_bottom - 1],
                  fill=(level, level, level))
        draw.line([frame + x, ramp_bottom, frame + x, sweep_bottom - 1],
                  fill=hue(x / inner))

    # The title on the navy below, twice the font's size.
    title = "VeeTee 2350"
    scale = 2
    x = (WIDTH - text_width(font, title, scale)) // 2
    y = sweep_bottom + (HEIGHT - frame - sweep_bottom - 16 * scale) // 2
    draw_text(draw, font, title, x, y, scale, WHITE)

    PNG.parent.mkdir(parents=True, exist_ok=True)
    image.save(PNG)
    print(f"wrote {PNG.relative_to(ROOT)}")


def rgb565(red, green, blue):
    return (red >> 3) << 11 | (green >> 2) << 5 | blue >> 3


def convert():
    """assets/splash.png -> lib/render/splash_image.c (RGB565)."""
    image = Image.open(PNG).convert("RGB")
    if image.size != (WIDTH, HEIGHT):
        sys.exit(f"{PNG}: must be {WIDTH}x{HEIGHT}, is "
                 f"{image.size[0]}x{image.size[1]}")
    pixels = [rgb565(*pixel) for pixel in image.get_flattened_data()]

    lines = [
        "/*",
        " * splash_image.c -- GENERATED by tools/splash.py from",
        " * assets/splash.png. Do not edit: edit the PNG and re-run it.",
        " */",
        '#include "splash.h"',
        "",
        "/* The top-right pixel: the screen around the image. */",
        f"const uint16_t splash_background = 0x{pixels[WIDTH - 1]:04x};",
        "",
        "/* Word-aligned: the painter reads two pixels at a time. */",
        "const uint16_t splash_pixels[SPLASH_HEIGHT * SPLASH_WIDTH]",
        "    __attribute__((aligned(4))) = {",
    ]
    for offset in range(0, len(pixels), 16):
        chunk = pixels[offset:offset + 16]
        lines.append("\t" + ", ".join(f"0x{p:04x}" for p in chunk) + ",")
    lines += ["};", ""]
    # Only write when something changed: the build runs this every time
    # (tools/pio_splash.py), and a rewritten file -- even identical --
    # gets its 130 KB of C recompiled.
    text = "\n".join(lines)
    name = OUT_C.relative_to(ROOT)
    if OUT_C.exists() and OUT_C.read_text() == text:
        print(f"{name}: up to date")
        return
    OUT_C.write_text(text)
    print(f"wrote {name}: {len(pixels) * 2} bytes")


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

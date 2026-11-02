#!/usr/bin/env python3
"""Build the contents of a VeeTee 2350 SD card in a folder.

    cd tools && uv run sd_card.py [--out ../sd] [--schemes PATH]

Then copy everything inside the output folder to the root of a FAT32 or
exFAT card. The result:

    fonts/      64 PSF2 fonts converted from The Ultimate Oldschool PC
                Font Pack (../fonts/TUOPFP, CC BY-SA 4.0, by VileR), with
                its license
    schemes/    64 base16 colour schemes from tinted-theming/schemes
                (MIT), as-is, with its license

The firmware lists only the first 64 of each it finds, so that's all
this writes: a favourites list first, then alphabetical.
    screens/    where F12 screenshots go
    print/      where the printer writes

Fonts: every TUOPFP pixel-outline TTF ("ttf - Px") whose cell is 8 px
wide, plus the 9-px ones (VGA and friends) whose ninth column is only
ever spacing or line-graphics extension: those are cropped to 8 exactly
as the built-in rainbow100-80 is, and box/block glyphs that needed the
ninth column are re-synthesized by ttf2psf. A font is skipped if any
printable ASCII glyph doesn't fit. Where a "PxPlus" (Unicode-extended)
version exists it is used instead of the "Px437" (code page 437) one,
since the terminal speaks UTF-8.

The grid of each TTF is found from its outlines: TUOPFP draws one
design pixel as a fixed number of font units (100 across; 200 down for
fonts whose originals doubled their scan lines), so the GCD of the
outline coordinates is the pixel size.

Schemes: tinted-theming's base16/*.yaml, copied unchanged (the firmware
parses them), except that names longer than the firmware's 31-character
limit are shortened. Without --schemes the repository is cloned (git,
shallow) into a temporary folder.
"""

from __future__ import annotations

import argparse
import math
import shutil
import subprocess
import sys
import tempfile
from functools import reduce
from pathlib import Path

import freetype

import ttf2psf

TOOLS_DIR = Path(__file__).resolve().parent
TUOPFP = TOOLS_DIR.parent / "fonts" / "TUOPFP"
PX_DIR = TUOPFP / "ttf - Px (pixel outline)"
SCHEMES_REPO = "https://github.com/tinted-theming/schemes"

# The firmware lists only the first 64 fonts and 64 schemes it finds
# (src/card.h, CARD_FONTS_MAX / CARD_SCHEMES_MAX), so the card gets at
# most that many: these favourites first, then alphabetical order.
LIMIT = 64
FAVOURITE_FONTS = [
    "IBM_VGA_8x16", "IBM_VGA_9x16", "IBM_VGA_8x14", "IBM_EGA_8x14",
    "IBM_EGA_9x14", "IBM_CGA-2y", "IBM_CGAthin-2y", "IBM_MDA",
    "IBM_BIOS-2y", "IBM_PGC", "IBM_Model3x_Alt1", "Amstrad_PC-2y",
    "ATI_8x16", "ATT_PC6300", "CompaqThin_8x16", "CL_Stingray_8x19",
    "DTK_BIOS-2y", "EuroPC_Mono", "HP_100LX_8x8", "ITT_Xtra-2y",
    "Kaypro2K_G-2y", "NEC_APC3_8x16", "OlivettiThin_8x16",
    "PhoenixVGA_8x16", "Phoenix_BIOS-2y", "SanyoMBC16-2y", "Sharp_PC3K",
    "Siemens_PC-D", "Sigma_RM_8x16", "SperryPC_8x16", "Tandy1K-II_225L-2y",
    "Tandy2K", "Ti_Pro", "ToshibaSat_8x16", "ToshibaSat_9x16",
    "Verite_8x16", "Zenith_Z-100",
]
FAVOURITE_SCHEMES = [
    "default-dark", "default-light", "dracula", "nord", "nord-light",
    "gruvbox-dark-hard", "gruvbox-dark-medium", "gruvbox-light-medium",
    "solarized-dark", "solarized-light", "monokai", "tomorrow",
    "tomorrow-night", "tomorrow-night-eighties", "eighties", "ocean",
    "onedark", "one-light", "material", "material-darker",
    "material-palenight", "catppuccin-mocha", "catppuccin-macchiato",
    "catppuccin-frappe", "catppuccin-latte", "tokyo-night-dark",
    "tokyo-night-storm", "tokyo-night-light", "rose-pine", "rose-pine-moon",
    "rose-pine-dawn", "everforest", "kanagawa", "zenburn", "github-dark",
    "github", "ayu-dark", "ayu-mirage", "ayu-light", "papercolor-dark",
    "papercolor-light", "selenized-dark", "selenized-light",
    "oxocarbon-dark", "spacemacs", "railscasts", "twilight", "jellybeans",
    "irblack", "3024", "greenscreen", "linux-vt", "windows-95",
    "windows-nt", "macintosh", "classic-dark", "classic-light",
    "grayscale-dark", "grayscale-light", "black-metal", "outrun-dark",
    "synth-midnight-dark", "unikitty-dark", "xcode-dusk",
]


def pick(names: list[str], favourites: list[str]) -> list[str]:
    """Favourites that exist, then the rest alphabetically, up to LIMIT."""
    present = set(names)
    chosen = [n for n in favourites if n in present]
    rest = sorted((n for n in names if n not in set(chosen)), key=str.lower)
    return (chosen + rest)[:LIMIT]


NAME_MAX = 28  # firmware: SETUP_CARD_NAME_MAX - 1 (font names)
SCHEME_NAME_MAX = 31  # firmware: SETTINGS_SCHEME_MAX - 1
CELL_HEIGHT_MAX = 32  # firmware: RENDER_MAX_CELL_HEIGHT


def pixel_grid(face: freetype.Face) -> tuple[int, int]:
    """Font units per design pixel, across and down."""
    xs: set[int] = set()
    ys: set[int] = set()
    for ch in "MW@#gQ%":
        try:
            face.load_char(ch, freetype.FT_LOAD_NO_SCALE)
        except freetype.FT_Exception:
            continue
        for x, y in face.glyph.outline.points:
            xs.add(abs(x))
            ys.add(abs(y))
    unit_x = reduce(math.gcd, [x for x in xs if x] or [1])
    unit_y = reduce(math.gcd, [y for y in ys if y] or [1])
    return unit_x, unit_y


def recipe_for(path: Path) -> ttf2psf.FontRecipe | None:
    """A ttf2psf recipe rendering one design pixel per screen pixel."""
    face = freetype.Face(str(path))
    unit_x, unit_y = pixel_grid(face)
    if unit_y % unit_x:
        return None  # non-integer aspect: not a pixel font we can place
    face.load_char("M", freetype.FT_LOAD_NO_SCALE)
    width = face.glyph.advance.x // unit_x
    # At ppem = units_per_EM / unit_x, one unit_x is one pixel; tall
    # pixels (unit_y = 2 unit_x) come out as two rows each.
    height = (face.ascender - face.descender) // unit_x
    baseline = face.ascender // unit_x
    if width not in (8, 9) or not 6 <= height <= CELL_HEIGHT_MAX:
        return None
    stem = path.stem.split("_", 1)[1]  # drop "Px437_" / "PxPlus_"
    return ttf2psf.FontRecipe(
        name=stem[:NAME_MAX],
        source=path,
        ppem=face.units_per_EM // unit_x,
        cell_w=8,
        cell_h=height,
        baseline=baseline,
        align="origin",
        synth=["box", "block", "dec"],
        license=f"{path.stem} from The Ultimate Oldschool PC Font Pack by "
        "VileR (int10h.org), CC BY-SA 4.0 (converted to PSF2"
        + (", cropped to 8 px)" if width == 9 else ")"),
    )


def build_fonts(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    ttfs = sorted(PX_DIR.glob("*.ttf"))
    extended = {p.name[len("PxPlus_"):] for p in ttfs if p.name.startswith("PxPlus_")}
    usable: dict[str, bytes] = {}
    skipped = 0
    for path in ttfs:
        if path.name.startswith("Px437_") and path.name[len("Px437_"):] in extended:
            continue  # the PxPlus version covers it
        recipe = recipe_for(path)
        if recipe is None:
            skipped += 1
            continue
        built = ttf2psf.build(recipe)
        lost = [c for c in built.dropped if 0x20 < c < 0x7F]
        if lost:
            skipped += 1
            continue
        usable[recipe.name] = ttf2psf.encode_psf2(built)
    for old in out.glob("*.psf"):
        old.unlink()
    chosen = pick(list(usable), FAVOURITE_FONTS)
    for name in chosen:
        (out / f"{name}.psf").write_bytes(usable[name])
    shutil.copy(TUOPFP / "LICENSE.TXT", out / "LICENSE.TXT")
    print(f"fonts: {len(chosen)} written of {len(usable)} usable "
          f"({skipped} not 8/9 px wide, or ASCII that doesn't fit)")


def build_schemes(out: Path, repo: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    available = {p.stem: p for p in (repo / "base16").glob("*.yaml")}
    for old in out.glob("*.yaml"):
        old.unlink()
    chosen = pick(list(available), FAVOURITE_SCHEMES)
    for name in chosen:
        shutil.copy(available[name], out / f"{name[:SCHEME_NAME_MAX]}.yaml")
    shutil.copy(repo / "LICENSE", out / "LICENSE")
    print(f"schemes: {len(chosen)} copied of {len(available)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=TOOLS_DIR.parent / "sd")
    parser.add_argument("--schemes", type=Path,
                        help="a tinted-theming/schemes checkout (else cloned)")
    args = parser.parse_args()

    build_fonts(args.out / "fonts")
    if args.schemes is not None:
        build_schemes(args.out / "schemes", args.schemes)
    else:
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(["git", "clone", "-q", "--depth", "1",
                            SCHEMES_REPO, tmp + "/schemes"], check=True)
            build_schemes(args.out / "schemes", Path(tmp) / "schemes")
    for folder in ("screens", "print"):
        (args.out / folder).mkdir(parents=True, exist_ok=True)
    print(f"done: copy the contents of {args.out} to the card's root")
    return 0


if __name__ == "__main__":
    sys.exit(main())

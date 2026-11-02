# Plan: SD card (phase F)

Goal (from the user, 2026-11-02): load fonts and colour schemes from an
SD card, dump screenshots to it, "turn on the printer" into it, and
(stretch) update the firmware from a file on it. Plus new built-in
fonts, base16 schemes ready to copy onto a card, and a loud
"Configuration Error" screen when something on the card is bad.

## Decisions

| | Choice | Why |
|---|---|---|
| Library | arduino-pico's bundled `SD` (SdFat 2.x), SPI mode | The framework's standard, FAT32 + exFAT. No PIO or DMA, unaffected by our per-mode sysclk. |
| Wiring | SPI1: SCK GP30, MOSI GP31, MISO GP40, CS GP43 (GPIO) | The PiZero's TF slot (SDIO CLK/CMD/D0/D3 nets). GP41/42 unused in SPI mode. |
| Fonts on card | PSF2 only, `/fonts/*.psf` | Already our format; `tools/ttf2psf.py` converts on the host. |
| Schemes on card | base16 YAML, `/schemes/*.yaml` | Parsed on the device (flat `baseXX: "rrggbb"` keys), copied as-is from tinted-theming/schemes (MIT). |
| Screenshots | F12 on the keyboard port -> `/screens/NNNN.bmp` | 16-bit BMP (RGB565 bitfields), rendered line by line by the real renderer. |
| Firmware update | Last, after the rest works | A/B flash images via the RP2350 boot ROM; designed once the SD layer is proven. |

The SD code is C++ (SdFat), so like `usbio` it sits behind a small C API
(`lib/hal/storage.h`), with the C++ in a shim next to `src/main.cpp`.
The card is mounted on demand (SET-UP open, F12, printer on), never in
the render path. No card-detect pin exists (DAT3 is CS), so "no card"
is a failed mount.

## Steps (one devlog entry and captures each)

1. **Built-in fonts.** Add TUOPFP's Rainbow100 re.80, Nix8810 M16, and
   FM Towns re. 8x8 and 8x16 to `tools/fonts.toml` (from the `Px437_*`
   pixel TTFs). CC BY-SA 4.0: credit int10h.org (VileR) in
   `fonts_builtin.c` and the docs, marked as converted.
2. **Storage HAL.** `storage_mount/open/read/write/close/list/mkdir`,
   SPI1 bring-up, a SET-UP status line ("SD: 7.4 GB FAT32" / "no card").
   Host tests use a directory-backed fake.
3. **Configuration Error screen.** Full-screen, double-height red
   banner, the file and the reason ("/fonts/foo.psf: not a PSF2 font"),
   "press any key: defaults loaded". Used by every loader below, and by
   the crash-guard fallback (which today falls back silently).
4. **Fonts from SD.** SET-UP's Font item lists the built-ins, then the
   card's `/fonts`. The chosen name is saved (`sd:name`). At boot, a
   missing or bad card font gives a Configuration Error and the
   default.
5. **Colour schemes from SD.** Base16 -> terminal mapping as
   base16-shell does it (ANSI 0-15 from base00/08/0B/0A/0D/0E/0C/05,
   brights from base03/08..0E/07, fg base05, bg base00). SET-UP lists the
   built-in schemes, then the card's. `tools/sd_card.py` fills an
   `sd/` folder: schemes from tinted-theming, fonts as PSF.
6. **Screenshots.** F12 -> next free `/screens/NNNN.bmp`, with a visual
   bell flash as confirmation. Works in SET-UP too.
7. **Printer.** SET-UP "Printer: off / SD card". Media Copy: `CSI i`
   print screen, `CSI ? 1 i` print cursor line, `CSI ? 5 i` / `? 4 i`
   auto print, `CSI 5 i` / `4 i` printer controller (bytes go to the
   file, not the screen). DSR `CSI ? 15 n` reports ready / no printer.
   Each "power on" opens `/print/NNNN.txt`.
8. **Firmware update (stretch).** `/firmware/veetee.uf2` or `.bin`,
   offered in SET-UP. Verify, write to the other flash slot, then boot
   it through the ROM's try-before-you-buy, so a bad image falls back.

Settings grow a scheme name and a printer setting (`SETTINGS_VERSION`
6); fonts already store a name.

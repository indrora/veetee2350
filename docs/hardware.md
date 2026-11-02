# Hardware notes: Waveshare RP2350-PiZero

## Board

- RP2350**B** (48 GPIO), 16 MB flash, **no PSRAM** (pad unpopulated).
  That leaves 520 KiB SRAM.
- arduino-pico board ID: `waveshare_rp2350_pizero`. The board was added
  upstream after early 2025, so the PlatformIO platform must be up to date.
- USB: native USB-C (CDC/programming) plus a PIO-USB port (host, Goal 3).

## DVI (mini-HDMI)

| Signal  | GPIO (P / N) | PicoDVI lane |
|---------|--------------|--------------|
| D0 blue | 36 / 37      | 0            |
| D1 green| 34 / 35      | 1            |
| D2 red  | 32 / 33      | 2            |
| CLK     | 38 / 39      | PWM slice    |
| DDC SDA / SCL | 44 / 45 | not used yet |
| CEC     | 46           | not used yet |

Sources: CheshirCa/HDMI_Tester `src/video/video_pins.h` (which says it
was checked against the Waveshare schematic) and
DnCraptor/pico-mac- `drivers/libdvi/common_dvi_pin_configs.h`. They agree.

The pairs are not inverted. GPIO ≥ 32 needs `pio_set_gpio_base(pio0, 16)`
plus 64-bit pin masks in the serialiser (lib/PicoDVI/PATCHES.md). Once
rebased, **pio0 cannot reach GPIO 0–15**.

## Scanout budget (measured 2026-11-01)

640×480p60 at 252 MHz sysclk, VREG 1.20 V. Each line has 8000 cycles.

| Config                                   | Encode | Line avg | Line max |
|------------------------------------------|--------|----------|----------|
| 1 symbol/word, no unroll                 | 9 224  | 10 121   | 10 738   |
| 2 symbols/word, unroll 4, PROC1 bus prio | 4 392  |  5 173   |  5 845   |

The second row's line figures are for the test pattern, whose render
step is just a memcpy plus border (about 780 cycles). That leaves
roughly **2 100 cycles of worst-case headroom per line** for the text
renderer on core 1, or about 26 cycles per 8-pixel cell at 80 columns.

## Capture

The Genki ShadowCast 2 is avfoundation device 2. It upscales
640×480 to 2560×1440 (stretched to 16:9). Grab a frame with:

    ffmpeg -f avfoundation -framerate 30 -i "2" -frames:v 1 capture.jpg

This has to run from a terminal that has macOS camera permission. The
sandboxed agent shell doesn't, so use the user's Terminal panel.

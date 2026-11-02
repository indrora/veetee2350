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
step is just a memcpy plus border (about 780 cycles).

### Text renderer (80 columns, every cell inked, 1 in 4 underlined)

Render cost was measured on idle core 0, so with no DMA and no IRQs.

| Renderer version                                  | Render | Line avg | Line max |
|---------------------------------------------------|--------|----------|----------|
| -Os, helper out-lined into flash via a veneer     |  ~6300 | 10 968   | 12 123   |
| -O2 + always_inline, nibble masks, attr call      |   3770 |  8 296   |  9 618   |
| byte masks, inline attr (and, or) table           |   3032 |  7 407   |  8 074   |

Lessons:
- The core builds at -Os, which out-lines `static inline` helpers into
  flash. That's why hot helpers use `HOT_INLINE` and lib/render builds
  at -O2.
- An out-of-line call for attributed cells cost about 35 cycles per
  cell.
- Running render_scanline from flash instead of RAM changed nothing
  (3740 vs 3770), so this loop is not limited by bus contention between
  instruction fetch and data access.

The current renderer fits at 60 fps on average, but the worst line can
slightly exceed the budget.

### Split pipeline: render on core 0, encode on core 1 (current)

Core 0 renders into an 8-line ring from a top-priority doorbell IRQ.
Core 1 only encodes. Measured with the `pizero-stats` env on the same
worst-case screen:

| Core | Work | Avg | Max | Budget |
|------|------|-----|-----|--------|
| 1 | TMDS encode | 4 348 | 4 348 | 8 000 |
| 0 | text render (IRQ) | 3 657 | 3 728 | 8 000 |

0 starved lines and a steady 60 fps. Core 1 has about 46% headroom for
graphics overlays. Rendering takes about 42% of core 0, and the
terminal and USB run in the rest.

Gotcha: arduino-pico hooks the shared SIO_IRQ_BELL even in single-core
mode, and watches doorbell 0 with an "idle forever" handler. The video
ring therefore uses doorbell 7.

## Capture

The Genki ShadowCast 2 upscales 640×480 to 2560×1440 (stretched to
16:9). Its avfoundation index moves around between sessions, so select it
by name:

    ffmpeg -f avfoundation -framerate 30 -i "ShadowCast 2" -frames:v 1 capture.jpg

This has to run from a terminal that has macOS camera permission. The
sandboxed agent shell doesn't, so use the user's Terminal panel.

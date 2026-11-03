# Hardware notes: Waveshare RP2350-PiZero

## Board

- RP2350**B** (48 GPIO), 16 MB flash, **no PSRAM** (pad unpopulated).
  That leaves 520 KiB SRAM.
- arduino-pico board ID: `waveshare_rp2350_pizero`. The board was added
  upstream after early 2025, so the PlatformIO platform must be up to date.
- USB: native USB-C (CDC line + programming; debug builds add the
  automation HID, devlog 029) plus a PIO-USB port: D+ GPIO 28,
  D- GPIO 29, VBUS straight from 5 V with no switch (so a device plugged
  in while running can brown the chip out). Host via Pico-PIO-USB on
  PIO 1 (transmit) and PIO 2 (receive); `lib/hal/usbhost.c`, devlog 027.

## 40-pin header

GPIO 0–27, all unused today. **Not BCM-numbered everywhere**: Waveshare
moved eight GPIOs so pins 8/10 carry UART1 TX/RX (GPIO 4/5), like a Pi's
serial port. The header map, the planned HAT (an RS-232 line on UART1
and a PS/2 keyboard on pio0) and its pin assignment are in `docs/hat.md`.

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

## Output resolutions (SET-UP → Resolution)

All of these are verified on the Genki. Changing the resolution saves
the settings and reboots, because the clock, VREG and DVI timing are
only set at boot.

| Mode | sysclk | VREG | Cycles/line | Grid (8×16 font) |
|------|--------|------|-------------|------------------|
| 640×480 @ 60 | 252 MHz | 1.20 V | 8 000 | 80×30 |
| 720×480 @ 60 | 270 MHz | 1.20 V | 8 580 | 90×30 |
| 800×480 @ 60 | 295 MHz | 1.20 V | 9 920 | 100×30 |
| 960×540 @ 60 | 372 MHz | 1.30 V | 11 040 | 120×33 |
| 1280×720 @ 30 | 372 MHz | 1.30 V | 16 500 | 160×45 |
| 1280×720 @ 24 | 297 MHz | 1.20 V | 16 500 | 160×45 |

At 720×480, encode is 4 977 and render 4 083 of the 8 580 cycles (and
the other modes scale the same way). 1280×720 @ 24 is the 720p30 frame
with a slower pixel clock: the per-line budget is the same, but the
overclock is far gentler.

### Flash (QSPI) timing at high clocks

The flash clock is sysclk / CLKDIV, and the flash is rated to 133 MHz.
`board_flash_timing()` picks CLKDIV = ⌈sysclk/133 MHz⌉ (min 2), and
RXDELAY = CLKDIV, plus 1 if the flash clock is above 100 MHz. A
`pizero-flashprobe` build re-reads the firmware image uncached at every
RXDELAY and reports which ones work:

| sysclk | CLKDIV | Working RXDELAY | Chosen |
|--------|--------|-----------------|--------|
| 252 MHz | 2 | 1–4 | 3 |
| 270 MHz | 3 | 1–5 | 3 |
| 295 MHz | 3 | 1–6 | 3 |
| 372 MHz | 3 | 2–6 | 4 |

Two traps:
1. **`set_sys_clock_khz()` runs from flash** and briefly parks clk_sys
   on the 48 MHz USB PLL. RXDELAY 4 is too late at 48 MHz, so 960×540
   hard-faulted inside `clock_configure`. During the switch we use a
   transition timing (CLKDIV + 1, RXDELAY 3) that works at every clock it
   passes through.
2. **`flash_range_erase/program` re-enter XIP with boot defaults**
   (CLKDIV 2). `flashcfg_write()` runs from RAM and restores the timing
   before anything fetches from flash.

### Crash guard

An 8 s watchdog is fed from the main loop. Two watchdog-timeout resets
in a row boot the next session on factory defaults; the saved settings
are left alone. A HardFault handler records PC/LR (plus r0, r2, r5)
in watchdog scratch registers, and the next boot prints them on the
banner (and over serial in `pizero-stats`), ready for `addr2line`.

## Capture

The Genki ShadowCast 2 upscales 640×480 to 2560×1440 (stretched to
16:9). Its avfoundation index moves around between sessions, so select it
by name:

    ffmpeg -f avfoundation -framerate 30 -i "ShadowCast 2" -frames:v 1 capture.jpg

This has to run from a terminal that has macOS camera permission. The
sandboxed agent shell doesn't, so use the user's Terminal panel.

## SD card slot

SPI1 on GP30 (SCK), GP31 (MOSI), GP40 (MISO), GP43 (CS); see
`lib/hal/storage.h`. Boot brings video up first and touches the card
only afterwards.

Capture caveat: when a boot probes an *empty* slot (saved settings name
card fonts/schemes, card pulled), the Genki ShadowCast 2 loses the
picture and stays at "no signal" across board reboots until it's
unplugged and replugged. The board is fine meanwhile -- the stats build
reports 60 frames/s, 0 starved lines -- and after the replug the screen
is there. A failed SD init clocks GP30/31 for about two seconds of
retries, next to TMDS lane 2 on GP32/33; whether that's the trigger, or
the grabber just dislikes something about that boot, isn't known. (The
same "wedged until replug" happened once after a 1280x720 @ 24 test.)

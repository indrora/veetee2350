# pico_pio_usb provenance

USB host (and device) in software on the RP2040/RP2350's PIO, used by
`lib/hal/usbhost.c` for keyboards on the board's PIO-USB port.

## Upstream

- Project: Pico-PIO-USB by sekigon-gonnoc,
  https://github.com/sekigon-gonnoc/Pico-PIO-USB
- Version: 0.5.2 (`library.properties`)
- License: MIT (`LICENSE`)

## Where this copy came from

Waveshare's RP2350-PiZero demo package, `C/02-USB/` (the user put it in
`external/upstream-demos/`, 2026-11-02), chosen over fetching a newer
upstream release. `src/`, `LICENSE`, `README.md` and `library.properties`
are copied as they were. The only Waveshare change we know of is the
default D+ pin, `PIO_USB_DP_PIN_DEFAULT` 28 in
`pio_usb_configuration.h`, which this board needs anyway (and
`usbhost.c` sets it explicitly).

## Added here

- `src/usb_rx.pio.h`, `src/usb_tx.pio.h`: generated from the `.pio`
  sources with the toolchain's pioasm 2.3.0 (`pioasm -o c-sdk`), since
  PlatformIO doesn't run pioasm the way the demo's CMake build does.
  Regenerate them if the `.pio` files change.
- `library.json`: a PlatformIO manifest (sources and headers in `src/`).

## Local modifications

Listed in `PATCHES.md` beside this file, as with PicoDVI and libvterm.

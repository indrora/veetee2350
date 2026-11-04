# VeeTee 2350 — notes for coding assistants

- Read `STYLE.md` before writing code. It is the authority on style,
  layering, and embedded rules.
- Project goals: `GOAL.md`. Design notes and spike reports: `docs/`.
- Build firmware: `pio run -e pizero`. Host tests: `pio test -e native`.
- Hardware: Waveshare RP2350-PiZero (RP2350B). DVI TMDS on GPIO 32–39
  (PIO GPIO base must be 16), see `lib/hal/video.c`. HAT pinout (UART,
  PS/2) and the header map: `docs/hat.md` -- firmware for those follows
  it. The header is NOT BCM-numbered everywhere; use that map.
- Run a real terminal session through the board: `cd tools && python3
  hostshell.py /dev/cu.usbmodem*` (see docs/terminal.md). Over the
  header's UART (SET-UP > Communications > Port: UART), `cd tools && uv
  run getty.py /dev/cu.usbserial-* --baud 115200`. Always the `/dev/cu.*`
  node: opening `/dev/tty.*` waits for carrier detect and can hang
  unkillably until the adapter is unplugged (devlog 030).
- Drive the board yourself (keys, BOOTSEL, debug text): flash a debug
  build (`pio run -e pizero-debug -t upload`), then `cd tools && uv run
  sendkeys.py 'key:ctrl+F3' 'wait:-> hotkey' ...` through its automation
  HID (devlog 029). Debug text comes back over the HID (`--log`), never
  down the line. Never leave the virtual BOOTSEL down: 5 s is a factory
  reset of the user's settings.
- Fonts: edit `tools/fonts.toml`, then `cd tools && uv run ttf2psf.py`
  regenerates `fonts/psf/` and `lib/font/fonts_builtin.c`.
- Boot splash: replace `assets/splash.png` (192x128, <= 16 colours), then
  `cd tools && uv run splash.py` regenerates `lib/render/splash_image.c`.
- Check the screen: `ffmpeg -f avfoundation -framerate 30 -i "ShadowCast 2"
  -frames:v 1 capture.jpg` from the user's Terminal panel (needs camera
  permission the agent sandbox lacks).
- Keep the devlog: every micro-milestone gets `docs/devlog/NNN-slug.md`
  with a capture in `docs/devlog/img/` and a short blog-style write-up,
  linked from `docs/devlog/README.md`.

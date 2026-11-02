# VeeTee 2350 — notes for coding assistants

- Read `STYLE.md` before writing code. It is the authority on style,
  layering, and embedded rules.
- Project goals: `GOAL.md`. Design notes and spike reports: `docs/`.
- Build firmware: `pio run -e pizero`. Host tests: `pio test -e native`.
- Hardware: Waveshare RP2350-PiZero (RP2350B). DVI TMDS on GPIO 32–39
  (PIO GPIO base must be 16), see `lib/hal/video.c`.
- Run a real terminal session through the board: `cd tools && python3
  hostshell.py /dev/cu.usbmodem*` (see docs/terminal.md).
- Fonts: edit `tools/fonts.toml`, then `cd tools && uv run ttf2psf.py`
  regenerates `fonts/psf/` and `lib/font/fonts_builtin.c`.
- Check the screen: `ffmpeg -f avfoundation -framerate 30 -i "ShadowCast 2"
  -frames:v 1 capture.jpg` from the user's Terminal panel (needs camera
  permission the agent sandbox lacks).

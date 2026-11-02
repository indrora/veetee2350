# VeeTee 2350 — notes for coding assistants

- Read `STYLE.md` before writing code. It is the authority on style,
  layering, and embedded rules.
- Project goals: `GOAL.md`. Design notes and spike reports: `docs/`.
- Build firmware: `pio run -e pizero`. Host tests: `pio test -e native`.
- Hardware: Waveshare RP2350-PiZero (RP2350B). DVI TMDS on GPIO 32–39
  (PIO GPIO base must be 16), see `lib/hal/video.c`.

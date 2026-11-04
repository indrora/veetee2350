# 032 — A boot splash for the capture card

*2026-11-04*

![The boot splash at 640x480: a small test card in the middle of a navy screen, with seven colour bars, a strip of reversed bars and "VeeTee 2350" underneath](img/032-splash.jpg)

On a fresh boot the ShadowCast capture card sometimes wouldn't lock onto
the picture. But tap BOOTSEL to bring up SET-UP and it locked every
time. The one obvious difference is the picture. A fresh boot is a black
screen with three lines of banner, while SET-UP fills most of the screen.
So the first three seconds now show a small test card instead of the
terminal.

## The card

`tools/splash.py --generate` draws it: 192x108 pixels, seven 75% colour
bars, the reversed strip from the SMPTE card, and the title in the
FM Towns 8x16 font. It has a navy frame, and the screen around the card
takes the frame's colour, so no line of the frame is black.

The PNG it writes (`assets/splash.png`) is the source. Replace it with
any 192x108 image and run `uv run splash.py` again. That converts it to
at most 16 colours, 4 bits a pixel (10 KB), into
`lib/render/splash_image.c`. The image's top-left pixel decides the
background colour.

## Painting it

`lib/render/splash.c` is a second scanline function, with the same shape
as `render_scanline`. Lines above and below the card are a word fill.
A line through the card is a palette lookup per source pixel, written
two pixels to a 32-bit store. At 720p each pixel is doubled, so the card
is 384x216. Elsewhere it's 1:1.

Swapping it in and out costs the text renderer nothing:

- `video_set_scanline()` changes which function the render IRQ calls.
  The function and its argument are two stores, so the swap masks
  interrupts. The IRQ never sees the new function with the old argument.
- `render.live` stays false while the splash is up. `render_pause`
  waits for the renderer's frame counter to move, and it doesn't move
  while the splash draws. With `live` false, a pause returns at once
  instead of hanging.

## Borrowed memory

Reading the pixels from flash inside the render IRQ risks an XIP cache
miss mid-line. So `splash_layout` copies them into RAM first. The RAM is
SET-UP's screen (`setup_cells`, 19 KB): nothing uses it at boot, and
the 10 KB of pixels fits. When the splash ends, the buffer is wiped and
handed back. Opening SET-UP ends the splash early, so SET-UP never finds
its screen borrowed.

The terminal runs underneath the whole time. The banner, host output and
settings carry on as usual, and they appear when the splash goes away.
Nothing waits on it.

## Does it help?

The recording above was made by reflashing the board while ffmpeg
captured. The card locked partway through the 3 seconds, and the splash
was on screen for about the last 2. One boot doesn't settle whether a busy
picture makes the ShadowCast lock faster. What tells us is cold boots,
over the next few days. If it isn't enough, the next things to try are a
longer splash, or a bigger card that lights more of each line.

Host tests: `test/test_splash` checks the layout at 640x480 and 720p,
the fallback to flash when the borrowed buffer is too small, a
background line, and a line through the card at both scales.

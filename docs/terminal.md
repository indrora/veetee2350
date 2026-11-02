# Terminal emulator notes

## Data path

```
host program ── USB CDC ──▶ app_poll() ──▶ term_feed()
                                             │ libvterm state layer
                                             ▼ (parser, modes, cursor, SGR)
                              lib/term callbacks ──▶ struct grid (12-byte cells)
                                                          │ core 1, per scanline
                                                          ▼
                                             render_scanline() ──▶ DVI
replies (DA, DSR, ...) ◀── USB CDC ◀── term output callback
```

## Running a real session

`tools/hostshell.py` runs a shell on the Mac in an 80×30 pty
(`TERM=xterm-256color`) and bridges it to the board's serial port. Keys
you type in the terminal running it go to the shell, so it doubles as the
keyboard until USB HID exists (Goal 3):

    cd tools && python3 hostshell.py /dev/cu.usbmodem11401
    # inside it:  ./vtdemo.sh, less, vim, top, ...

`--type 'cmd'` (repeatable) injects commands for scripted checks, and
`--exit-after N` stops after N seconds.

To see what a command *should* look like, run
`uv run refscreen.py -- <command>`. It renders the command's output with
pyte, an independent emulator, at the same size.

## Verified on hardware (2026-11-01)

The checks were done with Genki captures in `captures/`.

- `g1-vtdemo.jpg`: bash prompt and `tput cols/lines` = 80×30. Also SGR
  bold/underline/blink/reverse/strike/conceal, the ANSI 16 colours, the
  256-colour cube and grey ramp, 24-bit colour, DEC line drawing via
  terminfo `smacs`, and CUP with save/restore cursor.
- `g1-fastfetch.jpg`: fastfetch (colour logo, info column, colour
  blocks). The layout matches row for row what pyte, an independent
  emulator, produces for the same output at 80×30
  (`tools/refscreen.py -- fastfetch`). That includes long info lines
  auto-wrapping onto the logo, which is fastfetch at 80 columns, not a
  bug.
- `g1-less.jpg`: `less -N /etc/services` shows the alternate screen,
  full-screen redraw, line wrapping (including less's own
  space-backspace trick at the right margin, which depends on xterm-style
  deferred wrap), and the reverse-video prompt line.

Host unit tests (`pio test -e native`, `test/test_term`) cover CUP/ED,
SGR (truecolor, indexed, bold-as-bright), attributes, bottom and DECSTBM
scrolling, ICH/DCH (moverect path), the 1049 alternate screen with cursor
restore, DEC special graphics, DECSCNM/DECTCEM/DECSCUSR, DA1/DSR replies,
and UTF-8 split across writes.

## Known gaps

| Gap | Why / plan |
|-----|------------|
| Palette changes (OSC 4) recolour new text only | Cells store resolved RGB565. To fix, store the palette index alongside and re-resolve on change. |
| Italic renders upright, bold is colour-only | No italic or bold font faces. Could synthesize bold with a 1 px smear. |
| DECSED/DECSEL erase protected cells too | DECSCA protection isn't tracked in cells. |
| Double-width/height lines (DECDWL/DECDHL) not drawn | Needs a per-row attribute and a 2× render path. vttest exercises it. |
| Wide (CJK) characters drawn half-width | Needs double-width glyphs (Unicode stretch goal). |
| Combining characters dropped | One glyph per cell. |
| DECCOLM (132 columns) ignored | The grid size is fixed by the font. A narrow font could provide it. |
| Second CDC "keyboard" port (Goal 1) | Not done yet. hostshell.py covers the keyboard role for now. |

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

## Terminal size and telling the host

The grid size comes from three settings: resolution, font, and
**Terminal size** (full screen, or a fixed 80×24 / 80×25 / 80×43 /
80×50 / 132×24 / 132×43, letterboxed, and clamped when it doesn't fit).
Hosts can find out the size in several ways:

| Sequence | Reply |
|----------|-------|
| `CSI 18 t` / `CSI 19 t` | `CSI 8 ; rows ; cols t` / `CSI 9 ; …` |
| `CSI 14 t` | text area in pixels: `CSI 4 ; height ; width t` |
| `CSI 16 t` | cell size: `CSI 6 ; height ; width t` |
| `CSI ? 2048 h` | in-band resize reports: `CSI 48 ; rows ; cols ; height ; width t` now, and after every change |
| `CSI ? 2048 $ p` | DECRQM: `CSI ? 2048 ; 1/2 $ y` |
| cursor to 999;999, then `CSI 6 n` | the resize(1) method |

`tools/hostshell.py` enables 2048 and starts the shell at the reported
size. Later reports are stripped from the stream and applied to the pty,
which sends SIGWINCH. That fixed full-screen programs (htop, micro)
using only part of the screen after a font or size change.

## Emulation level

SET-UP → Emulation picks VT100 / VT220 / VT340 / xterm. A level sets
the DA1/DA2 replies and loads defaults for Colour depth (off / 16 / 256
/ 24-bit), UTF-8 (off means 8-bit: C1 controls plus Latin-1 GR), and
Alternate screen (off works like xterm's titeInhibit). Each can be
overridden. Set `TERM` on the host to match (SET-UP shows the hint).
Changing level or depth resets the terminal.

## Keyboard port and SET-UP

The device presents two CDC ports: **VeeTee line** (the terminal's line,
which ends in `1` on macOS) and **VeeTee keyboard** (ends in `3`). Bytes
written to the keyboard port are keys. They go to the host over the
line, with new-line mode and local echo applied, except **F3**
(`ESC O R` or `ESC [ 13 ~`), which opens **SET-UP**. The BOOTSEL button
opens it too. Inside SET-UP: Up/Down to select, Left/Right/Enter/Space
to change, `S` save, `R` recall, `D` factory defaults, F3/Esc to leave.

    python3 tools/hostshell.py /dev/cu.usbmodem*1 --keyboard /dev/cu.usbmodem*3

routes your keys through the device, so F3 opens SET-UP.
`tools/sendkeys.py` scripts keystrokes for tests.

Settings live in the last flash sector (`hal/flashcfg`), CRC-checked.
They're applied at power-up and when leaving SET-UP. Font, rows and
colour changes reset the terminal. ENQ is answered with the answerback
string.

## Switching fonts

The terminal implements xterm's OSC 50 for its built-in fonts:

| Sequence | Effect |
|----------|--------|
| `ESC ] 50 ; notjam-prophet-8 BEL` | switch to that font (names from `tools/fonts.toml`) |
| `ESC ] 50 ; #+ BEL` / `#- BEL` | next / previous built-in font |
| `ESC ] 50 ; ? BEL` | reply `ESC ] 50 ; <current name> BEL` |

A switch changes the grid size (80×30, 80×36, 80×43, 80×48, 80×60...).
Both screens are rebuilt and the terminal is fully reset; screen
contents are not kept. Inside `hostshell.py`, `tools/setfont <name>`
sends the OSC and then reads the new size back the way resize(1) does
(cursor to 999;999, then DSR 6), and finally runs `stty rows/cols`, so
programs get SIGWINCH. `tools/fontshow.sh` steps through several fonts
with a sample page in each.

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
- `font-*.jpg`: runtime font switching through `fontshow.sh`:
  clean-16 (80×30), prophet-8 (80×60), prophet-11 (80×43), casual-10
  (80×48) and arbata-compact (80×36, Cyrillic included). casual-10's
  remapped `@ ^ _` are correct on hardware. Fonts without Latin-1 or
  Cyrillic show the missing-glyph box, as expected.
- `g1-less.jpg`: `less -N /etc/services` shows the alternate screen,
  full-screen redraw, line wrapping (including less's own
  space-backspace trick at the right margin, which depends on xterm-style
  deferred wrap), and the reverse-video prompt line.

Host unit tests (`pio test -e native`, `test/test_term`) cover CUP/ED,
SGR (truecolor, indexed, bold-as-bright), attributes, bottom and DECSTBM
scrolling, ICH/DCH (moverect path), the 1049 alternate screen with cursor
restore, DEC special graphics, DECSCNM/DECTCEM/DECSCUSR, DA1/DSR replies,
and UTF-8 split across writes.

## 132 columns (DECCOLM)

`CSI ? 3 h` switches to 132 columns and `CSI ? 3 l` back to the
**configured** width: 80 with a fixed 80×N terminal size, the full
screen width with "full screen". (So `reset`, which sends `?3l`, never
shrinks a full-width screen, but vttest's "80-column" screens only look
right with a fixed 80×N size.) Rows don't change.

132 needs 1056 px with an 8-pixel font, so only the 720p modes can do
it; elsewhere the request is ignored, as xterm ignores it with 80/132
switching disallowed. Mode 40 (xterm's "allow 80⇄132") is set by default
and can turn DECCOLM off; mode 95 (DECNCSM) keeps the screen contents
across a switch. A switch clears the screen, homes the cursor, resets
the margins and, with DECSET 2048 on, sends the host a size report.

## Conformance (esctest2)

esctest2 runs unattended through `tools/hostshell.py` (see devlog 012):

```
cd tools && python3 hostshell.py /dev/cu.usbmodem*1 --type \
  "cd …/esctest2/esctest && python3 esctest.py --expected-terminal=xterm \
   --xterm-checksum=390 --max-vt-level=4 --timeout=2 --logfile=run.log \
   --no-print-logs; exit" --exit-after 3000
python3 tools/esc_summary.py run.log [--failures] [--compare old.log]
```

Latest: **408 / 567** pass (72%) at 640×480; 409 at 1280×720, where DECCOLM works (baseline 169). `docs/esctest-latest.log`
holds the full log; `docs/esctest-baseline.log` the first run.

## Known gaps

| Gap | Why / plan |
|-----|------------|
| Palette changes (OSC 4) recolour new text only | Cells store resolved RGB565. To fix, store the palette index alongside and re-resolve on change. |
| Italic renders upright, bold is colour-only | No italic or bold font faces. Could synthesize bold with a 1 px smear. |
| Wide (CJK) characters drawn half-width | Needs double-width glyphs (Unicode stretch goal). |
| Combining characters dropped | One glyph per cell. |
| DECCOLM (132 columns) ignored at 640×480, 800×480 and 960×540 | 132 × 8 px doesn't fit. Honoured at both 720p modes (see "132 columns" above). A narrow font would allow it elsewhere. |
| Window ops (iconify, move, title stack, label reports) | There is no window. 19 esctest failures. |
| DECBI/DECFI, DECCARA/DECRARA | Not implemented yet. |
| Xcms colour spaces in OSC 4/10–19 | Only `rgb:`, `rgbi:` and `#hex` are parsed. |
| DECSLPP / DECSNLS (set lines per page) | Reported, not settable: the row count comes from SET-UP. |
| 8-bit GR is Latin-1, not DEC Supplemental Graphics | libvterm has no 96-character sets. Close, but not identical for VT220 apps. |
| A host RIS (`ESC c`) resets modes to libvterm defaults, not to the saved settings | libvterm doesn't signal RIS. SET-UP's settings are applied at boot, on exit, and by its "Reset terminal" action. |

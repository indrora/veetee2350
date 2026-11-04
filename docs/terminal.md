# Terminal emulator notes

## Data path

```
host program ── USB CDC or UART ──▶ app_poll() ──▶ term_feed()
                                             │ libvterm state layer
                                             ▼ (parser, modes, cursor, SGR)
                              lib/term callbacks ──▶ struct grid (12-byte cells)
                                                          │ core 1, per scanline
                                                          ▼
                                             render_scanline() ──▶ DVI
replies (DA, DSR, ...) ◀── USB CDC or UART ◀── term output callback
```

## Serial line (USB or UART)

SET-UP > Communications picks the line: **USB** (the CDC port, "VeeTee
line"; the default) or **UART**, the 40-pin header's UART1 (devlog 030):

| Signal | GPIO | Header pin | Direction |
|---|---|---|---|
| TX | 4 | 8 | out |
| RX | 5 | 10 | in |
| CTS | 6 | 31 | in (with RTS/CTS flow control) |
| RTS | 7 | 26 | out (with RTS/CTS flow control) |

3.3 V logic. A USB-serial adapter crosses over: its RX to pin 8, its TX
to pin 10, and a ground. The page also sets speed (300 to 115200), data
bits (5 to 8), parity (none, even, odd), stop bits (1, 2) and flow control
(none, RTS/CTS); they apply to the UART only, when SET-UP is left with
the change saved. The boot banner says which line is in use ("line: UART
115200 8N1"). A factory reset (hold BOOTSEL) goes back to USB.

Received bytes go through an 8 KiB ring filled by the UART's interrupt, so
a busy main loop doesn't lose them; with RTS/CTS the board drops RTS at
3/4 full. `lib/hal/serial.c` has the details.

On the Mac side, `tools/getty.py` puts a shell on the port:

    cd tools && uv run getty.py /dev/cu.usbserial-XXXX --baud 115200 --format 8N1

Use the `/dev/cu.*` node, not `/dev/tty.*`: opening a `tty.` node waits
for carrier detect, which a USB-serial adapter may never raise, and a
process stuck there can't even be killed (only unplugging the adapter
frees it). getty sets the port's format, sizes the pty from the VeeTee's
resize reports (as hostshell.py does), restarts the shell when it exits
(`--once` to stop instead), and on SIGTERM/SIGINT/SIGHUP hangs the shell
up and restores the port. `--log FILE` records the bytes each way.

## Running a real session

`tools/hostshell.py` runs a shell on the Mac in an 80×30 pty
(`TERM=xterm-256color`) and bridges it to the board's serial port. Keys
you type in the terminal running it go to the shell, so it doubles as the
keyboard (or plug a USB keyboard into the board itself, below):

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
The default font is ATI Small Wonder, 6×8, so a full screen at 640×480
is **106×60** (devlog 025); `notjam-clean-16` gives the old 80×30.
**640×480 @ 72 Hz** runs the CPU at 315 MHz instead of 252 (text 24%
faster) for displays that take VESA timings; the Genki ShadowCast
doesn't, so 60 Hz stays the default.
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

## USB keyboard

A USB keyboard plugged into the board's **PIO-USB** port (the second
USB-C, GPIO 28/29) types straight into the terminal: US layout, Caps /
Num / Scroll Lock with their LEDs, auto-repeat (500 ms, then 30 a
second), and the keypad. The terminal encodes each key by its modes,
as a VT terminal does (cursor keys, keypad, new line), and the Tools
menu, hotkeys, SET-UP and replay take it like any other keys.
Print Screen and Pause send nothing, so they make good hotkeys.

**Plug the keyboard in before powering the board.** The port's 5 V
comes straight off the supply with no switch, and a keyboard plugged into
a running board can pull it down far enough to reset the chip; the boot
banner then says "last reset: brown-out". A powered hub avoids it too.
An unplug isn't noticed (the last report keeps arriving), which is
harmless: only changes between reports are keys.

The port works in every video mode (devlog 028). Debug builds print
every report and where each key went (`[hid]`, `[key]`) on the line
port. Devlog 027.

## Keys, automation and SET-UP

The device presents one CDC port, **VeeTee line** (the terminal's line,
`/dev/cu.usbmodem…1` on macOS). Keys come from the USB keyboard above.
They go to the host over the line, encoded by the terminal's modes, with
new-line mode and local echo applied, except **hotkeys**.

**Debug builds** (`pio run -e pizero-debug`, or any debug env) add a
vendor-defined HID, **VeeTee automation**, that a script drives the
VeeTee through as a person would, and that carries the debug text
(`[key] F3 -> set-up`, `[hid] …`, stats) instead of the line. Release
builds don't have it. It replaced a second CDC port, "VeeTee keyboard",
which stood in for a keyboard until the board had one (devlog 029).
From the Mac (`uv` installs hidapi):

    cd tools
    uv run sendkeys.py 'key:ctrl+F3' 'wait:-> hotkey' 'key:enter'  # Tools, Set-Up
    uv run sendkeys.py 'k:ls -G\r'          # bytes as an xterm sends them
    uv run sendkeys.py 'keys:hello'         # key by key, virtual USB keyboard
    uv run sendkeys.py 'tap'                # BOOTSEL; 'hold:N' holds it
    uv run sendkeys.py --log                # watch the debug text

`wait:REGEX` waits for a debug line, which makes scripts check
themselves. The report layout is `include/automation_hid.h`, mirrored
by `tools/vthid.py`.
**Ctrl+F3** (`ESC [ 1 ; 5 R`, `ESC [ 13 ; 5 ~` or `ESC O 5 R`) opens the
**Tools menu**: Set-Up, Screenshot, Print screen, Printer on/off, Clear
screen, Soft reset (DECSTR), Hard reset (RIS); Up/Down, Enter, Esc. F12
takes a screenshot. Plain F3 goes to the host like any other key. A tap
on the BOOTSEL button opens SET-UP (or closes it). **Holding BOOTSEL for
5 seconds is a factory reset**: after a second SET-UP opens and counts
down, letting go cancels, and at 5 seconds every setting goes back to
factory defaults, is saved, and the board restarts once the button is
let go. It needs no keyboard and no picture, so it's the way out of a
mode the monitor can't show. SET-UP opens on a directory (devlog 030):
Display, Terminal, Communications, Printer and card, Hotkeys, then the
actions (save, recall, factory defaults, reset terminal, exit). Enter
opens a page; Esc goes back to the directory. On a page: Up/Down to
select, Left/Right/Enter/Space to change. Everywhere: `S` save, `R`
recall, `D` factory defaults, **F4 to save and leave**, F3 to leave (Esc
too, from the directory). If anything changed,
F3/Esc ask "Save changes?" first: Y (or F4) saves, N discards it all as
if SET-UP never opened, Esc goes back. A new resolution takes a restart,
so it only happens once saved, and the question says "and restart".
Leaving never saves by itself.

SET-UP's **Hotkeys...** page rebinds any tool to any key (with
modifiers); **Profiler bar** shows the last frame's per-line cycle counts
at the top of the screen (devlog 021).

**Capture and replay** (devlog 023): the Tools menu's **Capture**
records host output to `/capture/NNNN.rec` as ttyrec (also a SET-UP
item, to start at boot); **Replay file...** opens a file picker and
plays a ttyrec file with its timing, anything else as raw bytes at
9600 baud. While it plays: Space pauses, Up/Down change speed (1/4x to
16x, max), `n` turns LF -> CR LF on or off, Esc stops. SET-UP's
**Replay newlines** sets that default (off, raw files, all files).
Host output waits until it's over.

**Status line** (SET-UP, devlog 022): Hidden, Bottom or Top (a row of
its own; the terminal is a row shorter if the screen has no spare row),
or Peek bottom / Peek top (it slides in over the edge row when something
changes and away after **Peek time**: 3, 5, 7 or 10 s). It shows the
VT100 user lights, the printer, Insert mode, messages ("Saved
/screens/0004.bmp") and the cursor, in the theme's colours swapped. The
host sets the lights with DECLL (`CSI Ps q`: 0 all off, 1-4 on, 21-24
off); while the indicator isn't on screen, lit lights show in the top
right corner. DECSSDT (`CSI Ps $ ~`) 0 blanks the status line and 1
brings the indicator back; 2 (host-writable) isn't implemented yet.

Keys carry xterm's modifier parameter (`; m`, m - 1 = Shift 1, Alt 2,
Ctrl 4, Meta 8) and ESC + a printable byte is Alt plus that key, so
hotkeys can use modifiers.

    cd tools && uv run hostshell.py /dev/cu.usbmodem*1 --hid

routes the Mac's keys through the device (debug builds), so Ctrl+F3
opens the Tools menu. `tools/sendkeys.py` scripts keystrokes for tests.

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
it; elsewhere, like xterm when it can't resize its window, the mode is
set and the DEC side effects happen (clear, home, margins) at the
current width. Mode 40 (xterm's "allow 80⇄132") is set by default
and can turn DECCOLM off. Mode 95 (DECNCSM, keep the screen across a
switch) is a VT510 feature: like xterm, VeeTee honours it only at
conformance level 5, which none of its emulations reach, so it is
stored and reported but has no effect. A switch clears the screen, homes the cursor, resets
the margins and, with DECSET 2048 on, sends the host a size report.

## Sixel graphics

`DCS P1 ; P2 ; P3 q … ST` draws a DEC Sixel image at the cursor
(`lib/sixel` decodes, `lib/term/graphics.c` places). Every cell the
image covers becomes a *graphic cell*: its glyph index points at a tile,
one font cell of 8-bit pixels, in a pool on the heap. So pictures scroll,
erase, survive the alternate screen and get overwritten by text exactly
like characters do. Tiles nothing points at are reclaimed by a sweep
when the pool runs out.

| | |
|---|---|
| Pool | up to 256 KB: at 8 bits 2048 cells (85% of 80×30) at 640×480, twice as many per halving of depth; less at 720p, where the grids are big. The boot banner shows the count. |
| Colours | 256 registers per image (1070 private by default). At 8 bits a pixel up to 255 distinct colours on screen, then nearest match; at 4/2/1 bits a fixed palette (CGA / greys / foreground ink) with Floyd–Steinberg dithering |
| Modes | 80 DECSDM (no scrolling, image at top-left), 1070 private registers, 8452 cursor right of image |
| Queries | DA1 adds `4`; XTSMGRAPHICS (`CSI ? 1/2 ; 1 S`) reports 256 registers and the text area in pixels |
| SET-UP | "Sixel graphics": off / 8 / 4 / 2 / 1 bits a pixel (2048 / 4096 / 8192 / 16384 cells at 640x480); VT340 defaults to 4, xterm to 8 |

Limits: pixels an image never paints stay transparent (P2 is not
honoured for 0/2); pixel aspect is always 1:1; graphics on double-size
lines aren't drawn; a font switch or DECCOLM drops all images (the pool
is handed back so the grids can be rebuilt).

## Printer (Media Copy)

With "Printer" on in SET-UP, MC controls print to `/print/NNNN.txt` on
the SD card: `CSI i` (screen or scrolling region, DECPEX/DECPFF
honoured), `CSI ? 1 i` (cursor line), `CSI ? 5/4 i` (auto print),
`CSI 5 i` ... `CSI 4 i` (printer controller mode, raw). `CSI ? 15 n`
reports ready / not ready / no printer. See devlog 018.

## Conformance (esctest2)

esctest2 runs unattended through `tools/hostshell.py` (see devlog 012):

```
cd tools && python3 hostshell.py /dev/cu.usbmodem*1 --type \
  "cd …/esctest2/esctest && python3 esctest.py --expected-terminal=xterm \
   --xterm-checksum=390 --xterm-reverse-wrap=390 --max-vt-level=4 \
   --timeout=2 --logfile=run.log \
   --no-print-logs; exit" --exit-after 3000
python3 tools/esc_summary.py run.log [--failures] [--compare old.log]
```

Latest: **494 / 567** pass (87%) at 640×480 (baseline 169). `docs/esctest-latest.log`
holds the full log; `docs/esctest-baseline.log` the first run.

## Known gaps

| Gap | Why / plan |
|-----|------------|
| Palette changes (OSC 4) recolour new text only | Cells store resolved RGB565. To fix, store the palette index alongside and re-resolve on change. |
| Italic renders upright, bold is colour-only | No italic or bold font faces. Could synthesize bold with a 1 px smear. |
| Wide (CJK) characters drawn half-width | Needs double-width glyphs (Unicode stretch goal). |
| Combining characters dropped | One glyph per cell. |
| DECCOLM can't widen at 640×480, 800×480 and 960×540 | 132 × 8 px doesn't fit: the mode is set, the width stays. Real 132 columns at both 720p modes. A narrow font would allow it elsewhere. |
| Window resizing (CSI 4/8 t) | There is no window; the size comes from SET-UP. Iconify/move are remembered for their reports; titles, icon labels and the title stack are real. |
| DECCARA/DECRARA | Not implemented (bold and inverse aren't stored per cell). DECBI/DECFI are (libvterm patch 10). |
| Xcms colour spaces in OSC 4/10–19 | Only `rgb:`, `rgbi:` and `#hex` are parsed. |
| DECSLPP / DECSNLS (set lines per page) | Reported, not settable: the row count comes from SET-UP. |
| 8-bit GR is Latin-1, not DEC Supplemental Graphics | libvterm has no 96-character sets. Close, but not identical for VT220 apps. |
| A host RIS (`ESC c`) resets modes to libvterm defaults, not to the saved settings | libvterm doesn't signal RIS. SET-UP's settings are applied at boot, on exit, and by its "Reset terminal" action. |

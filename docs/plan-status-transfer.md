# Plan: status line, user lights, capture/replay, file transfer (phase H)

Goal (from the user, 2026-11-02): settings for a status line, the VT100
user lights, and XMODEM/YMODEM/ZMODEM transfers to and from the SD card,
plus capture to and replay from the card.

## Decisions

| | Choice | Why |
|---|---|---|
| Tools menu | Ctrl+F3 opens a small menu: Set-Up, Screenshot, Print screen, Printer, Capture, Replay, Send, Receive, Clear screen, Soft reset, Hard reset | The user's idea (2026-11-02): F3 alone goes to the host now; actions get one launcher instead of being buried in SET-UP. |
| Hotkeys | Configurable in SET-UP: each action bound to a key with modifiers, or nothing. Defaults: Tools menu Ctrl+F3, Screenshot F12 | The user's pick; F12 keeps its old job until someone rebinds it. |
| Modifiers | `keys.c` decodes xterm's modified keys (`CSI 1 ; m X`, `CSI n ; m ~`, m = 1 + Shift 1 + Alt 2 + Ctrl 4) | Needed for Ctrl+F3 on the keyboard port today; the USB HID keyboard (Goal 3) will set them directly. |
| User lights | DECLL (`CSI Ps q`): 0 all off, 1–4 on, 21–24 off (VT510) | VT100 had L1–L4 on the keyboard; GOAL.md Goal 2 wants them on-display. |
| Where lights show | In the indicator status line when it's on; a small corner overlay when it's off (only while one is lit) | The user's pick: DECLL is always visible, never clutters an idle screen. |
| Status line | SET-UP: Hidden / Bottom / Top / Peek bottom / Peek top, peek time 3/5/7/10 s; shown as an indicator line. The host switches type with DECSSDT (`CSI Ps $ ~`: 0 none, 1 indicator, 2 host-writable) and writes with DECSASD (`CSI Ps $ }`). Bottom and Top reserve a row whatever the host says (DECSSDT 0 blanks it, as on a VT320); Hidden stays hidden (the host's type is only remembered); Peek modes peek for the indicator, not for DECSSDT 0 | The user's spec (2026-11-02). VT320 and later, and xterm. A reserved row: the terminal is one row shorter. Built in step 2 (devlog 022); the plan had a host request show a Hidden line at the bottom, dropped: it would resize the terminal under the host. |
| Status line colours | The theme's foreground and background, swapped | The user's spec: inverted, so it reads as chrome, and follows SD/base16 schemes. |
| Status row storage | A separate cell row in `struct render`, mapped to the bottom font-height lines (`RENDER_LINE_STATUS`) | libvterm, DECRQCRA, printing and screenshots-as-text never see it; the renderer only picks a different cell pointer for those lines. |
| Lights overlay | Patched in at scanline time over a copy of one grid row | The screen's contents are never touched, so readback and redraw stay right. |
| Host-writable status line | A second, one-row libvterm; `CSI 1 $ }` / `CSI 0 $ }` caught before libvterm (like `mc_feed`) | libvterm consumes whole buffers, so the switch has to happen in the byte pre-pass. |
| Capture | ttyrec to `/capture/NNNN.rec`, on/off in SET-UP | The user's pick: timed, so replay keeps animation; ttyplay/ipbt read it; raw is a trivial strip. |
| Replay | SET-UP file picker over the card; plays ttyrec with its timing, any other file as raw at a chosen speed; host input paused, any key stops | Demos, benchmarks, ANSI art. |
| X/YMODEM | Started from SET-UP or a hotkey (Send / Receive); host runs `rx`/`sx`/`rb`/`sb`; received files to `/download` | Small, well specified protocols; licence of any borrowed code (Menie's `xmodem.c`) checked first. |
| ZMODEM | Last: port NuttX's `system/zmodem` (Apache-2.0) after a licence check; auto-start on `sz`/`rz`'s ZRQINIT/ZRINIT | Too big to hand-roll; NuttX's is embedded-oriented. Needs POSIX file I/O mapped to `storage.h`. |

The repository is MIT licensed (`LICENSE`, 2026-11-02); vendored and
fetched code keeps its own licence, listed in `THIRD_PARTY.md`.

## Steps (one devlog entry and captures each)

0. **Modifiers, Tools menu, hotkeys.** `key_event.mods`; the menu as a
   small box drawn like SET-UP; a Hotkeys page in SET-UP that captures
   the next key for an action; settings version bump. Menu entries for
   things not built yet appear as their steps land.
1. **User lights.** *Done, devlog 022.* DECLL in `term.c`'s CSI fallback; `term->lights`;
   the corner overlay in the renderer. Host tests for the parameters.
2. **Indicator status line.** *Done, devlog 022, with the Peek
   modes; the status row is `render->status_cells`, peeks rewrite the
   line tables of the edge row (`render_status_peek`).* Setting (`SETTINGS_VERSION` bump), the
   status row in the renderer, rows - 1 for the terminal, an indicator
   composed by `term.c` from its own state (lights, cursor, printer)
   plus a line of text the app sets (capture, transfer progress).
   DECSSDT 0/1 from the host; DECRQSS reports it.
3. **Host-writable status line.** DECSSDT 2, DECSASD routing, the
   second libvterm drawing into the status row.
4. **Capture.** *Done, devlog 023.* `src/capture_sd.c` after `printer_sd.c`: a ttyrec
   header (sec, usec, length) per chunk of host bytes; closed after a
   quiet second; on/off in SET-UP, shown in the indicator.
5. **File picker and replay.** *Done, devlog 023: the picker is an
   overlay over the screen (lib/setup/picker.c), not a SET-UP list;
   "Replay newlines" (onlcr) is a setting, off by default.* A SET-UP list over a card directory
   (first 64 entries, like fonts and schemes); replay feeds
   `term_feed` from the main loop, paced, watchdog fed.
6. **XMODEM / YMODEM.** Checksum and CRC, 128 and 1K blocks, YMODEM
   batch header (name, size). A transfer owns the line: the screen
   shows progress in the status line (or a SET-UP-style box).
7. **ZMODEM.** Licence check, port, auto-start detection in the byte
   pre-pass.

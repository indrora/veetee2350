# Local changes to vendored libvterm

Upstream: https://www.leonerd.org.uk/code/libvterm/, release 0.3.3
(sha256 09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0).

Vendored: `src/` (including the generated `encoding/*.inc` and
`fullwidth.inc` shipped in the release tarball), `include/`, `LICENSE`.
Left out: `t/` (upstream tests), `bin/`, the Makefile.

Built as gnu11 (`library.json`). It is upstream's own C99 code, and we
don't want C23 keyword changes to start mattering in vendored code.

## 1. Unknown DEC private modes reach the CSI fallback (`src/state.c`)

`set_dec_mode()` and `request_dec_mode()` used to drop modes they don't
know, logging "Unknown DEC mode". Their default cases now offer the mode
to `VTermStateFallbacks.csi` first:
- set/reset arrives as `CSI ? <num> h` or `l`, one mode per call;
- DECRQM arrives as `CSI ? <num> $ p`.

If the fallback returns 0, the original behaviour applies. VeeTee uses
this for mode 2048 (in-band resize notifications).

## 2. DA1 / DA2 can be answered by the embedder (`src/state.c`)

libvterm always answered `CSI c` with `?1;2c` (VT100 with AVO) and
`CSI > c` with `>0;100;0c`. Both cases now offer the sequence to
`VTermStateFallbacks.csi` first, and the built-in reply is only used if
the fallback returns 0. VeeTee answers according to its emulation level
(VT100 / VT220 / VT340 / xterm), and adds Sixel (`;4`) when enabled.

## 3. CSI with unhandled intermediates reaches the fallback (`src/state.c`)

`on_csi()` returned 0 early for any intermediate other than
`space ! " $ '`, and for multi-byte intermediates, so those sequences
never reached `VTermStateFallbacks.csi`. Both early exits now offer the
sequence to the fallback. VeeTee needs this for DECRQCRA (`CSI ... * y`),
which esctest uses to read the screen back.

## 4. ANSI modes and status reports (`src/state.c`)

- `set_mode()` offers unknown ANSI modes to the CSI fallback as
  `CSI num h` or `l`, with no leader. Before, they were logged and
  dropped.
- DSR/DECDSR (`CSI [?] Ps n`): values libvterm doesn't answer go to the
  CSI fallback. VeeTee answers printer, UDK, keyboard, locator, macro
  space, memory checksum, data integrity and multi-session reports.
- DECXCPR (`CSI ? 6 n`) now includes the page number (`?row;col;1R`), as
  DEC terminals and xterm do. Plain CPR is unchanged.

## 5. SCOSC / SCORC (`src/state.c`)

libvterm always treated `CSI s` as DECSLRM and ignored `CSI u`. As in
xterm, `CSI s` now saves the cursor (SCOSC) unless DECLRMM is set, and
`CSI u` restores it (SCORC). Both use libvterm's own `savecursor()`.

## 6. Reverse wraparound, DECSET 45 (`src/state.c`, `src/vterm_internal.h`)

There is a new `mode.reverse_wrap` bit, which can be set, reset and
queried with DECRQM. With it and DECAWM both on, BS in the first column
moves to the last column of the previous line, as xterm does.

## 7. DECSC/DECRC save origin mode and DECSCA (`src/state.c`, `src/vterm_internal.h`)

DEC STD 070 includes DECOM and the selective-erase attribute in the
DECSC save. libvterm saved neither, so a protected character written
after DECRC wasn't protected. Both are now saved and restored. Soft and
hard reset (DECSTR, RIS) also reset the saved state (home, DECOM off,
unprotected) and the reverse-wraparound bit; without that, protection
saved before a reset leaked back in on the next DECRC. Reset also
seeds the saved pen with the default pen: DECRC with nothing saved
used to restore an all-zero pen, i.e. explicit black-on-black RGB.

## 8. DECRQSS (`src/state.c`)

- The SGR reply always starts with `0` (xterm form, replayable):
  `DCS 1$r0;1m ST`, or `DCS 1$r0m ST` for plain.
- Settings libvterm doesn't know go to `VTermStateFallbacks.dcs` as a
  complete `"$q"` string before libvterm replies `0$r` (invalid). VeeTee
  answers DECSCL, DECSLPP, DECSNLS, DECSACE, DECSASD and DECSSDT.

## 9. Line info follows row-count changes (`src/state.c`)

`on_resize()` is meant to reallocate the per-row `VTermLineInfo` arrays
(DECDWL/DECDHL flags) when the row count changes, but it compared
`rows != state->rows` after already assigning `state->rows = rows`, so it
never did, and the code only ran when there was no resize callback.
Growing the screen (a smaller font, a bigger SET-UP size) then wrote past
the end of both arrays. The reallocation now always runs, against the
old row count, before the embedder's resize callback.

## 10. DECBI / DECFI (`src/state.c`)

`ESC 6` (back index) and `ESC 9` (forward index), VT420. libvterm had
neither, and escape sequences have no embedder fallback, so they're
handled in `on_escape()`: move the cursor a column; at the left/right
margin (inside the margins) scroll the region's contents sideways
instead, with libvterm's own `scroll()`.

## 11. Margins for cursor motion; origin-relative reports (`src/state.c`)

As xterm (and esctest) expect with DECSLRM/DECSTBM margins:
- CR goes to the left margin from at or right of it (or in origin
  mode), else to column 1; CNL/CPL do the same for the column.
- CUU/CUD/CUF/CUB/CNL/CPL stop at the margin when they start at or
  inside it, else at the screen edge.
- CPR / DECXCPR report the position relative to the margins in origin
  mode (libvterm reported absolute positions).

## 12. DECALN resets margins and homes (`src/state.c`)

DEC STD 070 (and xterm): after filling the screen with E, DECALN resets
the top/bottom and left/right margins and moves the cursor home.
libvterm only filled the screen.


## 13. A DECSC save per screen (`src/vterm_internal.h`, `state.c`, `pen.c`)

xterm keeps the DECSC/DECRC (and 1048) save separately for the main and
the alternate screen. `saved` became `saved_screen[2]`, used through
`SAVED(state)`, which picks the one for the screen shown; mode 1049
saves before switching to the alternate screen and restores after
switching back, so its save lives on the main screen. Reset clears both.

## 14. Index outside the left/right margins (`src/state.c`)

With DECLRMM margins set, LF/IND/NEL/VT/FF at the bottom margin and RI
at the top margin scroll only when the cursor is between the left and
right margins; outside them the cursor stays put and nothing scrolls
(xterm, DEC STD 070).

## 15. Text and the left/right margins (`src/state.c`)

Printing between DECSLRM margins treats the right margin as the line's
edge: autowrap continues at the next line's *left margin*, without
autowrap the cursor sticks at the right margin, and insert mode pushes
characters off at the right margin. Outside the margins (or with
DECLRMM off) the screen edge applies, as before.

## 16. Reverse wraparound as xterm 383+ (`src/state.c`)

Extends patch 6. Mode 45 now backs BS up onto the previous line only
when the current line is its continuation (it wrapped); new mode 1045
always does, and from the top margin wraps to the bottom margin. Both
need DECAWM, stop at the left margin when the cursor is at or inside
it, land on the previous line's right margin, and from the
pending-wrap position only cancel the wrap. DECRQM and reset know 1045.

## 17. Mode 47 (`src/state.c`)

xterm's original alternate-screen switch, DECSET 47, is handled like
1047 (libvterm knew only 1047 and 1049). DECRQM reports it.

## 18. Conformance level limits (`src/vterm_internal.h`, `state.c`)

`state->vt_level` (1-5, set by the embedder from DECSCL; 0 = no limit):
below level 4 DECSET 69 (DECLRMM) is ignored, below level 3 DEC-private
DECRQM is not answered -- as a VT220 or VT320 would behave.

## 19. REP prints like text (`src/state.c`)

libvterm's REP repeated the last character up to the end of the row and
stopped. xterm treats REP as printing it `count` more times, so it
wraps, honours the left/right margins and scrolls. The per-glyph part
of `on_text()` became `print_glyph_begin()` / `print_glyph_end()`, and
REP prints through them.

## 20. NEL, DCH, one-line DECSTBM (`src/state.c`)

- NEL is LF then CR with patch 11's margin rule (LF first, so outside
  the left/right margins it doesn't scroll).
- DCH works outside the top/bottom margins; only the left/right
  margins limit it (xterm).
- DECSTBM with top >= bottom (a one-line region) is invalid and resets
  to the full screen, as xterm requires at least two lines.

## 21. CUB with reverse wraparound (`src/state.c`)

With DECAWM and reverse wraparound (45 or 1045) on, CUB moves like that
many backspaces, crossing line boundaries by the same rules (xterm).
BS's logic became `back_space()` for this.

## 22. XTSAVE / XTRESTORE (`src/state.c`, `src/vterm_internal.h`)

xterm's `CSI ? Pm s` saves DEC private modes and `CSI ? Pm r` puts them
back. libvterm keeps up to 16 of its own modes (`xt_saved`, cleared by
hard reset); modes it doesn't implement go to the CSI fallback one at a
time, so VeeTee's term.c saves its stored modes too. DECRQM's mode lookup
became `get_dec_mode()` to share with it.

## 23. Tabs stop at the left/right margins (`src/state.c`)

With DECLRMM on and the cursor between the margins, HT/CHT stop at the
right margin, as on xterm and the VT420. CBT still runs to column 1.

## 24. DECSET 41, xterm's more(1) fix (`src/state.c`, `src/vterm_internal.h`)

With mode 41 and DECAWM on, HT while a wrap is pending takes the wrap
first (to the next line, left margin) and then tabs, so curses' "fill
the row, then tab" lands where it meant. Off by default, as in xterm.

## 25. Alternate-screen clearing per mode (`src/state.c`)

Upstream erased the alternate screen on every entry. xterm clears it at
different moments: DECSET 47 never, 1047 on leaving, 1049 on entering.
`erase_alt_screen()` does that from `set_dec_mode()`, only while the
alternate screen is the one showing (an embedder that refuses the switch
keeps its primary intact). term.c no longer clears on entry itself.

## 26. CHA and HPA honour origin mode (`src/state.c`)

Under DECOM, CHA and HPA columns count from the left margin, as CUP's
do and as libvterm's VPA already did for rows (xterm's `CursorSet`).
esctest names the test "HPA_IgnoresOriginMode", but what it checks
(HPA 2 reads back as column 2 through an origin-relative CPR) needs this.

## 27. ISO protected areas: SPA / EPA (`src/state.c`, `src/vterm_internal.h`)

SPA (ESC V, C1 0x96) starts protected text and EPA (ESC W, 0x97) ends
it, sharing the protected bit with DECSCA. As in xterm, the state
remembers which of the two set protection last (`protect_iso`): after
SPA, plain ED, EL and ECH also leave protected cells alone (they erase
"selectively"); after DECSCA only DECSED/DECSEL do. Soft and hard reset
go back to DEC rules, so RIS still clears everything. (term.c's
DECSERA reads `protect_iso` too: it honours DEC protection only.)

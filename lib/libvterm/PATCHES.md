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


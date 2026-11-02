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

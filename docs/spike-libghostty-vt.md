# Spike: libghostty-vt on the RP2350

Date: 2026-11-01. Ghostty commit `042b16b` (2026-11-01), Zig 0.17.0,
arduino-pico 4.4.4 (GCC 14.2.0).

Question: can libghostty-vt serve as the VeeTee 2350 parser/terminal-state
backend instead of libvterm?

## What works

- **It cross-compiles.** The following produces a static `libghostty-vt.a`
  plus headers:

      zig build -Demit-lib-vt -Dtarget=thumb-freestanding-eabi \
          -Dcpu=cortex_m33 -Doptimize=ReleaseSmall

  Zig's own TLS fails against `deps.files.ghostty.org` on this machine.
  Workaround: download each tarball with curl, then `zig fetch <file>`.
- **The ABI matches.** Zig's `eabi` passes float arguments in core registers,
  which is compatible with arduino-pico's `-mfloat-abi=softfp`.
- **Few external dependencies.** The undefined symbols are `__aeabi_*`
  helpers, soft-float routines, a handful of libm functions (`exp`, `log`,
  `sqrt`, `round`, ...), `strlen`, and three 128-bit float extenders
  (`__extenddftf2`, `__extendsftf2`, `__extendxftf2`). That last group is
  probably missing from the ARM libgcc, so we may need to link Zig's
  compiler_rt. This was not verified at link time.
- **Flash cost is fine.** About 855 KB of `.text`/`.rodata` before
  dead-stripping, against 16 MB of flash. However, running that much code
  from XIP flash through a 16 KB cache is a performance risk.
- **The C API covers what we need:**
  - a custom allocator vtable, which is mandatory on freestanding targets
    (the default allocator always fails there);
  - `SCROLLBACK_MAX_LINES` / `SCROLLBACK_MAX_BYTES`;
  - a render-state API;
  - `ghostty_terminal_vt_write_until()` with `BOUNDARY_DCS_START` and
    `BOUNDARY_DCS_END`, which lets us intercept DCS strings (Sixel, ReGIS)
    before Ghostty discards them. The unknown-sequence callback reports
    only APC and OSC.

## What kills it as-is: page memory

Ghostty stores the grid in fixed-capacity **pages**. On freestanding
targets every page comes from our allocator. The size of a standard page
is a comptime constant (`pagepkg.std_capacity`, 215 × 215 cells with
128 styles and 8 KiB of grapheme space). A comptime probe built with
lib-vt's freestanding options (`page_size_min = 16`) reports, for the
M33 target:

| Page capacity             | Bytes   |
|---------------------------|---------|
| `std_capacity` (stock)    | 393 504 |
| 80 cols × 32 rows         |  42 704 |

`initialCapacity()` keeps 80-column pages at `std_size` (it adds rows
instead of shrinking the page), so:

- The primary screen alone needs **384 KiB of 520 KiB of SRAM**.
- The alternate screen is created lazily, on the first DECSET 1049 /
  47 / 1047. That costs **another 384 KiB**, which is a guaranteed OOM
  (vim, less, and vttest all switch screens).
- Scrollback is pruned only at page granularity, so scrolling past the
  end of a page probably needs a second page before the old one is
  freed. This was not verified.

## Options

1. **Carry a patch.** Make `std_capacity` small for freestanding builds,
   for example 80 × 64 at roughly 80 KiB per page. That needs a fork or
   patch file against a C API that is still marked WIP and has broken
   compatibility several times recently. Page-churn behavior at small
   capacities is untested upstream.
2. **libvterm state layer + our own 8-byte cell grid.** About 19 KiB per
   screen, an API that has been stable for a decade, and plain C in
   `lib/`. Costs: worse esctest edge-case coverage, no VT52.

Keep `term/emulator.h` backend-agnostic either way, so a libghostty-vt
backend can be revisited if upstream ever exposes page capacity as a
build option.

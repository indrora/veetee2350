# 001 — Choosing a parser: the libghostty-vt spike

*2026-11-01 · commit `ad821b3`*

Before writing any firmware we had to choose a parser: what turns escape
sequences into terminal state. The two candidates were **libvterm**,
neovim's mature C library, and **libghostty-vt**, the new embeddable
core of Ghostty. Ghostty's is probably the more correct of the two, so
we tried it on the hardware we actually have.

The good news: it cross-compiles to a freestanding Cortex-M33 library
(`zig build -Demit-lib-vt -Dtarget=thumb-freestanding-eabi`). The ABI
matches arduino-pico's `softfp`, and it only needs compiler-runtime and
libm symbols.

The deal-breaker turned up in a comptime probe of Ghostty's page
allocator. A standard terminal page is **393 504 bytes**. The RP2350 has
520 KiB of SRAM. The primary screen takes one page and the alternate
screen another, so the first time `vim` switches screens, the board runs
out of memory.

| Page capacity | Bytes |
|---|---|
| Ghostty `std_capacity` | 393 504 |
| 80 × 32 | 42 704 |

A small page would fit, but only by patching a library whose C API is
still marked work-in-progress. We picked libvterm's state layer plus our
own 12-byte cell grid instead. The full write-up is in
[`docs/spike-libghostty-vt.md`](../spike-libghostty-vt.md).

/*
 * hot.h — portability shim for code on the scanline hot path.
 *
 * Pure modules (font, render, term) must compile on the host for unit
 * tests (STYLE.md §4), but on the RP2350 their per-line functions must run
 * from SRAM, not XIP flash. HOT_FUNC(name) expands to pico-sdk's
 * __not_in_flash_func on target and to a plain name on the host.
 *
 * HOT_INLINE is for small helpers called from hot functions. The core
 * builds with -Os, under which GCC happily out-lines a `static inline`
 * helper into .text -- i.e. into FLASH -- and calls it through a veneer.
 * That cost the text renderer 2.5x its budget before it was caught, so
 * hot helpers must be forced inline rather than trusted to the optimiser.
 */
#ifndef HOT_H
#define HOT_H

#if defined(PICO_RP2350) || defined(PICO_RP2040)
#include "pico.h"
#define HOT_FUNC(name)	__not_in_flash_func(name)
#define HOT_DATA	__not_in_flash("hot")
/*
 * Scratch Y (4 KB, its own bus port): mostly core 0's stack, growing
 * down from the top. Small, hot tables that the renderer reads while
 * core 1's encoder hammers main SRAM go at the bottom; keep it to a
 * few hundred bytes so the stack keeps its headroom.
 */
#define SCRATCH_DATA	__scratch_y("veetee")
#else
#define HOT_FUNC(name)	name
#define HOT_DATA
#define SCRATCH_DATA
#endif

#define HOT_INLINE	static inline __attribute__((always_inline))

/*
 * Profiling (-DVEETEE_RENDER_PROFILE, env:pizero-profile): HOT_CYCLES()
 * reads core 0's DWT cycle counter (CYCCNT, 0xE0001004 on the M33;
 * hal/video.c enables it), so hot code can time its own pieces. Without
 * the define, or on the host, it's 0 and the timing code drops out.
 */
#if defined(VEETEE_RENDER_PROFILE) && defined(PICO_RP2350)
#define HOT_CYCLES()	(*(volatile uint32_t *)0xE0001004u)
#define HOT_PROFILE	1
#else
#define HOT_CYCLES()	0u
#define HOT_PROFILE	0
#endif

#endif /* HOT_H */

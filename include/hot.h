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
#else
#define HOT_FUNC(name)	name
#define HOT_DATA
#endif

#define HOT_INLINE	static inline __attribute__((always_inline))

#endif /* HOT_H */

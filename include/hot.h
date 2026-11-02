/*
 * hot.h — portability shim for code on the scanline hot path.
 *
 * Pure modules (font, render, term) must compile on the host for unit
 * tests (STYLE.md §4), but on the RP2350 their per-line functions must run
 * from SRAM, not XIP flash. HOT_FUNC(name) expands to pico-sdk's
 * __not_in_flash_func on target and to a plain name on the host.
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

#endif /* HOT_H */

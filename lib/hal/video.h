/*
 * video.h — DVI scanout HAL (640x480p60, RGB565 scanlines).
 *
 * Layering: lowest level (STYLE.md §4). The renderer above us never sees
 * TMDS, PIO or DMA; it only fills one 640-pixel RGB565 line at a time.
 *
 * Model: video_init() hands core 1 over to TMDS encoding. The registered
 * scanline callback runs ON CORE 0, inside a top-priority interrupt, and
 * renders lines into a short ring a few lines ahead of the beam. It must
 * live in RAM (__not_in_flash_func), must not block, and must not take
 * locks that core 0 thread code can hold. Data it reads from thread code
 * on the same core can change between lines (but not mid-line).
 */
#ifndef HAL_VIDEO_H
#define HAL_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

#define VIDEO_WIDTH	640
#define VIDEO_HEIGHT	480

/*
 * One 640x480p60 line is 800 pixel clocks including blanking. At one TMDS
 * bit per sysclk cycle and 10 bits per pixel, that is 8000 sysclk cycles
 * per line at 252 MHz. Encode (core 1) and render (core 0) must each stay
 * below it on average, or DVI emits "late" lines (solid colour) to resync.
 */
#define VIDEO_SYSCLK_KHZ		252000
#define VIDEO_LINE_BUDGET_CYCLES	8000

/*
 * Fill `pixels` (VIDEO_WIDTH RGB565 values, 4-byte aligned) for visible
 * line `line` (0..VIDEO_HEIGHT-1). Runs on core 0 in IRQ context; must
 * live in RAM (__not_in_flash_func) and must not block. Lines are always
 * requested in display order.
 */
typedef void (*video_scanline_fn)(uint16_t *pixels, unsigned line, void *ctx);

struct video_stats {
	uint32_t	frames;		  /* frames encoded since start */
	uint32_t	avg_encode_cycles;/* core 1, last frame */
	uint32_t	max_encode_cycles;
	uint32_t	avg_render_cycles;/* core 0 scanline callback, last frame */
	uint32_t	max_render_cycles;
	uint32_t	starved;	  /* lines the ring was empty (cumulative) */
};

/*
 * Raise VREG, confirm sysclk is 252 MHz, bring up PIO/DMA serialisers and
 * launch core 1. Never returns failure: if video can't start the device is
 * useless, so it panics (STYLE.md §7). Call once, from core 0.
 */
void	video_init(video_scanline_fn scanline, void *ctx);

/* Snapshot of scanout statistics; safe to call from core 0 at any time. */
void	video_get_stats(struct video_stats *out);

/*
 * True once core 1 runs only from SRAM, so core 0 may make XIP flash
 * unavailable (BOOTSEL read, flash program/erase) without crashing it.
 * Core 0's own render IRQ must be masked for the duration; the picture
 * glitches if that exceeds ~250 us (the ring's slack).
 */
bool	video_flash_safe(void);

#endif /* HAL_VIDEO_H */

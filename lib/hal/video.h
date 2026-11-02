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

#include "video_modes.h"

/*
 * Fill `pixels` (video_width() RGB565 values, 4-byte aligned) for visible
 * line `line` (0..video_height()-1). Runs on core 0 in IRQ context; must
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
 * Start output in `mode` (enum video_mode_id, see video_modes.h): set the
 * core voltage, flash timing and sysclk for the mode, bring up the PIO/DMA
 * serialisers and launch core 1. Never returns failure: if video can't
 * start the device is useless, so it panics (STYLE.md §7). Call once, from
 * core 0, at boot; changing mode means rebooting.
 */
void	video_init(unsigned mode, video_scanline_fn scanline, void *ctx);

/* The running mode's active size, and sysclk cycles per line (budget). */
unsigned video_width(void);
unsigned video_height(void);
unsigned video_line_budget(void);

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

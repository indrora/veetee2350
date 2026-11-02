/*
 * video.h — DVI scanout HAL (640x480p60, RGB565 scanlines).
 *
 * Layering: lowest level (STYLE.md §4). The renderer above us never sees
 * TMDS, PIO or DMA; it only fills one 640-pixel RGB565 line at a time.
 *
 * Model: video_init() hands core 1 over to scanout. Core 1 then loops
 * forever: for each visible line it calls the registered scanline
 * callback, TMDS-encodes the result with the RP2350's SIO encoder, and
 * queues it for DMA. The callback therefore runs ON CORE 1, from RAM,
 * with a hard budget (see VIDEO_LINE_BUDGET_CYCLES) — anything it reads
 * that core 0 writes must be designed for concurrent access.
 */
#ifndef HAL_VIDEO_H
#define HAL_VIDEO_H

#include <stdint.h>

#define VIDEO_WIDTH	640
#define VIDEO_HEIGHT	480

/*
 * One 640x480p60 line is 800 pixel clocks including blanking. At one TMDS
 * bit per sysclk cycle and 10 bits per pixel, that is 8000 sysclk cycles
 * per line at 252 MHz. Render + encode must average below this or DVI
 * starts emitting "late" lines (solid colour) to stay in sync.
 */
#define VIDEO_SYSCLK_KHZ		252000
#define VIDEO_LINE_BUDGET_CYCLES	8000

/*
 * Fill `pixels` (VIDEO_WIDTH RGB565 values, 4-byte aligned) for visible
 * line `line` (0..VIDEO_HEIGHT-1). Runs on core 1; must live in RAM
 * (__not_in_flash_func) and must not block.
 */
typedef void (*video_scanline_fn)(uint16_t *pixels, unsigned line, void *ctx);

struct video_stats {
	uint32_t	frames;		/* frames completed since start */
	uint32_t	max_line_cycles;/* worst render+encode, last frame */
	uint32_t	avg_line_cycles;/* mean render+encode, last frame */
};

/*
 * Raise VREG, confirm sysclk is 252 MHz, bring up PIO/DMA serialisers and
 * launch core 1. Never returns failure: if video can't start the device is
 * useless, so it panics (STYLE.md §7). Call once, from core 0.
 */
void	video_init(video_scanline_fn scanline, void *ctx);

/* Snapshot of scanout statistics; safe to call from core 0 at any time. */
void	video_get_stats(struct video_stats *out);

#endif /* HAL_VIDEO_H */

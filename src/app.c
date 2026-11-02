/*
 * app.c — application glue. Milestone 1: DVI bring-up test pattern.
 *
 * The pattern is designed to be checked from a framegrabber capture:
 *   - 1-pixel white border: proves the full 640x480 is visible and that
 *     the capture isn't scaling or cropping.
 *   - eight colour bars (lines 0-239): proves lane order. Wrong pin
 *     mapping shows up as swapped colours (e.g. yellow <-> cyan).
 *   - R, G, B and grey ramps (lines 240-399): proves every channel bit,
 *     and that RGB565 maps onto the lanes the right way round.
 *   - 1-pixel checkerboard (lines 400-479): proves pixel-exact sampling.
 *   - a small square moving left to right: proves the picture is live.
 * Scanout statistics are printed over USB serial once per second.
 */
#include "app.h"

#include <stdint.h>

#include "console.h"
#include "video.h"

#include "pico.h"
#include "pico/time.h"

static inline uint16_t
rgb565(unsigned red, unsigned green, unsigned blue)
{
	return (uint16_t)(((red >> 3) << 11) | ((green >> 2) << 5) |
	    (blue >> 3));
}

/* Bumped by the scanline callback at line 0; only core 1 writes it. */
static uint32_t pattern_frame;

static void __not_in_flash_func(test_pattern_line)(uint16_t *pixels,
    unsigned line, void *ctx)
{
	static const uint16_t bars[8] = {
		0xffff,	/* white */
		0xffe0,	/* yellow */
		0x07ff,	/* cyan */
		0x07e0,	/* green */
		0xf81f,	/* magenta */
		0xf800,	/* red */
		0x001f,	/* blue */
		0x0000,	/* black */
	};
	const unsigned bar_width = VIDEO_WIDTH / 8;
	(void)ctx;

	if (line == 0)
		pattern_frame++;

	if (line < 240) {
		for (unsigned col = 0; col < VIDEO_WIDTH; col++)
			pixels[col] = bars[col / bar_width];
	} else if (line < 400) {
		/* Four 40-line ramps: red, green, blue, grey. */
		unsigned ramp = (line - 240) / 40;
		for (unsigned col = 0; col < VIDEO_WIDTH; col++) {
			unsigned level = col * 256 / VIDEO_WIDTH;
			switch (ramp) {
			case 0:
				pixels[col] = rgb565(level, 0, 0);
				break;
			case 1:
				pixels[col] = rgb565(0, level, 0);
				break;
			case 2:
				pixels[col] = rgb565(0, 0, level);
				break;
			default:
				pixels[col] = rgb565(level, level, level);
				break;
			}
		}
	} else {
		for (unsigned col = 0; col < VIDEO_WIDTH; col++)
			pixels[col] = ((col ^ line) & 1) ? 0xffff : 0x0000;
	}

	/* 32x32 marker sweeping across the bars, 2 px per frame. */
	unsigned marker_x = (pattern_frame * 2) % (VIDEO_WIDTH - 32);
	if (line >= 100 && line < 132)
		for (unsigned col = marker_x; col < marker_x + 32; col++)
			pixels[col] = 0x0000;

	/* White frame border, drawn last so nothing covers it. */
	if (line == 0 || line == VIDEO_HEIGHT - 1) {
		for (unsigned col = 0; col < VIDEO_WIDTH; col++)
			pixels[col] = 0xffff;
	} else {
		pixels[0] = 0xffff;
		pixels[VIDEO_WIDTH - 1] = 0xffff;
	}
}

static absolute_time_t next_report;

void
app_init(void)
{
	video_init(test_pattern_line, nullptr);
	next_report = make_timeout_time_ms(1000);
}

void
app_poll(void)
{
	struct video_stats stats;

	if (!time_reached(next_report))
		return;
	next_report = delayed_by_ms(next_report, 1000);

	video_get_stats(&stats);
	console_printf("frames=%lu line cycles avg=%lu max=%lu (budget %u)\r\n",
	    (unsigned long)stats.frames, (unsigned long)stats.avg_line_cycles,
	    (unsigned long)stats.max_line_cycles, VIDEO_LINE_BUDGET_CYCLES);
}

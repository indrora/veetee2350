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
#include <string.h>

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

/*
 * The pattern bands are built once at init (core 0) and copied per line
 * (core 1). A first version computed each pixel inline, with a hardware
 * divide per pixel, and cost ~19k cycles/line against an 8k budget. This
 * keeps the callback a single memcpy so the stats measure the HAL itself.
 */
enum pattern_row {
	ROW_BARS,
	ROW_RAMP_RED,
	ROW_RAMP_GREEN,
	ROW_RAMP_BLUE,
	ROW_RAMP_GREY,
	ROW_CHECKER_EVEN,
	ROW_CHECKER_ODD,
	ROW_COUNT
};

static uint16_t pattern_rows[ROW_COUNT][VIDEO_WIDTH]
    __attribute__((aligned(4)));

static void
build_pattern_rows(void)
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

	for (unsigned col = 0; col < VIDEO_WIDTH; col++) {
		unsigned level = col * 256 / VIDEO_WIDTH;

		pattern_rows[ROW_BARS][col] = bars[col / (VIDEO_WIDTH / 8)];
		pattern_rows[ROW_RAMP_RED][col] = rgb565(level, 0, 0);
		pattern_rows[ROW_RAMP_GREEN][col] = rgb565(0, level, 0);
		pattern_rows[ROW_RAMP_BLUE][col] = rgb565(0, 0, level);
		pattern_rows[ROW_RAMP_GREY][col] = rgb565(level, level, level);
		pattern_rows[ROW_CHECKER_EVEN][col] = (col & 1) ? 0xffff : 0;
		pattern_rows[ROW_CHECKER_ODD][col] = (col & 1) ? 0 : 0xffff;
	}
}

static void __not_in_flash_func(test_pattern_line)(uint16_t *pixels,
    unsigned line, void *ctx)
{
	enum pattern_row row;
	(void)ctx;

	if (line == 0)
		pattern_frame++;

	if (line < 240)
		row = ROW_BARS;
	else if (line < 400)
		row = ROW_RAMP_RED + (line - 240) / 40;
	else
		row = (line & 1) ? ROW_CHECKER_ODD : ROW_CHECKER_EVEN;
	memcpy(pixels, pattern_rows[row], sizeof(pattern_rows[row]));

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
	build_pattern_rows();
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
	console_printf("frames=%lu line cycles avg=%lu (encode %lu) max=%lu "
	    "budget=%u\r\n", (unsigned long)stats.frames,
	    (unsigned long)stats.avg_line_cycles,
	    (unsigned long)stats.avg_encode_cycles,
	    (unsigned long)stats.max_line_cycles, VIDEO_LINE_BUDGET_CYCLES);
}

/*
 * app.c — application glue. Milestone 2: text renderer demo.
 *
 * Shows a demo screen in each built-in font, switching every 8 seconds,
 * to prove: glyph rendering, the 16 ANSI colours, a truecolor gradient
 * (quantised to RGB565 on output), box drawing / DEC graphics glyphs,
 * every text attribute, the blinking cursor, and runtime font switching.
 * The lower part of the screen is packed solid with coloured text so the
 * per-line statistics printed over USB serial reflect the worst case.
 *
 * Temporary: Goal 1's terminal emulator (libvterm -> grid) replaces the
 * hand-written demo content.
 */
#include "app.h"

#include <stdint.h>
#include <string.h>

#include "console.h"
#include "font.h"
#include "grid.h"
#include "render.h"
#include "video.h"

#include "pico.h"
#include "pico/time.h"

static_assert(RENDER_WIDTH == VIDEO_WIDTH && RENDER_HEIGHT == VIDEO_HEIGHT);

#define FONT_CYCLE_MS	8000

static struct font font;
static struct grid grid;
static struct render render;
static unsigned font_index;

/* xterm's default 16-colour palette. */
static const uint8_t ansi16[16][3] = {
	{0, 0, 0}, {205, 0, 0}, {0, 205, 0}, {205, 205, 0},
	{0, 0, 238}, {205, 0, 205}, {0, 205, 205}, {229, 229, 229},
	{127, 127, 127}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0},
	{92, 92, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255},
};

static uint16_t
ansi(unsigned index)
{
	return grid_rgb565(ansi16[index][0], ansi16[index][1],
	    ansi16[index][2]);
}

/* Minimal UTF-8 decoder for the demo's string literals (trusted input). */
static uint32_t
next_codepoint(const char **text)
{
	const uint8_t *p = (const uint8_t *)*text;
	uint32_t codepoint;
	int extra;

	if (p[0] < 0x80) {
		codepoint = p[0];
		extra = 0;
	} else if (p[0] < 0xe0) {
		codepoint = p[0] & 0x1f;
		extra = 1;
	} else if (p[0] < 0xf0) {
		codepoint = p[0] & 0x0f;
		extra = 2;
	} else {
		codepoint = p[0] & 0x07;
		extra = 3;
	}
	for (int i = 1; i <= extra; i++)
		codepoint = codepoint << 6 | (p[i] & 0x3f);
	*text += 1 + extra;
	return codepoint;
}

static unsigned
put_text(unsigned row, unsigned col, const char *text, uint16_t fg,
    uint16_t bg, uint16_t attr)
{
	struct cell *cells;

	if (row >= grid.rows)
		return col;
	cells = grid_row(&grid, row);
	while (*text != '\0' && col < grid.cols) {
		uint32_t codepoint = next_codepoint(&text);
		cells[col++] = grid_make_cell(font_glyph_for(&font, codepoint),
		    fg, bg, attr);
	}
	return col;
}

static void
draw_demo_screen(void)
{
	const uint16_t white = ansi(15);
	const uint16_t grey = ansi(7);
	const uint16_t black = ansi(0);
	unsigned col;
	unsigned row = 0;

	col = put_text(row, 0, "VeeTee 2350", ansi(11), black, 0);
	col = put_text(row, col + 1, "font:", grey, black, 0);
	put_text(row, col + 1, font_builtins[font_index].name, white, black,
	    CELL_UNDERLINE);
	row += 2;

	/* 16 ANSI colours as swatches with their index. */
	for (unsigned index = 0; index < 16; index++) {
		char label[4] = {' ', "0123456789abcdef"[index], ' ', 0};
		put_text(row, index * 4, label, index < 8 && index != 7 ?
		    white : black, ansi(index), 0);
	}
	row += 1;

	/* Truecolor: a hue sweep across the full width, as cell bg. */
	for (unsigned c = 0; c < grid.cols; c++) {
		unsigned phase = c * 1536 / grid.cols;
		uint8_t ramp = (uint8_t)(phase % 256);
		uint8_t red, green, blue;

		switch (phase / 256) {
		case 0: red = 255; green = ramp; blue = 0; break;
		case 1: red = 255 - ramp; green = 255; blue = 0; break;
		case 2: red = 0; green = 255; blue = ramp; break;
		case 3: red = 0; green = 255 - ramp; blue = 255; break;
		case 4: red = ramp; green = 0; blue = 255; break;
		default: red = 255; green = 0; blue = 255 - ramp; break;
		}
		grid_row(&grid, row)[c] = grid_make_cell(font_glyph_for(&font,
		    ' '), white, grid_rgb565(red, green, blue), 0);
	}
	row += 2;

	put_text(row++, 0, "┌──────┬──────┐ ╔══════╗ ░▒▓█ ▀▄▌▐ ◆ ⎺⎻─⎼⎽",
	    ansi(14), black, 0);
	put_text(row++, 0, "│ box  │ test │ ║ dbl  ║ ␉␊␌␍ ≤≥π≠£·°±",
	    ansi(14), black, 0);
	put_text(row++, 0, "└──────┴──────┘ ╚══════╝ Ж Ω é ñ ß",
	    ansi(14), black, 0);
	row++;

	col = put_text(row, 0, "underline", white, black, CELL_UNDERLINE);
	col = put_text(row, col + 1, "blink", white, black, CELL_BLINK);
	col = put_text(row, col + 1, "strike", white, black, CELL_STRIKE);
	col = put_text(row, col + 1, "overline", white, black, CELL_OVERLINE);
	col = put_text(row, col + 1, "reverse", black, white, 0);
	col = put_text(row, col + 1, "invisible:", grey, black, 0);
	col = put_text(row, col, "secret", white, black, CELL_INVISIBLE);
	put_text(row, col, "|", grey, black, 0);
	row += 2;

	/* Worst case for the renderer: every remaining cell inked and
	 * coloured, every fourth one carrying attributes. */
	unsigned glyph_cursor = 0;
	for (; row + 1 < grid.rows; row++) {
		struct cell *cells = grid_row(&grid, row);
		for (unsigned c = 0; c < grid.cols; c++, glyph_cursor++) {
			uint32_t codepoint = '!' + glyph_cursor % 94;
			uint16_t attr = (c % 4 == 3) ? CELL_UNDERLINE : 0;
			cells[c] = grid_make_cell(font_glyph_for(&font,
			    codepoint), ansi(9 + glyph_cursor % 7),
			    ansi(glyph_cursor % 3 == 0 ? 4 : 0), attr);
		}
	}

	put_text(grid.rows - 1, 0, "$ ", ansi(10), black, 0);
	grid.cursor_row = grid.rows - 1;
	grid.cursor_col = 2;
}

/*
 * (Re)load a built-in font and rebuild the grid to match. Pausing the
 * renderer first means core 1 never sees a half-swapped font/grid pair.
 */
static void
switch_font(unsigned index, bool video_running)
{
	struct cell blank;
	unsigned cols, rows;

	if (video_running)
		render_pause(&render);
	grid_free(&grid);
	font_free(&font);

	font_index = index % font_builtin_count;
	const struct font_builtin *builtin = &font_builtins[font_index];
	if (font_load_psf2(&font, builtin->data, builtin->len) != FONT_OK)
		panic("font: cannot load %s", builtin->name);
	render_grid_size(&font, &cols, &rows);
	blank = grid_make_cell(font_glyph_for(&font, ' '), ansi(7), ansi(0),
	    0);
	if (!grid_init(&grid, cols, rows, blank))
		panic("grid: cannot allocate %ux%u", cols, rows);
	draw_demo_screen();
	render_init(&render, &font, &grid);
}

static absolute_time_t next_report;
static absolute_time_t next_font;

/* DEBUG: render cost on core 0 with no DVI running (no DMA, no IRQs). */
static uint32_t bench_cycles;
static uint16_t bench_line[RENDER_WIDTH] __attribute__((aligned(4)));

#include "hardware/structs/systick.h"
static void
bench_render(void)
{
	systick_hw->rvr = 0x00ffffff;
	systick_hw->cvr = 0;
	systick_hw->csr = 0x5;
	uint32_t start = systick_hw->cvr;
	for (unsigned line = 200; line < 300; line++)
		render_scanline(bench_line, line, &render);
	bench_cycles = ((start - systick_hw->cvr) & 0x00ffffff) / 100;
}

void
app_init(void)
{
	switch_font(0, false);
	bench_render();
	video_init(render_scanline, &render);
	next_report = make_timeout_time_ms(1000);
	next_font = make_timeout_time_ms(FONT_CYCLE_MS);
}

void
app_poll(void)
{
	struct video_stats stats;

	if (time_reached(next_font)) {
		next_font = delayed_by_ms(next_font, FONT_CYCLE_MS);
		switch_font(font_index + 1, true);
	}
	if (!time_reached(next_report))
		return;
	next_report = delayed_by_ms(next_report, 1000);

	video_get_stats(&stats);
	console_printf("bench(core0, idle) render=%lu cycles/line\r\n",
	    (unsigned long)bench_cycles);
	console_printf("%-18s %ux%u frames=%lu line avg=%lu (encode %lu) "
	    "max=%lu budget=%u\r\n", font_builtins[font_index].name,
	    grid.cols, grid.rows, (unsigned long)stats.frames,
	    (unsigned long)stats.avg_line_cycles,
	    (unsigned long)stats.avg_encode_cycles,
	    (unsigned long)stats.max_line_cycles, VIDEO_LINE_BUDGET_CYCLES);
}

/*
 * test_render — grid scrolling and pixel-exact scanline output, on host.
 */
#include <string.h>

#include <unity.h>

#include "font.h"
#include "grid.h"
#include "render.h"

#define WHITE	0xffff
#define BLUE	0x001f
#define BLACK	0x0000

static struct font font;
static struct grid grid;
static struct render render;
static uint16_t pixels[RENDER_MAX_WIDTH] __attribute__((aligned(4)));

static struct cell
blank_cell(void)
{
	return grid_make_cell(font_glyph_for(&font, ' '), WHITE, BLACK, 0);
}

void
setUp(void)
{
	unsigned cols, rows;

	TEST_ASSERT_EQUAL(FONT_OK, font_load_psf2(&font, font_builtins[0].data,
	    font_builtins[0].len));
	render_set_screen(&render, 640, 480);
	render_grid_size(&font, 640, 480, &cols, &rows);
	TEST_ASSERT_TRUE(grid_init(&grid, cols, rows, blank_cell()));
	render_init(&render, &font, &grid);
	grid.cursor_visible = false;
}

void
tearDown(void)
{
	grid_free(&grid);
	font_free(&font);
}

/* Expected pixel for column x of glyph row `sub` of `codepoint`. */
static uint16_t
expected(uint32_t codepoint, unsigned sub, unsigned x, uint16_t fg,
    uint16_t bg)
{
	uint8_t bits = *font_glyph_row(&font, font_glyph_for(&font, codepoint),
	    sub);
	return (bits >> (7 - x)) & 1 ? fg : bg;
}

static void
test_grid_size_default_font(void)
{
	TEST_ASSERT_EQUAL(80, grid.cols);
	TEST_ASSERT_EQUAL(30, grid.rows);
	TEST_ASSERT_EQUAL(0, render.x_offset);
	TEST_ASSERT_EQUAL(0, render.y_offset);
}

static void
test_glyph_pixels(void)
{
	grid_row(&grid, 0)[0] = grid_make_cell(font_glyph_for(&font, 'A'),
	    WHITE, BLUE, 0);
	for (unsigned sub = 0; sub < font.height; sub++) {
		render_scanline(pixels, sub, &render);
		for (unsigned x = 0; x < 8; x++)
			TEST_ASSERT_EQUAL_HEX16(expected('A', sub, x, WHITE,
			    BLUE), pixels[x]);
		/* Neighbouring blank cell is all background. */
		for (unsigned x = 8; x < 16; x++)
			TEST_ASSERT_EQUAL_HEX16(BLACK, pixels[x]);
	}
}

static void
test_reverse_video_and_underline(void)
{
	grid_row(&grid, 0)[1] = grid_make_cell(font_glyph_for(&font, 'g'),
	    WHITE, BLUE, CELL_UNDERLINE);
	grid.reverse_video = true;
	render_scanline(pixels, 5, &render);
	for (unsigned x = 0; x < 8; x++)
		TEST_ASSERT_EQUAL_HEX16(expected('g', 5, x, BLUE, WHITE),
		    pixels[8 + x]);
	/* Underline row: solid "foreground", which reverse makes bg. */
	render_scanline(pixels, font.underline_row, &render);
	for (unsigned x = 0; x < 8; x++)
		TEST_ASSERT_EQUAL_HEX16(BLUE, pixels[8 + x]);
}

static void
test_block_cursor(void)
{
	grid.cursor_visible = true;
	grid.cursor_blink = false;
	grid.cursor_row = 0;
	grid.cursor_col = 2;
	render_scanline(pixels, 3, &render);
	/* Cursor on a blank cell: solid foreground block. */
	for (unsigned x = 16; x < 24; x++)
		TEST_ASSERT_EQUAL_HEX16(WHITE, pixels[x]);
	TEST_ASSERT_EQUAL_HEX16(BLACK, pixels[24]);
}

static void
test_scroll_up_reuses_rows(void)
{
	grid_row(&grid, 1)[0] = grid_make_cell(font_glyph_for(&font, 'X'),
	    WHITE, BLUE, 0);
	grid_scroll_up(&grid, 0, grid.rows, 1, blank_cell());
	TEST_ASSERT_EQUAL(font_glyph_for(&font, 'X'), grid_row(&grid, 0)[0].glyph);
	TEST_ASSERT_EQUAL(font_glyph_for(&font, ' '),
	    grid_row(&grid, grid.rows - 1)[0].glyph);

	grid_scroll_down(&grid, 0, grid.rows, 2, blank_cell());
	TEST_ASSERT_EQUAL(font_glyph_for(&font, 'X'), grid_row(&grid, 2)[0].glyph);
	TEST_ASSERT_EQUAL(font_glyph_for(&font, ' '), grid_row(&grid, 0)[0].glyph);
}

static void
test_border_for_uneven_font(void)
{
	/* Clean 13: 480 / 13 = 36 rows = 468 lines, 6 border lines on top. */
	struct font font13;
	struct grid grid13;
	struct render render13;
	unsigned cols, rows;

	TEST_ASSERT_EQUAL(FONT_OK, font_load_psf2(&font13, font_builtins[1].data,
	    font_builtins[1].len));
	render_set_screen(&render13, 640, 480);
	render_grid_size(&font13, 640, 480, &cols, &rows);
	TEST_ASSERT_EQUAL(36, rows);
	TEST_ASSERT_TRUE(grid_init(&grid13, cols, rows,
	    grid_make_cell(0, WHITE, BLUE, 0)));
	render_init(&render13, &font13, &grid13);
	TEST_ASSERT_EQUAL(6, render13.y_offset);
	render_scanline(pixels, 0, &render13);
	TEST_ASSERT_EQUAL_HEX16(BLUE, pixels[0]);
	TEST_ASSERT_EQUAL(RENDER_LINE_BORDER, render13.line_row[5]);
	TEST_ASSERT_EQUAL(0, render13.line_row[6]);
	grid_free(&grid13);
	font_free(&font13);
}

static void
test_720p_geometry(void)
{
	/* 1280x720 with the 8x16 font: 160x45, exactly filling the screen. */
	struct grid wide;
	struct render render720;
	unsigned cols, rows;

	render_set_screen(&render720, 1280, 720);
	render_grid_size(&font, 1280, 720, &cols, &rows);
	TEST_ASSERT_EQUAL(160, cols);
	TEST_ASSERT_EQUAL(45, rows);
	TEST_ASSERT_TRUE(grid_init(&wide, cols, rows, blank_cell()));
	grid_row(&wide, 44)[159] = grid_make_cell(font_glyph_for(&font, 'Z'),
	    WHITE, BLUE, 0);
	render_init(&render720, &font, &wide);
	render720.grid->cursor_visible = false;
	TEST_ASSERT_EQUAL(0, render720.x_offset);
	TEST_ASSERT_EQUAL(0, render720.y_offset);
	render_scanline(pixels, 719, &render720);	/* last line of row 44 */
	for (unsigned x = 0; x < 8; x++)
		TEST_ASSERT_EQUAL_HEX16(expected('Z', 15, x, WHITE, BLUE),
		    pixels[1272 + x]);
	grid_free(&wide);
}

static void
test_overlay_covers_cells_and_cursor(void)
{
	struct cell box[2] = {
		grid_make_cell(font_glyph_for(&font, 'X'), BLUE, BLACK, 0),
		grid_make_cell(font_glyph_for(&font, 'Y'), BLUE, BLACK, 0),
	};
	struct render_overlay over = {.row = 0, .rows = 1, .col = 2,
	    .cols = 2, .cells = box};

	grid_row(&grid, 0)[0] = grid_make_cell(font_glyph_for(&font, 'a'),
	    WHITE, BLACK, 0);
	grid.cursor_visible = true;
	grid.cursor_blink = false;
	grid.cursor_row = 0;
	grid.cursor_col = 2;
	render.overlay[RENDER_OVERLAY_MENU] = &over;
	render_plan(&render);		/* overlays show through the plan */
	TEST_ASSERT_EQUAL(3, render.plan->row[0].spans);
	TEST_ASSERT_TRUE(render.plan->row[0].covered);
	TEST_ASSERT_EQUAL(RENDER_SPAN_MIXED == render.plan->row[0].span[1].kind,
	    false);
	render_scanline(pixels, 3, &render);
	render.overlay[RENDER_OVERLAY_MENU] = nullptr;
	render_plan(&render);
	for (unsigned x = 0; x < 8; x++) {
		/* The grid shows through beside it... */
		TEST_ASSERT_EQUAL_HEX16(expected('a', 3, x, WHITE, BLACK),
		    pixels[x]);
		/* ...the overlay covers columns 2-3, cursor and all. */
		TEST_ASSERT_EQUAL_HEX16(expected('X', 3, x, BLUE, BLACK),
		    pixels[16 + x]);
		TEST_ASSERT_EQUAL_HEX16(expected('Y', 3, x, BLUE, BLACK),
		    pixels[24 + x]);
	}
	/* The grid itself is untouched. */
	TEST_ASSERT_EQUAL(font_glyph_for(&font, ' '),
	    grid_row(&grid, 0)[2].glyph);
}

/*
 * A plan a frame behind the grid: the row was planned PLAIN, then the
 * host underlined a cell. The lean loop still draws it underlined.
 */
static void
test_stale_plain_plan(void)
{
	grid_row(&grid, 1)[0] = grid_make_cell(font_glyph_for(&font, 'a'),
	    WHITE, BLACK, 0);
	render_plan(&render);
	render_plan(&render);	/* the second has last frame's pair */
	TEST_ASSERT_EQUAL(RENDER_SPAN_PAIR, render.plan->row[1].span[0].kind);
	grid_row(&grid, 1)[0].attr = CELL_UNDERLINE;
	unsigned line = 16 + font.underline_row;
	render_scanline(pixels, line, &render);
	for (unsigned x = 0; x < 8; x++)
		TEST_ASSERT_EQUAL_HEX16(WHITE, pixels[x]);
	render_plan(&render);
	TEST_ASSERT_EQUAL(RENDER_SPAN_MIXED, render.plan->row[1].span[0].kind);
	grid_row(&grid, 1)[0].attr = 0;
}

/* A PAIR span draws exactly what the full loop would, inverted too. */
static void
test_pair_table(void)
{
	for (unsigned row = 0; row < grid.rows; row++)
		for (unsigned col = 0; col < grid.cols; col++)
			grid_row(&grid, row)[col] = grid_make_cell(
			    font_glyph_for(&font, (col & 1) ? 'Q' : 'g'),
			    WHITE, BLUE, 0);
	grid.cursor_visible = false;
	render_plan(&render);
	render_plan(&render);	/* now with this screen's pair */
	TEST_ASSERT_EQUAL(RENDER_SPAN_PAIR, render.plan->row[2].span[0].kind);
	for (unsigned sub = 0; sub < font.height; sub++) {
		render_scanline(pixels, 2 * font.height + sub, &render);
		for (unsigned x = 0; x < 16; x++)
			TEST_ASSERT_EQUAL_HEX16(expected(x < 8 ? 'g' : 'Q',
			    sub, x % 8, WHITE, BLUE), pixels[x]);
	}
	/* Reverse video: until a plan rebuilds the table, the plain loop. */
	grid.reverse_video = true;
	for (int pass = 0; pass < 2; pass++) {
		render_scanline(pixels, 2 * font.height + 5, &render);
		for (unsigned x = 0; x < 16; x++)
			TEST_ASSERT_EQUAL_HEX16(expected(x < 8 ? 'g' : 'Q', 5,
			    x % 8, BLUE, WHITE), pixels[x]);
		render_plan(&render);
	}
	grid.reverse_video = false;
}

int
main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_grid_size_default_font);
	RUN_TEST(test_glyph_pixels);
	RUN_TEST(test_reverse_video_and_underline);
	RUN_TEST(test_block_cursor);
	RUN_TEST(test_overlay_covers_cells_and_cursor);
	RUN_TEST(test_stale_plain_plan);
	RUN_TEST(test_pair_table);
	RUN_TEST(test_scroll_up_reuses_rows);
	RUN_TEST(test_border_for_uneven_font);
	RUN_TEST(test_720p_geometry);
	return UNITY_END();
}

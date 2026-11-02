/*
 * test_term — escape sequences in, grid cells (and host replies) out.
 *
 * Runs the real libvterm + term glue on the host against the default
 * font's 80x30 grid. Each test feeds a byte string and inspects cells.
 */
#include <string.h>

#include <unity.h>

#include "font.h"
#include "grid.h"
#include "render.h"
#include "term.h"

static struct render render;
static struct term term;
static char reply[128];
static size_t reply_len;

static void
capture_output(const char *bytes, size_t len, void *ctx)
{
	(void)ctx;
	if (reply_len + len < sizeof(reply)) {
		memcpy(reply + reply_len, bytes, len);
		reply_len += len;
		reply[reply_len] = '\0';
	}
}

static void
feed(const char *text)
{
	term_feed(&term, (const uint8_t *)text, strlen(text));
}

static const struct cell *
cell_at(unsigned row, unsigned col)
{
	return &grid_row(term.screen, row)[col];
}

static uint16_t
glyph(uint32_t codepoint)
{
	return font_glyph_for(&term.font, codepoint);
}

/* Foreground RGB565 of a cell (cells store bg and fg^bg). */
static uint16_t
cell_fg(const struct cell *cell)
{
	return (uint16_t)(cell->bg2 ^ cell->diff2);
}

static uint16_t
cell_bg(const struct cell *cell)
{
	return (uint16_t)cell->bg2;
}

void
setUp(void)
{
	render_set_screen(&render, 640, 480);
	TEST_ASSERT_TRUE(term_init(&term, &font_builtins[0], &render,
	    capture_output, nullptr));
	reply_len = 0;
	reply[0] = '\0';
}

void
tearDown(void)
{
	term_free(&term);
}

static void
test_text_and_cursor(void)
{
	feed("Hi");
	TEST_ASSERT_EQUAL(glyph('H'), cell_at(0, 0)->glyph);
	TEST_ASSERT_EQUAL(glyph('i'), cell_at(0, 1)->glyph);
	TEST_ASSERT_EQUAL(2, term.screen->cursor_col);
	feed("\r\nok");
	TEST_ASSERT_EQUAL(glyph('o'), cell_at(1, 0)->glyph);
	TEST_ASSERT_EQUAL(1, term.screen->cursor_row);
}

static void
test_cup_and_erase(void)
{
	feed("\x1b[5;10HX");	/* CUP is 1-based */
	TEST_ASSERT_EQUAL(glyph('X'), cell_at(4, 9)->glyph);
	feed("\x1b[2J");	/* ED 2: erase display */
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(4, 9)->glyph);
}

static void
test_sgr_colours(void)
{
	feed("\x1b[38;2;10;200;30mA");	/* truecolor fg */
	TEST_ASSERT_EQUAL_HEX16(grid_rgb565(10, 200, 30), cell_fg(cell_at(0, 0)));

	feed("\x1b[0;41mB");		/* indexed bg: red */
	uint16_t bg = cell_bg(cell_at(0, 1));
	TEST_ASSERT_TRUE((bg >> 11) > 20);	/* lots of red */
	TEST_ASSERT_EQUAL(0, bg & 0x07ff);	/* no green or blue */

	/* Bold + colour 1 is drawn as bright red (colour 9). */
	feed("\x1b[0;31mC\x1b[1mD");
	TEST_ASSERT_NOT_EQUAL(cell_fg(cell_at(0, 2)), cell_fg(cell_at(0, 3)));
}

static void
test_attributes(void)
{
	feed("\x1b[4mU\x1b[0;9mS\x1b[0;8mH\x1b[0;7mR");
	TEST_ASSERT_EQUAL(CELL_UNDERLINE, cell_at(0, 0)->attr);
	TEST_ASSERT_EQUAL(CELL_STRIKE, cell_at(0, 1)->attr);
	TEST_ASSERT_EQUAL(CELL_INVISIBLE, cell_at(0, 2)->attr);
	/* Reverse swaps colours: fg becomes the default background. */
	TEST_ASSERT_EQUAL_HEX16(term.default_bg, cell_fg(cell_at(0, 3)));
	TEST_ASSERT_EQUAL_HEX16(term.default_fg, cell_bg(cell_at(0, 3)));
}

static void
test_scroll_at_bottom(void)
{
	feed("first");
	for (unsigned i = 0; i < term.screen->rows; i++)
		feed("\r\n");
	feed("last");
	/* "first" scrolled off the top; "last" is on the bottom row. */
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(0, 0)->glyph);
	TEST_ASSERT_EQUAL(glyph('l'), cell_at(term.screen->rows - 1, 0)->glyph);
}

static void
test_scroll_region(void)
{
	feed("\x1b[1;1Htop\x1b[3;1Hmid\x1b[10;1Hlow");
	feed("\x1b[2;5r");		/* DECSTBM rows 2-5 */
	feed("\x1b[5;1H\n");		/* LF at the bottom margin */
	TEST_ASSERT_EQUAL(glyph('t'), cell_at(0, 0)->glyph);	/* outside */
	TEST_ASSERT_EQUAL(glyph('m'), cell_at(1, 0)->glyph);	/* moved up */
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(2, 0)->glyph);
	TEST_ASSERT_EQUAL(glyph('l'), cell_at(9, 0)->glyph);	/* outside */
}

static void
test_insert_character_uses_moverect(void)
{
	feed("ABC\x1b[1G\x1b[@");	/* ICH 1 at column 1 */
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(0, 0)->glyph);
	TEST_ASSERT_EQUAL(glyph('A'), cell_at(0, 1)->glyph);
	TEST_ASSERT_EQUAL(glyph('C'), cell_at(0, 3)->glyph);
	feed("\x1b[1G\x1b[2P");		/* DCH 2 */
	TEST_ASSERT_EQUAL(glyph('B'), cell_at(0, 0)->glyph);
}

static void
test_alternate_screen(void)
{
	feed("main");
	feed("\x1b[?1049h");
	TEST_ASSERT_EQUAL_PTR(&term.alternate, term.screen);
	TEST_ASSERT_EQUAL_PTR(&term.alternate, render.grid);
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(0, 0)->glyph);
	feed("alt");
	feed("\x1b[?1049l");
	TEST_ASSERT_EQUAL_PTR(&term.primary, render.grid);
	TEST_ASSERT_EQUAL(glyph('m'), cell_at(0, 0)->glyph);
	/* 1049 restores the cursor saved on entry. */
	TEST_ASSERT_EQUAL(4, term.screen->cursor_col);
}

static void
test_dec_special_graphics(void)
{
	feed("\x1b(0lqk\x1b(Bq");	/* G0 = DEC line drawing, then ASCII */
	TEST_ASSERT_EQUAL(glyph(0x250c), cell_at(0, 0)->glyph);	/* ┌ */
	TEST_ASSERT_EQUAL(glyph(0x2500), cell_at(0, 1)->glyph);	/* ─ */
	TEST_ASSERT_EQUAL(glyph(0x2510), cell_at(0, 2)->glyph);	/* ┐ */
	TEST_ASSERT_EQUAL(glyph('q'), cell_at(0, 3)->glyph);
	TEST_ASSERT_NOT_EQUAL(FONT_GLYPH_MISSING, cell_at(0, 0)->glyph);
}

static void
test_modes_reach_grid(void)
{
	feed("\x1b[?5h");	/* DECSCNM */
	TEST_ASSERT_TRUE(term.screen->reverse_video);
	feed("\x1b[?25l");	/* DECTCEM off */
	TEST_ASSERT_FALSE(term.screen->cursor_visible);
	feed("\x1b[4 q");	/* DECSCUSR steady underline */
	TEST_ASSERT_EQUAL(CURSOR_UNDERLINE, term.screen->cursor_shape);
	TEST_ASSERT_FALSE(term.screen->cursor_blink);
}

static void
test_device_replies(void)
{
	feed("\x1b[c");		/* DA1 */
	TEST_ASSERT_EQUAL_STRING_LEN("\x1b[?", reply, 3);
	TEST_ASSERT_EQUAL('c', reply[reply_len - 1]);

	reply_len = 0;
	feed("\x1b[3;7H\x1b[6n");	/* DSR cursor position */
	TEST_ASSERT_EQUAL_STRING("\x1b[3;7R", reply);
}

static void
test_utf8_split_across_writes(void)
{
	/* "é" = C3 A9 delivered one byte at a time, as USB may do. */
	feed("\xc3");
	feed("\xa9");
	TEST_ASSERT_EQUAL(glyph(0xe9), cell_at(0, 0)->glyph);
}

static void
test_osc50_switches_font_and_resets(void)
{
	feed("old text");
	feed("\x1b]50;notjam-prophet-8\a");
	TEST_ASSERT_EQUAL_STRING("notjam-prophet-8", term.font_source->name);
	TEST_ASSERT_EQUAL(8, term.font.height);
	TEST_ASSERT_EQUAL(80, term.screen->cols);
	TEST_ASSERT_EQUAL(60, term.screen->rows);
	TEST_ASSERT_EQUAL_PTR(&term.primary, render.grid);
	TEST_ASSERT_EQUAL_PTR(&term.font, render.font);
	/* Full reset: screen cleared, cursor home. */
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(0, 0)->glyph);
	TEST_ASSERT_EQUAL(0, term.screen->cursor_row);
	/* libvterm knows the new size: resize(1)-style query. */
	feed("\x1b[999;999H\x1b[6n");
	TEST_ASSERT_EQUAL_STRING("\x1b[60;80R", reply);
	/* And the terminal still works at the new size. */
	feed("\x1b[60;1HZ");
	TEST_ASSERT_EQUAL(glyph('Z'), cell_at(59, 0)->glyph);
}

static void
test_osc50_query_cycle_and_unknown(void)
{
	feed("\x1b]50;?\a");
	TEST_ASSERT_EQUAL_STRING("\x1b]50;notjam-clean-16\a", reply);
	feed("\x1b]50;#+\a");
	TEST_ASSERT_EQUAL_PTR(&font_builtins[1], term.font_source);
	feed("\x1b]50;#-\a");
	TEST_ASSERT_EQUAL_PTR(&font_builtins[0], term.font_source);
	feed("\x1b]50;no-such-font\a");
	TEST_ASSERT_EQUAL_PTR(&font_builtins[0], term.font_source);
	/* OSC split across writes (USB packet boundary). */
	feed("\x1b]50;arbata-");
	feed("compact\x1b\\");
	TEST_ASSERT_EQUAL_STRING("arbata-compact", term.font_source->name);
	TEST_ASSERT_EQUAL(36, term.screen->rows);
}

static void
test_size_queries(void)
{
	feed("\x1b[18t");
	TEST_ASSERT_EQUAL_STRING("\x1b[8;30;80t", reply);
	reply_len = 0;
	feed("\x1b[14t");
	TEST_ASSERT_EQUAL_STRING("\x1b[4;480;640t", reply);
	reply_len = 0;
	feed("\x1b[16t");
	TEST_ASSERT_EQUAL_STRING("\x1b[6;16;8t", reply);
}

static void
test_inband_resize_reports(void)
{
	/* DECRQM before enabling: recognised, reset. */
	feed("\x1b[?2048$p");
	TEST_ASSERT_EQUAL_STRING("\x1b[?2048;2$y", reply);
	/* Enabling reports the current size straight away... */
	reply_len = 0;
	feed("\x1b[?2048h");
	TEST_ASSERT_EQUAL_STRING("\x1b[48;30;80;480;640t", reply);
	/* ...and again whenever the size changes (font switch). */
	reply_len = 0;
	feed("\x1b]50;notjam-prophet-8\a");
	TEST_ASSERT_EQUAL_STRING("\x1b[48;60;80;480;640t", reply);
	/* Disabled: no report on change. */
	feed("\x1b[?2048l");
	reply_len = 0;
	reply[0] = '\0';
	feed("\x1b]50;notjam-clean-16\a");
	TEST_ASSERT_EQUAL_STRING("", reply);
	/* Other unknown DEC modes still don't break anything. */
	feed("\x1b[?9999h");
	TEST_ASSERT_EQUAL_STRING("", reply);
}

int
main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_text_and_cursor);
	RUN_TEST(test_cup_and_erase);
	RUN_TEST(test_sgr_colours);
	RUN_TEST(test_attributes);
	RUN_TEST(test_scroll_at_bottom);
	RUN_TEST(test_scroll_region);
	RUN_TEST(test_insert_character_uses_moverect);
	RUN_TEST(test_alternate_screen);
	RUN_TEST(test_dec_special_graphics);
	RUN_TEST(test_modes_reach_grid);
	RUN_TEST(test_device_replies);
	RUN_TEST(test_utf8_split_across_writes);
	RUN_TEST(test_osc50_switches_font_and_resets);
	RUN_TEST(test_osc50_query_cycle_and_unknown);
	RUN_TEST(test_size_queries);
	RUN_TEST(test_inband_resize_reports);
	return UNITY_END();
}

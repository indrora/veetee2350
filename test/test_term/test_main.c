/*
 * test_term — escape sequences in, grid cells (and host replies) out.
 *
 * Runs the real libvterm + term glue on the host against the default
 * font's 80x30 grid. Each test feeds a byte string and inspects cells.
 */
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "font.h"
#include "grid.h"
#include "render.h"
#include "term.h"
#include "colour.h"

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
	return cell->fg;
}

static uint16_t
cell_bg(const struct cell *cell)
{
	return cell->bg;
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

static void
configure(enum term_emulation level, unsigned depth, bool utf8, bool alt)
{
	struct term_options options = term.options;

	options.emulation = level;
	options.colour_depth = depth;
	options.utf8 = utf8;
	options.alt_screen = alt;
	TEST_ASSERT_TRUE(term_configure(&term, term.font_source, &options));
	reply_len = 0;
	reply[0] = '\0';
}

static void
test_colour_module(void)
{
	TEST_ASSERT_EQUAL(196, colour_nearest_256(255, 0, 0));
	TEST_ASSERT_EQUAL(244, colour_nearest_256(128, 128, 128));
	TEST_ASSERT_EQUAL(16, colour_nearest_256(0, 0, 0));
	uint8_t rgb[3];
	colour_256_rgb(196, rgb);
	TEST_ASSERT_EQUAL(255, rgb[0]);
	TEST_ASSERT_EQUAL(0, rgb[1]);
}

static void
test_device_attributes_per_level(void)
{
	feed("\x1b[c");
	TEST_ASSERT_EQUAL_STRING("\x1b[?64;6;22c", reply);	/* xterm */
	reply_len = 0;
	feed("\x1b[>c");
	TEST_ASSERT_EQUAL_STRING("\x1b[>41;390;0c", reply);

	configure(EMU_VT100, DEPTH_MONO, false, false);
	feed("\x1b[c");
	TEST_ASSERT_EQUAL_STRING("\x1b[?1;2c", reply);
	reply_len = 0;
	reply[0] = '\0';
	feed("\x1b[>c");				/* VT100: no DA2 */
	TEST_ASSERT_EQUAL_STRING("", reply);

	configure(EMU_VT220, DEPTH_MONO, false, false);
	feed("\x1b[c");
	TEST_ASSERT_EQUAL_STRING("\x1b[?62;6;22c", reply);
}

static void
test_colour_depths(void)
{
	/* Truecolor: exact. */
	feed("\x1b[38;2;10;200;30mA");
	TEST_ASSERT_EQUAL_HEX16(grid_rgb565(10, 200, 30), cell_fg(cell_at(0, 0)));

	/* 256: snapped to the cube (0,215,0)-ish, not exact. */
	configure(EMU_XTERM, DEPTH_256, true, true);
	feed("\x1b[38;2;10;200;30mA");
	uint8_t rgb[3];
	colour_256_rgb(colour_nearest_256(10, 200, 30), rgb);
	TEST_ASSERT_EQUAL_HEX16(grid_rgb565(rgb[0], rgb[1], rgb[2]),
	    cell_fg(cell_at(0, 0)));

	/* 16: snapped to the nearest ANSI colour, libvterm's green (0,224,0). */
	configure(EMU_XTERM, DEPTH_16, true, true);
	feed("\x1b[38;2;10;230;30mA");
	const uint8_t *green = term.palette16[2];
	TEST_ASSERT_EQUAL_HEX16(grid_rgb565(green[0], green[1], green[2]),
	    cell_fg(cell_at(0, 0)));

	/* Mono: colours ignored. */
	configure(EMU_VT100, DEPTH_MONO, false, false);
	feed("\x1b[31;44mA");
	TEST_ASSERT_EQUAL_HEX16(term.default_fg, cell_fg(cell_at(0, 0)));
	TEST_ASSERT_EQUAL_HEX16(term.default_bg, cell_bg(cell_at(0, 0)));
}

static void
test_utf8_off_and_alt_screen_off(void)
{
	configure(EMU_VT220, DEPTH_MONO, false, false);
	feed("\xe9");				/* Latin-1 e-acute */
	TEST_ASSERT_EQUAL(glyph(0xe9), cell_at(0, 0)->glyph);

	/* 8-bit C1: 0x9B is CSI. */
	feed("\x9b" "5;1HX");
	TEST_ASSERT_EQUAL(glyph('X'), cell_at(4, 0)->glyph);

	feed("\x1b[?1049h");			/* ignored */
	TEST_ASSERT_EQUAL_PTR(&term.primary, term.screen);
	TEST_ASSERT_EQUAL(glyph(0xe9), cell_at(0, 0)->glyph);
}

static void
test_decrqcra_checksums(void)
{
	feed("AB\x1b[3;1H ");			/* a written space at 3,1 */
	feed("\x1b[1;1;1;1;1;1*y");		/* 'A' */
	TEST_ASSERT_EQUAL_STRING("\x1bP1!~0041\x1b\\", reply);
	reply_len = 0;
	feed("\x1b[2;;1;1;1;2*y");		/* "AB" */
	TEST_ASSERT_EQUAL_STRING("\x1bP2!~0083\x1b\\", reply);
	reply_len = 0;
	feed("\x1b[3;;2;1;2;1*y");		/* erased: a space (xterm >= 334) */
	TEST_ASSERT_EQUAL_STRING("\x1bP3!~0020\x1b\\", reply);
	reply_len = 0;
	feed("\x1b[4;;3;1;3;1*y");		/* written space: 0x20 */
	TEST_ASSERT_EQUAL_STRING("\x1bP4!~0020\x1b\\", reply);
	reply_len = 0;
	feed("\x1b[1;1H\x1b[K\x1b[5;;1;1;1;1*y");	/* EL erases */
	TEST_ASSERT_EQUAL_STRING("\x1bP5!~0020\x1b\\", reply);
}

static void
test_selective_erase_keeps_protected(void)
{
	feed("A\x1b[1\"qB\x1b[0\"qC");		/* DECSCA around 'B' */
	feed("\x1b[?2J");				/* DECSED: selective */
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(0, 0)->glyph);
	TEST_ASSERT_EQUAL(glyph('B'), cell_at(0, 1)->glyph);
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(0, 2)->glyph);
	feed("\x1b[2J");				/* ED: everything */
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(0, 1)->glyph);
}

static void
test_double_width_and_height_lines(void)
{
	feed("\x1b#6AB");			/* DECDWL */
	TEST_ASSERT_EQUAL(ROW_DOUBLE_WIDTH, *grid_row_attr(term.screen, 0));
	feed("\x1b[2;1H\x1b#3X\x1b[3;1H\x1b#4X");	/* DECDHL top/bottom */
	TEST_ASSERT_EQUAL(ROW_DOUBLE_TOP, *grid_row_attr(term.screen, 1));
	TEST_ASSERT_EQUAL(ROW_DOUBLE_BOTTOM, *grid_row_attr(term.screen, 2));

	/* Rendered: 'A' at 2x width, so pixel pairs repeat. */
	static uint16_t pixels[RENDER_MAX_WIDTH] __attribute__((aligned(4)));
	render.grid->cursor_visible = false;
	render_scanline(pixels, 8, &render);
	uint8_t bits = *font_glyph_row(&term.font, glyph('A'), 8);
	for (unsigned x = 0; x < 8; x++) {
		bool ink = (bits >> (7 - x)) & 1;
		uint16_t want = ink ? term.default_fg : term.default_bg;
		TEST_ASSERT_EQUAL_HEX16(want, pixels[2 * x]);
		TEST_ASSERT_EQUAL_HEX16(want, pixels[2 * x + 1]);
	}

	/* Scrolling carries the line size with the line; new lines are
	 * normal. */
	feed("\x1b[30;1H\n");
	TEST_ASSERT_EQUAL(ROW_DOUBLE_TOP, *grid_row_attr(term.screen, 0));
	TEST_ASSERT_EQUAL(ROW_NORMAL, *grid_row_attr(term.screen, 29));
	/* ED clears line sizes (libvterm forces them off). */
	feed("\x1b[2J");
	TEST_ASSERT_EQUAL(ROW_NORMAL, *grid_row_attr(term.screen, 0));
}

static void
test_osc_colours(void)
{
	feed("\x1b]4;1;rgb:f0f0/0000/0000;2;#888\x1b\\");
	feed("\x1b]4;1;?;2;?\x1b\\");
	TEST_ASSERT_EQUAL_STRING("\x1b]4;1;rgb:f0f0/0000/0000\x1b\\"
	    "\x1b]4;2;rgb:8080/8080/8080\x1b\\", reply);
	reply_len = 0;
	feed("\x1b]104;1\x1b\\\x1b]4;1;?\x07");	/* reset entry 1 */
	TEST_ASSERT_EQUAL_STRING_LEN("\x1b]4;1;rgb:", reply, 10);
	TEST_ASSERT_NOT_EQUAL(0, strcmp(reply,
	    "\x1b]4;1;rgb:f0f0/0000/0000\x1b\\"));
	reply_len = 0;
	feed("\x1b]10;#f0f0f0;rgb:1/2/3\x07\x1b]10;?;?\x07");
	TEST_ASSERT_EQUAL_STRING("\x1b]10;rgb:f0f0/f0f0/f0f0\x1b\\"
	    "\x1b]11;rgb:1111/2222/3333\x1b\\", reply);
	/* New text uses the new default foreground. */
	feed("Z");
	TEST_ASSERT_EQUAL_HEX16(grid_rgb565(0xf0, 0xf0, 0xf0),
	    cell_fg(cell_at(0, 0)));
}

static void
test_mode_reports(void)
{
	feed("\x1b[4$p");			/* IRM, real state */
	TEST_ASSERT_EQUAL_STRING("\x1b[4;2$y", reply);
	reply_len = 0;
	feed("\x1b[4h\x1b[4$p");
	TEST_ASSERT_EQUAL_STRING("\x1b[4;1$y", reply);
	reply_len = 0;
	feed("\x1b[7$p");			/* VEM: permanently reset */
	TEST_ASSERT_EQUAL_STRING("\x1b[7;4$y", reply);
	reply_len = 0;
	feed("\x1b[?67h\x1b[?67$p");		/* DECBKM stored */
	TEST_ASSERT_EQUAL_STRING("\x1b[?67;1$y", reply);
	reply_len = 0;
	feed("\x1b[?67l\x1b[?67$p");
	TEST_ASSERT_EQUAL_STRING("\x1b[?67;2$y", reply);
	reply_len = 0;
	feed("\x1b[?7$p");			/* DECAWM: libvterm's own */
	TEST_ASSERT_EQUAL_STRING("\x1b[?7;1$y", reply);
}

static void
test_status_reports(void)
{
	feed("\x1b[3;7H\x1b[?6n");		/* DECXCPR with page */
	TEST_ASSERT_EQUAL_STRING("\x1b[?3;7;1R", reply);
	reply_len = 0;
	feed("\x1b[?15n");
	TEST_ASSERT_EQUAL_STRING("\x1b[?13n", reply);
	reply_len = 0;
	feed("\x1b[?63;9n");
	TEST_ASSERT_EQUAL_STRING("\x1bP9!~0000\x1b\\", reply);
}

static void
test_rep(void)
{
	feed("x\x1b[3b");
	for (unsigned col = 0; col < 4; col++)
		TEST_ASSERT_EQUAL(glyph('x'), cell_at(0, col)->glyph);
	TEST_ASSERT_EQUAL(4, term.screen->cursor_col);
}

static void
test_xtgettcap_and_special_colours(void)
{
	feed("\x1bP+q436F\x1b\\");		/* "Co" */
	TEST_ASSERT_EQUAL_STRING("\x1bP1+r436F=323536\x1b\\", reply); /* 256 */
	reply_len = 0;
	feed("\x1bP+q7a7a\x1b\\");		/* unknown "zz" */
	TEST_ASSERT_EQUAL_STRING("\x1bP0+r7a7a\x1b\\", reply);
	reply_len = 0;
	feed("\x1b]4;256;#f00\x1b\\\x1b]5;0;?\x1b\\");	/* same slot */
	TEST_ASSERT_EQUAL_STRING("\x1b]5;0;rgb:f0f0/0000/0000\x1b\\", reply);
}

static void
test_rectangle_operations(void)
{
	feed("\x1b[88;2;2;3;4$x");		/* DECFRA 'X' rows 2-3, cols 2-4 */
	TEST_ASSERT_EQUAL(glyph('X'), cell_at(1, 1)->glyph);
	TEST_ASSERT_EQUAL(glyph('X'), cell_at(2, 3)->glyph);
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(1, 4)->glyph);
	TEST_ASSERT_EQUAL(0, term.screen->cursor_row);	/* cursor untouched */
	feed("\x1b[2;2;3;4;1;5;6;1$v");		/* DECCRA to row 5, col 6 */
	TEST_ASSERT_EQUAL(glyph('X'), cell_at(4, 5)->glyph);
	TEST_ASSERT_EQUAL(glyph('X'), cell_at(5, 7)->glyph);
	feed("\x1b[2;2;2;4$z");			/* DECERA row 2 */
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(1, 2)->glyph);
	TEST_ASSERT_EQUAL(glyph('X'), cell_at(2, 2)->glyph);
}

static void
test_scosc_and_reverse_wrap(void)
{
	feed("\x1b[5;6H\x1b[s\x1b[1;1H\x1b[u\x1b[6n");	/* SCOSC/SCORC */
	TEST_ASSERT_EQUAL_STRING("\x1b[5;6R", reply);
	reply_len = 0;
	/* Mode 45 backs up only onto a line that wrapped into this one. */
	feed("\x1b[?45h\x1b[3;1H\x08\x1b[6n");
	TEST_ASSERT_EQUAL_STRING("\x1b[3;1R", reply);	/* row 3 didn't wrap */
	reply_len = 0;
	feed("\x1b[10;79Habc\x1b[11;1H\x08\x1b[6n");	/* row 11 did */
	TEST_ASSERT_EQUAL_STRING("\x1b[10;80R", reply);
	/* Mode 1045 always does, and wraps from the top to the bottom. */
	reply_len = 0;
	feed("\x1b[?45l\x1b[?1045h\x1b[3;1H\x08\x1b[6n");
	TEST_ASSERT_EQUAL_STRING("\x1b[2;80R", reply);
	reply_len = 0;
	feed("\x1b[1;1H\x08\x1b[6n");
	TEST_ASSERT_EQUAL_STRING("\x1b[30;80R", reply);
	reply_len = 0;
	feed("\x1b[?1045l\x1b[3;1H\x08\x1b[6n");	/* off: stays */
	TEST_ASSERT_EQUAL_STRING("\x1b[3;1R", reply);
}

static void
test_decsc_saves_protection(void)
{
	feed("\x1b[1\"q\x1b" "7\x1b[0\"q\x1b" "8a");	/* DECSCA, DECSC, 0, DECRC */
	feed("\x1b[1;1;1;1${");			/* DECSERA */
	TEST_ASSERT_EQUAL(glyph('a'), cell_at(0, 0)->glyph);

	/* A soft reset clears the saved protection: DECRC after DECSTR
	 * must not bring it back. */
	feed("\x1b[1\"q\x1b" "7\x1b[!p\x1b" "8b");
	feed("\x1b[1;1;1;1${");
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(0, 0)->glyph);
}

static void
test_decrqss(void)
{
	feed("\x1b[1m\x1bP$qm\x1b\\");
	TEST_ASSERT_EQUAL_STRING("\x1bP1$r0;1m\x1b\\", reply);
	reply_len = 0;
	feed("\x1b[0m\x1bP$qm\x1b\\");
	TEST_ASSERT_EQUAL_STRING("\x1bP1$r0m\x1b\\", reply);
	reply_len = 0;
	/* esctest's between-test reset, after OSC 10/11 changed defaults */
	feed("\x1b]10;rgb:ff/00/00\x1b\\\x1b]11;rgb:00/00/ff\x1b\\");
	feed("\x1b[64;1\"p\x1b[!p\x1b[8;25;80t\x1b[?1047l\x1b[?1048l"
	    "\x1b[?1049l\x1b[?69l\x1b[4l\x1b[20l");
	reply_len = 0;
	feed("\x1b[1m\x1bP$qm\x1b\\");
	TEST_ASSERT_EQUAL_STRING("\x1bP1$r0;1m\x1b\\", reply);
	reply_len = 0;
	feed("\x1bP$q\"p\x1b\\");			/* DECSCL, xterm level */
	TEST_ASSERT_EQUAL_STRING("\x1bP1$r64;1\"p\x1b\\", reply);
	reply_len = 0;
	feed("\x1bP$qt\x1b\\");				/* DECSLPP */
	TEST_ASSERT_EQUAL_STRING("\x1bP1$r30t\x1b\\", reply);
}

/* Start over on a 1280x720 screen (160 x 45 with the default font). */
static void
reinit_720p(void)
{
	term_free(&term);
	render_set_screen(&render, 1280, 720);
	TEST_ASSERT_TRUE(term_init(&term, &font_builtins[0], &render,
	    capture_output, nullptr));
}

static void
test_deccolm(void)
{
	/* 640x480 has no room for 132 columns: like xterm when it can't
	 * resize, the mode is set and the screen cleared, width kept. */
	feed("x\x1b[?3h\x1b[?3$p");
	TEST_ASSERT_EQUAL(80, term.screen->cols);
	TEST_ASSERT_EQUAL_STRING("\x1b[?3;1$y", reply);
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(0, 0)->glyph);	/* cleared */
	feed("\x1b[?3l");

	reinit_720p();
	TEST_ASSERT_EQUAL(160, term.screen->cols);
	feed("junk\x1b[5;5r");
	/* Bytes after ?3h in the same chunk land at the new width. */
	reply_len = 0;
	feed("\x1b[?3h\x1b[1;132HX\x1b[?3$p");
	TEST_ASSERT_EQUAL(132, term.screen->cols);
	TEST_ASSERT_EQUAL(45, term.screen->rows);
	TEST_ASSERT_EQUAL(glyph('X'), cell_at(0, 131)->glyph);
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(0, 0)->glyph);	/* cleared */
	TEST_ASSERT_EQUAL_STRING("\x1b[?3;1$y", reply);
	/* Margins were reset: a newline on the last row scrolls the whole
	 * screen, so row 0's X moves off it. */
	feed("\x1b[45;1H\n");
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(0, 131)->glyph);

	/* Back to normal = the configured (full) width, not 80. */
	feed("\x1b[?3l");
	TEST_ASSERT_EQUAL(160, term.screen->cols);
	TEST_ASSERT_EQUAL(0, term.screen->cursor_row);

	/* DECNCSM keeps the contents across the switch. */
	feed("abc\x1b[?95h\x1b[?3h");
	TEST_ASSERT_EQUAL(132, term.screen->cols);
	TEST_ASSERT_EQUAL(glyph('b'), cell_at(0, 1)->glyph);
	feed("\x1b[?95l");

	/* Mode 40 reset: DECCOLM ignored entirely. */
	feed("\x1b[?40l\x1b[?3l");
	TEST_ASSERT_EQUAL(132, term.screen->cols);
	TEST_ASSERT_EQUAL(glyph('b'), cell_at(0, 1)->glyph);
}

/* --- Sixel ---------------------------------------------------------- */

static size_t
plenty_of_heap(void)
{
	return 1024 * 1024;
}

static void
enable_sixel(void)
{
	term_set_graphics_budget(&term, 64 * 1024, plenty_of_heap);
	TEST_ASSERT_NOT_NULL(term.graphics.pool);
}

/* Palette slot of the pixel at (x, y) inside cell (row, col). */
static unsigned
tile_px(unsigned row, unsigned col, unsigned x, unsigned y)
{
	const struct cell *cell = cell_at(row, col);
	const struct graphics *graphics = &term.graphics;

	TEST_ASSERT_TRUE(cell->attr & CELL_GRAPHIC);
	const uint8_t *line = graphics->pool + cell->glyph *
	    graphics->tile_bytes + y * graphics->tile_row_bytes;
	unsigned bit = x * graphics->bpp;
	return (line[bit / 8] >> (bit % 8)) & graphics->transparent;
}

static void
test_sixel_basic(void)
{
	enable_sixel();
	feed("\x1b[c");				/* DA1 now claims Sixel */
	TEST_ASSERT_EQUAL_STRING("\x1b[?64;4;6;22c", reply);

	/* Two red columns, six pixels tall, at the home cell. */
	feed("\x1bPq#1;2;100;0;0#1~~\x1b\\");
	unsigned red = tile_px(0, 0, 0, 0);
	TEST_ASSERT_EQUAL_HEX16(0xf800, term.graphics.palette[red]);
	TEST_ASSERT_EQUAL(red, tile_px(0, 0, 1, 5));
	TEST_ASSERT_EQUAL(term.graphics.transparent, tile_px(0, 0, 2, 0));
	TEST_ASSERT_EQUAL(term.graphics.transparent, tile_px(0, 0, 0, 6));
	TEST_ASSERT_FALSE(cell_at(0, 1)->attr & CELL_GRAPHIC);
	/* Cursor: left column of the image, on the line below it. */
	TEST_ASSERT_EQUAL(1, term.screen->cursor_row);
	TEST_ASSERT_EQUAL(0, term.screen->cursor_col);

	/* Text over the picture makes the cell text again; its tile is
	 * reclaimed. */
	feed("\x1b[HA");
	TEST_ASSERT_FALSE(cell_at(0, 0)->attr & CELL_GRAPHIC);
	TEST_ASSERT_EQUAL(0, graphics_tiles_in_use(&term));
}

static void
test_sixel_runs_and_bands(void)
{
	enable_sixel();
	/* A 10-pixel run crosses into the second cell; '-' starts a band
	 * six pixels down; HLS 0;50;100 is DEC blue. */
	feed("\x1b[2;3H\x1bP0;1;0q#2;1;0;50;100#2!10~-~\x1b\\");
	unsigned blue = tile_px(1, 2, 0, 0);
	TEST_ASSERT_EQUAL_HEX16(0x001f, term.graphics.palette[blue]);
	TEST_ASSERT_EQUAL(blue, tile_px(1, 3, 1, 0));
	TEST_ASSERT_EQUAL(term.graphics.transparent, tile_px(1, 3, 2, 0));
	TEST_ASSERT_EQUAL(blue, tile_px(1, 2, 0, 11));
	TEST_ASSERT_EQUAL(term.graphics.transparent, tile_px(1, 2, 1, 11));
	TEST_ASSERT_EQUAL(2, term.screen->cursor_row);
	TEST_ASSERT_EQUAL(2, term.screen->cursor_col);
}

static void
test_sixel_scrolls(void)
{
	enable_sixel();
	/* 36 px tall (three cells of 16) from the last row: the screen
	 * scrolls so the image fits, then once more for the cursor. */
	feed("\x1b[30;1H\x1bPq~-~-~-~-~-~\x1b\\");
	TEST_ASSERT_TRUE(cell_at(26, 0)->attr & CELL_GRAPHIC);
	TEST_ASSERT_TRUE(cell_at(28, 0)->attr & CELL_GRAPHIC);
	TEST_ASSERT_FALSE(cell_at(29, 0)->attr & CELL_GRAPHIC);
	TEST_ASSERT_EQUAL(29, term.screen->cursor_row);

	/* DECSDM: top-left of the screen, no scrolling, cursor stays. */
	feed("\x1b[2J\x1b[5;5H\x1b[?80h\x1bPq~\x1b\\");
	TEST_ASSERT_TRUE(cell_at(0, 0)->attr & CELL_GRAPHIC);
	TEST_ASSERT_EQUAL(4, term.screen->cursor_row);
	TEST_ASSERT_EQUAL(4, term.screen->cursor_col);
}

static void
test_sixel_queries_and_off(void)
{
	enable_sixel();
	feed("\x1b[?1;1;0S");
	TEST_ASSERT_EQUAL_STRING("\x1b[?1;0;256S", reply);
	reply_len = 0;
	feed("\x1b[?2;1;0S");
	TEST_ASSERT_EQUAL_STRING("\x1b[?2;0;640;480S", reply);

	/* Sixel switched off: the image is swallowed, nothing drawn. */
	struct term_options options = term.options;
	options.sixel = false;
	TEST_ASSERT_TRUE(term_configure(&term, term.font_source, &options));
	feed("\x1bPq~~\x1b\\ok");
	TEST_ASSERT_FALSE(cell_at(0, 0)->attr & CELL_GRAPHIC);
	TEST_ASSERT_EQUAL(glyph('o'), cell_at(0, 0)->glyph);
}

static void
test_sixel_after_font_switch(void)
{
	enable_sixel();
	feed("\x1bPq~\x1b\\");
	TEST_ASSERT_TRUE(cell_at(0, 0)->attr & CELL_GRAPHIC);
	/* The pool is handed back and re-taken around the rebuild. */
	TEST_ASSERT_TRUE(term_set_font(&term, &font_builtins[1]));
	TEST_ASSERT_NOT_NULL(term.graphics.pool);
	TEST_ASSERT_NOT_EQUAL(0, term.graphics.tiles);
	TEST_ASSERT_EQUAL(term.font.width * term.font.height,
	    term.graphics.tile_bytes);
	feed("\x1bPq~\x1b\\");
	TEST_ASSERT_TRUE(cell_at(0, 0)->attr & CELL_GRAPHIC);
	TEST_ASSERT_EQUAL(1, term.screen->cursor_row);
}

/* --- Media Copy -------------------------------------------------------- */

static char printed[4096];
static size_t printed_len;
static unsigned print_jobs;
static enum term_printer_status printer_state;

static bool
fake_print(const uint8_t *bytes, size_t len, void *ctx)
{
	(void)ctx;
	memcpy(printed + printed_len, bytes, len);
	printed_len += len;
	printed[printed_len] = '\0';
	return true;
}

static void
fake_job_done(void *ctx)
{
	(void)ctx;
	print_jobs++;
}

static enum term_printer_status
fake_status(void *ctx)
{
	(void)ctx;
	return printer_state;
}

static void
attach_printer(void)
{
	static const struct term_printer printer = {
		.write = fake_print, .job_done = fake_job_done,
		.status = fake_status,
	};
	printed_len = 0;
	printed[0] = '\0';
	print_jobs = 0;
	printer_state = TERM_PRINTER_READY;
	term_set_printer(&term, &printer);
}

static void
test_print_screen_and_line(void)
{
	attach_printer();
	feed("hello\r\n\r\nworld  \x1b[i");
	/* 30 rows, trailing blanks dropped, CR LF each. */
	TEST_ASSERT_EQUAL_STRING_LEN("hello\r\n\r\nworld\r\n\r\n", printed,
	    18);
	unsigned lines = 0;
	for (size_t i = 0; i < printed_len; i++)
		lines += printed[i] == '\n';
	TEST_ASSERT_EQUAL(30, lines);
	TEST_ASSERT_EQUAL(1, print_jobs);

	/* Without DECPEX only the scrolling region is printed. */
	printed_len = 0;
	feed("\x1b[1;2r\x1b[0i");
	TEST_ASSERT_EQUAL_STRING("hello\r\n\r\n", printed);

	/* Print cursor line. */
	printed_len = 0;
	feed("\x1b[r\x1b[3;1H\x1b[?1i");
	TEST_ASSERT_EQUAL_STRING("world\r\n", printed);
}

static void
test_auto_print(void)
{
	attach_printer();
	feed("\x1b[?5iabc\r\ndef\r\n\x1b[?4ighi\r\n");
	TEST_ASSERT_EQUAL_STRING("abc\r\ndef\r\n", printed);
	TEST_ASSERT_EQUAL(glyph('g'), cell_at(2, 0)->glyph);	/* still shown */
}

static void
test_printer_controller(void)
{
	attach_printer();
	/* Split across chunks, mid-sequence both ways. */
	feed("x\x1b[");
	feed("5iHEL");
	feed("LO\x1b");
	feed("[4iy");
	TEST_ASSERT_EQUAL_STRING("HELLO", printed);
	TEST_ASSERT_EQUAL(1, print_jobs);
	TEST_ASSERT_EQUAL(glyph('x'), cell_at(0, 0)->glyph);
	TEST_ASSERT_EQUAL(glyph('y'), cell_at(0, 1)->glyph);

	/* Look-alikes go where they belong: ESC [ 5 m is blink. */
	printed_len = 0;
	printed[0] = '\0';
	feed("\x1b[5mZ\x1b[0m");
	TEST_ASSERT_EQUAL(0, printed_len);
	TEST_ASSERT_EQUAL(glyph('Z'), cell_at(0, 2)->glyph);
	TEST_ASSERT_TRUE(cell_at(0, 2)->attr & CELL_BLINK);
	/* ...and inside controller mode, ESC [ 5 m is printer data. */
	feed("\x1b[5i\x1b[5m\x1b[4i");
	TEST_ASSERT_EQUAL_STRING("\x1b[5m", printed);
}

static void
test_printer_status(void)
{
	feed("\x1b[?15n");
	TEST_ASSERT_EQUAL_STRING("\x1b[?13n", reply);	/* no printer */
	attach_printer();
	reply_len = 0;
	feed("\x1b[?15n");
	TEST_ASSERT_EQUAL_STRING("\x1b[?10n", reply);
	printer_state = TERM_PRINTER_NOT_READY;
	reply_len = 0;
	feed("\x1b[?15n");
	TEST_ASSERT_EQUAL_STRING("\x1b[?11n", reply);
}

/* Sixel at fewer bits a pixel: packed tiles, a smaller palette. */
static void
use_sixel_depth(unsigned bpp)
{
	struct term_options options = term.options;
	options.sixel_bpp = (uint8_t)bpp;
	TEST_ASSERT_TRUE(term_configure(&term, term.font_source, &options));
	enable_sixel();
	TEST_ASSERT_EQUAL(bpp, term.graphics.bpp);
}

/* Count each palette slot over the cell at (row, col). */
static void
count_slots(unsigned row, unsigned col, unsigned counts[256])
{
	memset(counts, 0, 256 * sizeof(unsigned));
	for (unsigned y = 0; y < term.graphics.tile_height; y++)
		for (unsigned x = 0; x < term.graphics.tile_width; x++)
			counts[tile_px(row, col, x, y)]++;
}

static void
test_sixel_depths(void)
{
	unsigned counts[256];

	/* 4 bits: the fixed CGA palette, dithered. */
	use_sixel_depth(4);
	TEST_ASSERT_EQUAL(4, term.graphics.tile_row_bytes);	/* 8 px */
	TEST_ASSERT_EQUAL(15, term.graphics.palette_used);
	TEST_ASSERT_EQUAL_HEX16(0xa800, term.graphics.palette[4]); /* red */
	/* A 40% blue 8x12 block (two bands), unpainted below: between
	 * black and CGA blue (0x0000aa), so a mix of the two. */
	feed("\x1b[H\x1bPq#1;2;0;0;40#1!8~-#1!8~\x1b\\");
	count_slots(0, 0, counts);
	TEST_ASSERT_EQUAL(8 * 12, counts[0] + counts[1]);
	TEST_ASSERT_TRUE(counts[0] > 8 && counts[1] > 8);	/* dithered */
	TEST_ASSERT_EQUAL(8 * 4, counts[15]);	/* rows 12-15 untouched */

	/* 2 bits: black, grey, white. A 25% grey is mostly a mix of black
	 * and grey. */
	use_sixel_depth(2);
	TEST_ASSERT_EQUAL(3, term.graphics.palette_used);
	feed("\x1b[H\x1bPq#1;2;25;25;25#1!8~-#1!8~-#1!8~\x1b\\");
	count_slots(0, 0, counts);
	TEST_ASSERT_TRUE(counts[0] > 16 && counts[1] > 16);
	TEST_ASSERT_EQUAL(0, counts[2]);			/* no white */

	/* The renderer unpacks 2-bit rows: white, transparent, white. */
	feed("\x1b[2J\x1b[2;1H\x1bPq#1;2;100;100;100#1~?~\x1b\\");
	uint16_t line[640];
	render_scanline(line, render.y_offset + term.font.height, &render);
	TEST_ASSERT_EQUAL_HEX16(0xffff, line[render.x_offset]);
	TEST_ASSERT_EQUAL_HEX16(cell_at(1, 0)->bg, line[render.x_offset + 1]);
	TEST_ASSERT_EQUAL_HEX16(0xffff, line[render.x_offset + 2]);

	/* 1 bit: ink (the foreground) or nothing. A 50% grey is about half
	 * ink, half see-through. */
	use_sixel_depth(1);
	TEST_ASSERT_EQUAL(1, term.graphics.palette_used);
	feed("\x1b[H\x1bPq#1;2;50;50;50#1!8~-#1!8~-#1!8~\x1b\\");
	count_slots(0, 0, counts);
	TEST_ASSERT_TRUE(counts[0] > 32 && counts[0] < 96);	/* ink */
}

static void
test_decbi_decfi(void)
{
	feed("\x1b[1;5H\x1b" "6");			/* DECBI: just moves */
	TEST_ASSERT_EQUAL(3, term.screen->cursor_col);
	feed("\x1b" "9\x1b" "9");			/* DECFI twice */
	TEST_ASSERT_EQUAL(5, term.screen->cursor_col);
	feed("\x1b[1;1H\x1b" "6");			/* left edge: nothing */
	TEST_ASSERT_EQUAL(0, term.screen->cursor_col);

	/* At the left margin the region scrolls right (esctest's case). */
	feed("\x1b[3;2Habcde\x1b[4;2Hfghij\x1b[5;2Hklmno"
	    "\x1b[?69h\x1b[3;5s\x1b[4;6r\x1b[5;3H\x1b" "6");
	TEST_ASSERT_EQUAL(glyph('f'), cell_at(3, 1)->glyph);
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(3, 2)->glyph);
	TEST_ASSERT_EQUAL(glyph('g'), cell_at(3, 3)->glyph);
	TEST_ASSERT_EQUAL(glyph('h'), cell_at(3, 4)->glyph);
	TEST_ASSERT_EQUAL(glyph('j'), cell_at(3, 5)->glyph);
	TEST_ASSERT_EQUAL(glyph('c'), cell_at(2, 3)->glyph);	/* above: kept */
	/* ...and DECFI at the right margin scrolls it back left. */
	feed("\x1b[5;5H\x1b" "9");
	TEST_ASSERT_EQUAL(glyph('g'), cell_at(3, 2)->glyph);
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(3, 4)->glyph);
}

static void
test_window_ops(void)
{
	feed("\x1b]0;both\x07\x1b]2;window\x07");
	feed("\x1b[21t");
	TEST_ASSERT_EQUAL_STRING("\x1b]lwindow\x1b\\", reply);
	reply_len = 0;
	feed("\x1b[20t");
	TEST_ASSERT_EQUAL_STRING("\x1b]Lboth\x1b\\", reply);

	/* Push both, change both, pop the icon only. */
	feed("\x1b[22;0t\x1b]1;x\x07\x1b]2;x\x07\x1b[23;1t");
	reply_len = 0;
	feed("\x1b[20t\x1b[21t");
	TEST_ASSERT_EQUAL_STRING("\x1b]Lboth\x1b\\\x1b]lx\x1b\\", reply);
	/* That pop used the entry up: popping the title does nothing. */
	reply_len = 0;
	feed("\x1b[23;2t\x1b[21t");
	TEST_ASSERT_EQUAL_STRING("\x1b]lx\x1b\\", reply);
	/* DECID answers as DA1. */
	reply_len = 0;
	feed("\x1bZ");
	TEST_ASSERT_EQUAL_STRING("\x1b[?64;6;22c", reply);

	/* Iconify and move are remembered for their reports. */
	reply_len = 0;
	feed("\x1b[2t\x1b[11t\x1b[1t\x1b[11t\x1b[3;12;34t\x1b[13t");
	TEST_ASSERT_EQUAL_STRING("\x1b[2t\x1b[1t\x1b[3;12;34t", reply);
}

static void
test_saved_cursor_per_screen(void)
{
	/* Save on the main screen, save elsewhere on the alternate,
	 * restore on main: the main screen's save. */
	feed("\x1b[3;2H\x1b" "7\x1b[?1047h\x1b[7;6H\x1b" "7\x1b[?1047l"
	    "\x1b[1;1H\x1b" "8");
	TEST_ASSERT_EQUAL(2, term.screen->cursor_row);
	TEST_ASSERT_EQUAL(1, term.screen->cursor_col);
	/* The same with xterm's original mode 47. */
	feed("\x1b[3;2H\x1b" "7\x1b[?47h\x1b[7;6H\x1b" "7\x1b[?47l"
	    "\x1b[1;1H\x1b" "8");
	TEST_ASSERT_EQUAL(2, term.screen->cursor_row);
	/* 1049 still brings the cursor back. */
	feed("\x1b[5;5H\x1b[?1049h\x1b[10;10H\x1b[?1049l");
	TEST_ASSERT_EQUAL(4, term.screen->cursor_row);
	TEST_ASSERT_EQUAL(4, term.screen->cursor_col);
}

static void
test_index_outside_lr_margins(void)
{
	feed("\x1b[2;5r\x1b[?69h\x1b[2;5s\x1b[5;3Hx");
	feed("\x1b[5;6H\x1b" "D");		/* IND right of the margins */
	TEST_ASSERT_EQUAL(4, term.screen->cursor_row);
	TEST_ASSERT_EQUAL(glyph('x'), cell_at(4, 2)->glyph);	/* no scroll */
	feed("\x1b[2;6H\x1b" "M");		/* RI at the top margin */
	TEST_ASSERT_EQUAL(1, term.screen->cursor_row);
	TEST_ASSERT_EQUAL(glyph('x'), cell_at(4, 2)->glyph);
	feed("\x1b[5;3H\x1b" "D");		/* inside: scrolls */
	TEST_ASSERT_EQUAL(glyph('x'), cell_at(3, 2)->glyph);
}

static void
test_text_lr_margins(void)
{
	/* Margins 5..10: printing wraps at 10 to column 5 of the next line. */
	feed("\x1b[?69h\x1b[5;10s\x1b[1;9Habcd");
	TEST_ASSERT_EQUAL(glyph('a'), cell_at(0, 8)->glyph);
	TEST_ASSERT_EQUAL(glyph('b'), cell_at(0, 9)->glyph);
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(0, 10)->glyph);
	TEST_ASSERT_EQUAL(glyph('c'), cell_at(1, 4)->glyph);
	TEST_ASSERT_EQUAL(glyph('d'), cell_at(1, 5)->glyph);
	/* Autowrap off: stuck at the right margin, last char wins. */
	feed("\x1b[?7l\x1b[3;9Hxyz");
	TEST_ASSERT_EQUAL(glyph('z'), cell_at(2, 9)->glyph);
	TEST_ASSERT_EQUAL(glyph(' '), cell_at(2, 10)->glyph);
	feed("\x1b[?7h");
}

static void
test_decscl_levels(void)
{
	/* Level 3 (VT320): no left/right margins. */
	feed("\x1b[63;1\"p\x1b[?69h\x1b[5;6s\x1b[1;5Habc");
	TEST_ASSERT_EQUAL(7, term.screen->cursor_col);
	/* Level 2 (VT220): DECRQM isn't answered at all. */
	reply_len = 0;
	reply[0] = '\0';
	feed("\x1b[62;1\"p\x1b[?7$p\x1b[4$p");
	TEST_ASSERT_EQUAL(0, reply_len);
	/* DECRQSS reports the level, capped at the emulation's (64). */
	feed("\x1b[65;1\"p\x1bP$q\"p\x1b\\");
	TEST_ASSERT_EQUAL_STRING("\x1bP1$r64;1\"p\x1b\\", reply);
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
	RUN_TEST(test_colour_module);
	RUN_TEST(test_device_attributes_per_level);
	RUN_TEST(test_colour_depths);
	RUN_TEST(test_utf8_off_and_alt_screen_off);
	RUN_TEST(test_decrqcra_checksums);
	RUN_TEST(test_selective_erase_keeps_protected);
	RUN_TEST(test_double_width_and_height_lines);
	RUN_TEST(test_osc_colours);
	RUN_TEST(test_mode_reports);
	RUN_TEST(test_status_reports);
	RUN_TEST(test_rep);
	RUN_TEST(test_xtgettcap_and_special_colours);
	RUN_TEST(test_rectangle_operations);
	RUN_TEST(test_scosc_and_reverse_wrap);
	RUN_TEST(test_decsc_saves_protection);
	RUN_TEST(test_decrqss);
	RUN_TEST(test_deccolm);
	RUN_TEST(test_sixel_basic);
	RUN_TEST(test_sixel_runs_and_bands);
	RUN_TEST(test_sixel_scrolls);
	RUN_TEST(test_sixel_queries_and_off);
	RUN_TEST(test_sixel_after_font_switch);
	RUN_TEST(test_sixel_depths);
	RUN_TEST(test_decbi_decfi);
	RUN_TEST(test_window_ops);
	RUN_TEST(test_saved_cursor_per_screen);
	RUN_TEST(test_index_outside_lr_margins);
	RUN_TEST(test_text_lr_margins);
	RUN_TEST(test_decscl_levels);
	RUN_TEST(test_print_screen_and_line);
	RUN_TEST(test_auto_print);
	RUN_TEST(test_printer_controller);
	RUN_TEST(test_printer_status);
	return UNITY_END();
}

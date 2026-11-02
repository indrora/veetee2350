/*
 * test_setup — settings record, SET-UP navigation, and applying options.
 */
#include <string.h>

#include <unity.h>

#include "font.h"
#include "grid.h"
#include "keys.h"
#include "render.h"
#include "settings.h"
#include "setup.h"
#include "term.h"
#include "video_modes.h"

static struct font font;
static struct grid grid;
static struct setup setup;
static struct settings settings;

void
setUp(void)
{
	TEST_ASSERT_EQUAL(FONT_OK, font_load_psf2(&font, font_builtins[0].data,
	    font_builtins[0].len));
	TEST_ASSERT_TRUE(grid_init(&grid, 80, 30, grid_make_cell(0, 0, 0, 0)));
	settings_defaults(&settings);
	setup_begin(&setup, &settings, &grid, &font, VIDEO_MODE_640x480);
}

void
tearDown(void)
{
	grid_free(&grid);
	font_free(&font);
}

static enum setup_action
press(enum key_code code, uint8_t byte)
{
	struct key_event key = {.code = code, .byte = byte};

	return setup_key(&setup, &key);
}

static void
test_settings_crc(void)
{
	TEST_ASSERT_TRUE(settings_valid(&settings));
	struct settings erased;
	memset(&erased, 0xff, sizeof(erased));
	TEST_ASSERT_FALSE(settings_valid(&erased));
	settings.autowrap = !settings.autowrap;	/* changed, not resealed */
	TEST_ASSERT_FALSE(settings_valid(&settings));
	settings_seal(&settings);
	TEST_ASSERT_TRUE(settings_valid(&settings));
}

static void
test_navigation_and_values(void)
{
	/* Starts on Resolution; Right picks the next mode. */
	TEST_ASSERT_EQUAL(ITEM_RESOLUTION, setup.selected);
	press(KEY_RIGHT, 0);
	TEST_ASSERT_EQUAL(VIDEO_MODE_720x480, setup.work.resolution);
	press(KEY_LEFT, 0);
	press(KEY_LEFT, 0);	/* wraps to the last mode */
	TEST_ASSERT_EQUAL(VIDEO_MODE_COUNT - 1, setup.work.resolution);
	/* Font: Right steps to the next built-in font. */
	press(KEY_DOWN, 0);
	TEST_ASSERT_EQUAL(SETUP_NONE, press(KEY_RIGHT, 0));
	TEST_ASSERT_EQUAL_STRING(font_builtins[1].name, setup.work.font);
	/* Down to Colours, Left wraps around to the last scheme. */
	press(KEY_DOWN, 0);
	press(KEY_DOWN, 0);
	TEST_ASSERT_EQUAL(ITEM_SCHEME, setup.selected);
	press(KEY_LEFT, 0);
	TEST_ASSERT_EQUAL(SCHEME_COUNT - 1, setup.work.scheme);
	/* Up from the top wraps to Exit. */
	setup.selected = 0;
	press(KEY_UP, 0);
	TEST_ASSERT_EQUAL(ITEM_EXIT, setup.selected);
	TEST_ASSERT_EQUAL(SETUP_EXIT, press(KEY_ENTER, 0));
	/* The original settings are untouched until the app applies. */
	TEST_ASSERT_EQUAL_STRING("notjam-clean-16", settings.font);
}

static void
test_toggles_and_actions(void)
{
	setup.selected = ITEM_WRAP;
	press(KEY_CHAR, ' ');
	TEST_ASSERT_FALSE(setup.work.autowrap);
	TEST_ASSERT_EQUAL(SETUP_SAVE, press(KEY_CHAR, 's'));
	TEST_ASSERT_EQUAL(SETUP_EXIT, press(KEY_F3, 0));
	TEST_ASSERT_EQUAL(SETUP_EXIT, press(KEY_ESCAPE, 0));
}

static void
test_answerback_editing(void)
{
	setup.selected = ITEM_ANSWERBACK;
	press(KEY_ENTER, 0);
	TEST_ASSERT_TRUE(setup.editing);
	for (size_t i = strlen(setup.work.answerback); i > 0; i--)
		press(KEY_BACKSPACE, 0);
	press(KEY_CHAR, 'h');
	press(KEY_CHAR, 'i');
	/* While editing, 's' is text, not "save". */
	TEST_ASSERT_EQUAL(SETUP_NONE, press(KEY_CHAR, 's'));
	press(KEY_ENTER, 0);
	TEST_ASSERT_FALSE(setup.editing);
	TEST_ASSERT_EQUAL_STRING("his", setup.work.answerback);
}

static void
test_size_resolution(void)
{
	const struct font_builtin *clean16 = font_builtin_find("notjam-clean-16");
	const struct font_builtin *prophet8 =
	    font_builtin_find("notjam-prophet-8");
	unsigned cols, rows;

	TEST_ASSERT_FALSE(setup_grid_for(prophet8, SIZE_FULL, 640, 480, &cols,
	    &rows));
	TEST_ASSERT_EQUAL(80, cols);
	TEST_ASSERT_EQUAL(60, rows);
	TEST_ASSERT_FALSE(setup_grid_for(prophet8, SIZE_80x24, 640, 480, &cols,
	    &rows));
	TEST_ASSERT_EQUAL(24, rows);
	/* 720 lines / 8 = 90 rows, capped at what the grid supports. */
	setup_grid_for(prophet8, SIZE_FULL, 1280, 720, &cols, &rows);
	TEST_ASSERT_EQUAL(GRID_MAX_ROWS, rows);
	/* 132 columns don't fit 640 px of 8-px cells: clamped to 80. */
	TEST_ASSERT_TRUE(setup_grid_for(clean16, SIZE_132x24, 640, 480, &cols,
	    &rows));
	TEST_ASSERT_EQUAL(80, cols);
	TEST_ASSERT_EQUAL(24, rows);
	/* ...but they do at 1280x720. */
	TEST_ASSERT_FALSE(setup_grid_for(clean16, SIZE_132x43, 1280, 720, &cols,
	    &rows));
	TEST_ASSERT_EQUAL(132, cols);
	TEST_ASSERT_EQUAL(43, rows);
}

/* --- applying options to the terminal -------------------------------- */

static char reply[64];
static size_t reply_len;

static void
capture(const char *bytes, size_t len, void *ctx)
{
	(void)ctx;
	if (reply_len + len < sizeof(reply)) {
		memcpy(reply + reply_len, bytes, len);
		reply_len += len;
		reply[reply_len] = '\0';
	}
}

static void
test_term_configure(void)
{
	struct render render;
	struct term term;
	struct term_options options = {
		.fg_rgb = {255, 176, 0},
		.bg_rgb = {20, 10, 0},
		.max_rows = 24,
		.max_cols = 64,
		.cursor_shape = CURSOR_BAR,
		.cursor_blink = false,
		.autowrap = true,
		.answerback = "hello",
	};

	render_set_screen(&render, 640, 480);
	TEST_ASSERT_TRUE(term_init(&term, &font_builtins[0], &render, capture,
	    nullptr));
	TEST_ASSERT_TRUE(term_configure(&term, &font_builtins[0], &options));
	TEST_ASSERT_EQUAL(24, term.screen->rows);	/* row limit */
	TEST_ASSERT_EQUAL(64, term.screen->cols);	/* column limit */
	/* 64 columns of 8 px centred in 640: 64 px border each side. */
	TEST_ASSERT_EQUAL(64, render.x_offset);
	TEST_ASSERT_EQUAL(CURSOR_BAR, term.screen->cursor_shape);
	TEST_ASSERT_FALSE(term.screen->cursor_blink);
	TEST_ASSERT_EQUAL_HEX16(grid_rgb565(20, 10, 0),
	    (uint16_t)grid_row(term.screen, 0)[0].bg2);
	/* 24 rows of 16 px = 384 lines, centred: 48 border lines on top. */
	TEST_ASSERT_EQUAL(48, render.y_offset);

	reply_len = 0;
	term_feed(&term, (const uint8_t *)"\x05", 1);	/* ENQ */
	TEST_ASSERT_EQUAL_STRING("hello", reply);
	term_free(&term);
}

int
main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_settings_crc);
	RUN_TEST(test_navigation_and_values);
	RUN_TEST(test_toggles_and_actions);
	RUN_TEST(test_answerback_editing);
	RUN_TEST(test_size_resolution);
	RUN_TEST(test_term_configure);
	return UNITY_END();
}

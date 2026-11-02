/*
 * test_setup — settings record, SET-UP navigation, and applying options.
 */
#include <stdio.h>
#include <string.h>

#include <unity.h>

#include "base16.h"
#include "font.h"
#include "grid.h"
#include "keys.h"
#include "menu.h"
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
	/* Colour scheme: Left wraps around to the last scheme. */
	setup.selected = ITEM_SCHEME;
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
test_emulation_presets(void)
{
	setup.selected = ITEM_EMULATION;
	TEST_ASSERT_EQUAL(EMU_XTERM, setup.work.emulation);
	press(KEY_RIGHT, 0);			/* wraps to VT100 */
	TEST_ASSERT_EQUAL(EMU_VT100, setup.work.emulation);
	TEST_ASSERT_EQUAL(DEPTH_MONO, setup.work.colour_depth);
	TEST_ASSERT_FALSE(setup.work.utf8);
	TEST_ASSERT_FALSE(setup.work.alt_screen);
	/* Toggles can still be overridden after choosing a level. */
	setup.selected = ITEM_DEPTH;
	press(KEY_RIGHT, 0);
	TEST_ASSERT_EQUAL(DEPTH_16, setup.work.colour_depth);
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
	    grid_row(term.screen, 0)[0].bg);
	/* 24 rows of 16 px = 384 lines, centred: 48 border lines on top. */
	TEST_ASSERT_EQUAL(48, render.y_offset);

	reply_len = 0;
	term_feed(&term, (const uint8_t *)"\x05", 1);	/* ENQ */
	TEST_ASSERT_EQUAL_STRING("hello", reply);
	term_free(&term);
}

static void
test_base16(void)
{
	/* tinted-theming layout: nested palette, '#', quotes, comments. */
	static const char current[] =
	    "system: \"base16\"\nname: \"3024\"\nauthor: \"x\" # who\n"
	    "variant: \"dark\"\npalette:\n"
	    "  base00: \"#090300\"\n  base01: \"#3a3432\"\n"
	    "  base02: \"#4a4543\"\n  base03: \"#5c5855\"\n"
	    "  base04: \"#807d7c\"\n  base05: \"#a5a2a2\"\n"
	    "  base06: \"#d6d5d4\"\n  base07: \"#f7f7f7\"\n"
	    "  base08: \"#db2d20\"\n  base09: \"#e8bbd0\"\n"
	    "  base0A: \"#fded02\"\n  base0B: \"#01a252\"\n"
	    "  base0C: \"#b5e4f4\"\n  base0D: \"#01a0e4\"\n"
	    "  base0E: \"#a16a94\"   # purple\n  base0F: \"#cdab53\"\n";
	struct base16 scheme;
	struct base16_terminal colours;
	const char *error = nullptr;

	TEST_ASSERT_TRUE(base16_parse(current, sizeof(current) - 1, &scheme,
	    &error));
	TEST_ASSERT_EQUAL_STRING("3024", scheme.name);
	base16_to_terminal(&scheme, &colours);
	TEST_ASSERT_EQUAL_HEX8(0xdb, colours.ansi[1][0]);	/* red = 08 */
	TEST_ASSERT_EQUAL_HEX8(0x01, colours.ansi[4][0]);	/* blue = 0D */
	TEST_ASSERT_EQUAL_HEX8(0x5c, colours.ansi[8][0]);	/* = 03 */
	TEST_ASSERT_EQUAL_HEX8(0xf7, colours.ansi[15][0]);	/* = 07 */
	TEST_ASSERT_EQUAL_HEX8(0xa5, colours.fg[0]);
	TEST_ASSERT_EQUAL_HEX8(0x09, colours.bg[0]);

	/* Older layout: flat keys, bare hex, CRLF. */
	static const char older[] =
	    "scheme: \"Old\"\r\nbase00: \"000000\"\r\nbase01: 111111\r\n"
	    "base02: '222222'\r\nbase03: \"333333\"\r\nbase04: 444444\r\n"
	    "base05: 555555\r\nbase06: 666666\r\nbase07: 777777\r\n"
	    "base08: 888888\r\nbase09: 999999\r\nbase0a: aaaaaa\r\n"
	    "base0B: bbbbbb\r\nbase0C: cccccc\r\nbase0D: dddddd\r\n"
	    "base0E: eeeeee\r\nbase0F: ffffff\r\n";
	TEST_ASSERT_TRUE(base16_parse(older, sizeof(older) - 1, &scheme,
	    &error));
	TEST_ASSERT_EQUAL_STRING("Old", scheme.name);
	TEST_ASSERT_EQUAL_HEX8(0xaa, scheme.base[10][0]);

	/* Errors name the culprit. */
	TEST_ASSERT_FALSE(base16_parse("base00: 000000\n", 15, &scheme,
	    &error));
	TEST_ASSERT_EQUAL_STRING("base01 missing", error);
	TEST_ASSERT_FALSE(base16_parse("base00: zz0000\n", 15, &scheme,
	    &error));
	TEST_ASSERT_EQUAL_STRING("base00: not a colour", error);
	TEST_ASSERT_FALSE(base16_parse("hello\n", 6, &scheme, &error));
	TEST_ASSERT_EQUAL_STRING("no base16 colours in file", error);
}

static void
test_card_lists(void)
{
	static const struct setup_card_font fonts[] = {
		{"Bm437_IBM_VGA_8x16", 8, 16},
	};
	static const char schemes[][SETTINGS_SCHEME_MAX] = {"dracula",
	    "nord"};
	struct setup_catalogue card = {
		.font_count = 1, .fonts = fonts,
		.scheme_count = 2, .schemes = schemes,
	};

	setup_catalogue(&setup, &card);
	/* Font: Left from the first built-in wraps to the card font. */
	while (setup.selected != ITEM_FONT)
		press(KEY_DOWN, 0);
	snprintf(setup.work.font, sizeof(setup.work.font), "%s",
	    font_builtins[0].name);
	press(KEY_LEFT, 0);
	TEST_ASSERT_EQUAL_STRING("sd:Bm437_IBM_VGA_8x16", setup.work.font);
	press(KEY_RIGHT, 0);
	TEST_ASSERT_EQUAL_STRING(font_builtins[0].name, setup.work.font);

	/* Scheme: after the built-ins come the card's. */
	while (setup.selected != ITEM_SCHEME)
		press(KEY_DOWN, 0);
	for (unsigned i = 0; i < SCHEME_COUNT; i++)
		press(KEY_RIGHT, 0);
	TEST_ASSERT_EQUAL_STRING("dracula", setup.work.scheme_file);
	press(KEY_RIGHT, 0);
	TEST_ASSERT_EQUAL_STRING("nord", setup.work.scheme_file);
	press(KEY_RIGHT, 0);	/* wraps to the first built-in */
	TEST_ASSERT_EQUAL_STRING("", setup.work.scheme_file);
	TEST_ASSERT_EQUAL(0, setup.work.scheme);

	/* A card font that isn't on this card still shows, marked. */
	snprintf(setup.work.font, sizeof(setup.work.font), "sd:gone");
	setup_draw(&setup);
	press(KEY_RIGHT, 0);	/* from "missing": back to the first */
}

static struct menu menu;

static int
menu_press(enum key_code code, uint8_t mods)
{
	struct key_event key = {.code = code, .mods = mods};

	return menu_key(&menu, &key);
}

static void
test_tools_menu(void)
{
	bool available[TOOL_COUNT] = {false};
	const char *state[TOOL_COUNT] = {nullptr};

	settings_defaults(&settings);
	available[TOOL_MENU] = true;		/* never listed */
	available[TOOL_SETUP] = true;
	available[TOOL_PRINTER] = true;
	available[TOOL_HARD_RESET] = true;
	state[TOOL_PRINTER] = "off";
	menu_open(&menu, &font, 0xffff, 0x0000, 80, 30, available, state,
	    settings.hotkeys);
	TEST_ASSERT_EQUAL(3, menu.count);
	TEST_ASSERT_EQUAL(3 + 3, menu.overlay.rows);
	TEST_ASSERT_EQUAL((80 - MENU_COLS) / 2, menu.overlay.col);

	/* Up from the top wraps to Hard reset; Enter picks it. */
	TEST_ASSERT_EQUAL(MENU_STAY, menu_press(KEY_UP, 0));
	TEST_ASSERT_EQUAL(TOOL_HARD_RESET, menu_press(KEY_ENTER, 0));
	TEST_ASSERT_EQUAL(MENU_STAY, menu_press(KEY_DOWN, 0));
	TEST_ASSERT_EQUAL(TOOL_SETUP, menu_press(KEY_ENTER, 0));
	/* Esc, or the menu's own hotkey (Ctrl+F3), close it; F3 alone
	 * doesn't. */
	TEST_ASSERT_EQUAL(MENU_STAY, menu_press(KEY_F3, 0));
	TEST_ASSERT_EQUAL(MENU_CLOSE, menu_press(KEY_F3, KEY_MOD_CTRL));
	TEST_ASSERT_EQUAL(MENU_CLOSE, menu_press(KEY_ESCAPE, 0));

	/* The selected row is drawn in the theme's own colours (fg white),
	 * the others swapped (fg black). */
	const struct cell *first = &menu.cells[2 * MENU_COLS + 2];
	TEST_ASSERT_EQUAL_HEX16(0xffff, first->fg);
	TEST_ASSERT_EQUAL_HEX16(0x0000, menu.cells[3 * MENU_COLS + 2].fg);
}

static void
test_hotkey_matching(void)
{
	struct key_event ctrl_f3 = {.code = KEY_F3, .mods = KEY_MOD_CTRL};
	struct key_event f3 = {.code = KEY_F3};
	struct key_event ctrl_bracket = {.code = KEY_CHAR, .byte = 0x1d};
	struct hotkey bracket = {KEY_CHAR, 0x1d, 0};
	struct hotkey none = {KEY_NONE, 0, 0};

	settings_defaults(&settings);
	TEST_ASSERT_TRUE(hotkey_matches(&settings.hotkeys[TOOL_MENU],
	    &ctrl_f3));
	TEST_ASSERT_FALSE(hotkey_matches(&settings.hotkeys[TOOL_MENU], &f3));
	TEST_ASSERT_TRUE(hotkey_matches(&bracket, &ctrl_bracket));
	TEST_ASSERT_FALSE(hotkey_matches(&none, &f3));
}

int
main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_tools_menu);
	RUN_TEST(test_hotkey_matching);
	RUN_TEST(test_settings_crc);
	RUN_TEST(test_navigation_and_values);
	RUN_TEST(test_toggles_and_actions);
	RUN_TEST(test_emulation_presets);
	RUN_TEST(test_answerback_editing);
	RUN_TEST(test_size_resolution);
	RUN_TEST(test_term_configure);
	RUN_TEST(test_base16);
	RUN_TEST(test_card_lists);
	return UNITY_END();
}

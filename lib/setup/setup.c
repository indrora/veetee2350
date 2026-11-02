/*
 * setup.c — SET-UP screen: drawing and key handling.
 *
 * Drawing is plain ASCII on purpose: SET-UP must be readable in every
 * built-in font, and some (Not Jam Casual 10) have nothing beyond ASCII.
 */
#include "setup.h"

#include "menu.h"

#include <stdio.h>
#include <string.h>

#include "render.h"
#include "term.h"
#include "video_modes.h"

/* Menu colours: VT-style light text on blue, selection inverted. */
#define MENU_FG		grid_rgb565(230, 230, 230)
#define MENU_BG		grid_rgb565(16, 40, 120)
#define TITLE_FG	grid_rgb565(16, 40, 120)
#define TITLE_BG	grid_rgb565(255, 210, 0)
#define DIM_FG		grid_rgb565(150, 170, 220)

#define LABEL_COL	3
#define VALUE_COL	22
#define FIRST_ROW	2

static const char *const cursor_names[] = {"block", "underline", "bar"};

bool
setup_grid_for(const struct font_builtin *builtin, unsigned size,
    unsigned width, unsigned height, unsigned *cols, unsigned *rows)
{
	unsigned cell_w, cell_h;

	font_builtin_cell(builtin, &cell_w, &cell_h);
	return setup_grid_for_cell(cell_w, cell_h, size, width, height, cols,
	    rows);
}

bool
setup_grid_for_cell(unsigned cell_w, unsigned cell_h, unsigned size,
    unsigned width, unsigned height, unsigned *cols, unsigned *rows)
{
	const struct settings_size *want = &settings_sizes[size % SIZE_COUNT];

	*cols = width / cell_w;
	*rows = height / cell_h;
	if (*cols > GRID_MAX_COLS)
		*cols = GRID_MAX_COLS;
	if (*rows > GRID_MAX_ROWS)
		*rows = GRID_MAX_ROWS;
	if (want->cols == 0)
		return false;
	bool clamped = want->cols > *cols || want->rows > *rows;
	if (want->cols < *cols)
		*cols = want->cols;
	if (want->rows < *rows)
		*rows = want->rows;
	return clamped;
}

/*
 * The font being edited, as an index into built-ins followed by card
 * fonts; -1 for a card font that isn't on this card (or no card).
 */
static int
work_font_index(const struct setup *setup)
{
	const char *name = setup->work.font;
	const struct font_builtin *builtin = font_builtin_find(name);
	size_t prefix = strlen(SETTINGS_CARD_PREFIX);

	if (builtin != nullptr)
		return (int)(builtin - font_builtins);
	if (strncmp(name, SETTINGS_CARD_PREFIX, prefix) != 0)
		return 0;	/* unknown built-in: the default is used */
	for (unsigned i = 0; i < setup->card.font_count; i++)
		if (strcmp(setup->card.fonts[i].name, name + prefix) == 0)
			return (int)(font_builtin_count + i);
	return -1;
}

/* Cell size of the font being edited (the default's if unavailable). */
static void
work_cell(const struct setup *setup, unsigned *width, unsigned *height)
{
	int index = work_font_index(setup);

	if (index >= (int)font_builtin_count) {
		const struct setup_card_font *card =
		    &setup->card.fonts[index - (int)font_builtin_count];
		*width = card->width;
		*height = card->height;
	} else {
		font_builtin_cell(&font_builtins[index < 0 ? 0 : index], width,
		    height);
	}
}

/* Index into built-in schemes followed by card schemes; -1: missing. */
static int
work_scheme_index(const struct setup *setup)
{
	const char *file = setup->work.scheme_file;

	if (file[0] == '\0')
		return setup->work.scheme % SCHEME_COUNT;
	for (unsigned i = 0; i < setup->card.scheme_count; i++)
		if (strcmp(setup->card.schemes[i], file) == 0)
			return (int)(SCHEME_COUNT + i);
	return -1;
}

/* --- drawing ---------------------------------------------------------- */

static void
put(struct setup *setup, unsigned row, unsigned col, const char *text,
    uint16_t fg, uint16_t bg)
{
	struct grid *grid = setup->grid;

	if (row >= grid->rows)
		return;
	struct cell *cells = grid_row(grid, row);
	for (; *text != '\0' && col < grid->cols; text++, col++)
		cells[col] = grid_make_cell(font_glyph_for(setup->font,
		    (uint8_t)*text), fg, bg, 0);
}

static void
fill_row(struct setup *setup, unsigned row, uint16_t fg, uint16_t bg)
{
	grid_clear_span(setup->grid, row, 0, setup->grid->cols,
	    grid_make_cell(font_glyph_for(setup->font, ' '), fg, bg, 0));
}

/* The text shown for an item's value. */
static void
format_value(const struct setup *setup, unsigned item, char *out, size_t len)
{
	const struct settings *work = &setup->work;
	const struct video_mode *mode = &video_modes[work->resolution %
	    VIDEO_MODE_COUNT];
	unsigned width, height;

	switch (item) {
	case ITEM_RESOLUTION:
		snprintf(out, len, "%s%s", mode->name,
		    work->resolution != setup->running_resolution ?
		    "  (restarts on exit)" : "");
		break;
	case ITEM_FONT: {
		unsigned cols, rows;
		int index = work_font_index(setup);
		if (index < 0) {
			snprintf(out, len, "%s  (not on this card)",
			    work->font);
			break;
		}
		work_cell(setup, &width, &height);
		setup_grid_for_cell(width, height, SIZE_FULL, mode->width,
		    mode->height, &cols, &rows);
		snprintf(out, len, "%s  (%ux%u cell, up to %ux%u)",
		    index < (int)font_builtin_count ?
		    font_builtins[index].name : work->font, width, height, cols,
		    rows);
		break;
	}
	case ITEM_SIZE: {
		unsigned cols, rows;
		work_cell(setup, &width, &height);
		bool clamped = setup_grid_for_cell(width, height,
		    work->term_size, mode->width, mode->height, &cols, &rows);
		const struct settings_size *want =
		    &settings_sizes[work->term_size % SIZE_COUNT];
		if (want->cols == 0)
			snprintf(out, len, "full screen (%ux%u)", cols, rows);
		else if (clamped)
			snprintf(out, len, "%ux%u -> %ux%u (doesn't fit)",
			    want->cols, want->rows, cols, rows);
		else
			snprintf(out, len, "%ux%u", cols, rows);
		break;
	}
	case ITEM_EMULATION: {
		const struct term_emulation_info *info =
		    &term_emulations[work->emulation % EMU_COUNT];
		snprintf(out, len, "%s  (host: TERM=%s)", info->name,
		    info->term_hint);
		break;
	}
	case ITEM_DEPTH: {
		static const char *const depth_names[DEPTH_COUNT] = {
			"off (monochrome)", "16 colours", "256 colours",
			"24-bit",
		};
		snprintf(out, len, "%s", depth_names[work->colour_depth %
		    DEPTH_COUNT]);
		break;
	}
	case ITEM_UTF8:
		snprintf(out, len, "%s", work->utf8 ? "on" :
		    "off (8-bit: C1 controls, Latin-1)");
		break;
	case ITEM_ALTSCREEN:
		snprintf(out, len, "%s", work->alt_screen ? "allowed" :
		    "ignored (full-screen apps leave output behind)");
		break;
	case ITEM_SIXEL:
		snprintf(out, len, "%s", work->sixel == 8 ?
		    "255 colours  (8 bits a pixel)" : work->sixel == 4 ?
		    "15 colours  (4 bits, VT340; 2x the image area)" :
		    work->sixel == 2 ? "3 colours  (2 bits; 4x the area)" :
		    work->sixel == 1 ? "1 colour  (1 bit; 8x the area)" :
		    "off");
		break;
	case ITEM_PRINTER:
		snprintf(out, len, "%s", work->printer ?
		    "on: SD card  (/print/NNNN.txt)" : "off");
		break;
	case ITEM_PROFILER:
		snprintf(out, len, "%s", work->profiler_bar ?
		    "on  (cycles a line vs. budget, top of screen)" : "off");
		break;
	case ITEM_SCREENSHOT:
		snprintf(out, len, "%s", work->screenshot_format == SHOT_QOI ?
		    "QOI  (/screens/NNNN.qoi, small)" :
		    "BMP  (/screens/NNNN.bmp, opens anywhere)");
		break;
	case ITEM_SCHEME:
		if (work->scheme_file[0] == '\0')
			snprintf(out, len, "%s",
			    settings_scheme_name(work->scheme));
		else
			snprintf(out, len, "%s  (base16, %s)", work->scheme_file,
			    work_scheme_index(setup) < 0 ? "not on this card" :
			    "card");
		break;
	case ITEM_CURSOR:
		snprintf(out, len, "%s", cursor_names[work->cursor_shape % 3]);
		break;
	case ITEM_BLINK:
	case ITEM_WRAP:
	case ITEM_ECHO:
	case ITEM_BELL: {
		bool on = item == ITEM_BLINK ? work->cursor_blink :
		    item == ITEM_WRAP ? work->autowrap :
		    item == ITEM_ECHO ? work->local_echo : work->visual_bell;
		snprintf(out, len, "%s", on ? "on" : "off");
		break;
	}
	case ITEM_NEWLINE:
		snprintf(out, len, "%s", work->newline_mode ?
		    "on  (Enter sends CR LF)" : "off (Enter sends CR)");
		break;
	case ITEM_ANSWERBACK:
		snprintf(out, len, "\"%s\"%s", work->answerback,
		    setup->editing ? "_" : "");
		break;
	default:
		out[0] = '\0';
		break;
	}
}

static const char *const item_labels[ITEM_COUNT] = {
	[ITEM_RESOLUTION] = "Resolution",
	[ITEM_FONT] = "Font",
	[ITEM_SIZE] = "Terminal size",
	[ITEM_EMULATION] = "Emulation",
	[ITEM_DEPTH] = "Colour depth",
	[ITEM_UTF8] = "UTF-8",
	[ITEM_ALTSCREEN] = "Alternate screen",
	[ITEM_SIXEL] = "Sixel graphics",
	[ITEM_SCHEME] = "Colour scheme",
	[ITEM_SCREENSHOT] = "Screenshot format",
	[ITEM_PRINTER] = "Printer",
	[ITEM_PROFILER] = "Profiler bar",
	[ITEM_CURSOR] = "Cursor",
	[ITEM_BLINK] = "Cursor blink",
	[ITEM_WRAP] = "Auto wrap",
	[ITEM_NEWLINE] = "New line mode",
	[ITEM_ECHO] = "Local echo",
	[ITEM_BELL] = "Visual bell",
	[ITEM_ANSWERBACK] = "Answerback",
	[ITEM_HOTKEYS] = "Hotkeys...",
	[ITEM_SAVE] = "Save settings          (S)",
	[ITEM_RECALL] = "Recall saved settings  (R)",
	[ITEM_DEFAULTS] = "Factory defaults       (D)",
	[ITEM_RESET] = "Reset terminal",
	[ITEM_EXIT] = "Exit SET-UP            (F3 / Esc)",
};

/*
 * Layout: title on row 0, a note on row 1 when there's room (28+ rows),
 * then the items, then a help line and a message line at the bottom.
 * Items that don't fit scroll: the window follows the selection, and
 * '^' / 'v' in column 1 say there's more above or below. (SET-UP used
 * to lay every item out at once and assumed 24 rows were enough; at 23
 * items the help line had already fallen off an 80x24 screen.)
 */
static unsigned
list_top(const struct setup *setup)
{
	return setup->grid->rows >= 28 ? FIRST_ROW : 1;
}

static unsigned
list_rows(const struct setup *setup)
{
	return setup->grid->rows - list_top(setup) - 2;
}

/* Keep `selected` inside the window [*scroll, *scroll + rows). */
static void
follow(unsigned selected, unsigned count, unsigned rows, unsigned *scroll)
{
	if (count <= rows)
		*scroll = 0;
	else if (selected < *scroll)
		*scroll = selected;
	else if (selected >= *scroll + rows)
		*scroll = selected - rows + 1;
}

static void
draw_more_marks(struct setup *setup, unsigned scroll, unsigned count)
{
	unsigned top = list_top(setup), rows = list_rows(setup);

	if (scroll > 0)
		put(setup, top, 1, "^", DIM_FG, MENU_BG);
	if (scroll + rows < count)
		put(setup, top + rows - 1, 1, "v", DIM_FG, MENU_BG);
}

/*
 * Tools listed on the Hotkeys page (the Tools menu's, in its order, for
 * those that exist yet).
 */
static unsigned
hotkey_tools(uint8_t tools[TOOL_COUNT])
{
	unsigned count = 0;

	for (unsigned tool = 0; tool < TOOL_COUNT; tool++)
		if (tool_ready((enum tool)tool))
			tools[count++] = (uint8_t)tool;
	return count;
}

static void
draw_hotkeys(struct setup *setup)
{
	uint8_t tools[TOOL_COUNT];
	unsigned count = hotkey_tools(tools);
	unsigned top = list_top(setup), rows = list_rows(setup);
	char name[24];

	follow(setup->hotkey_selected, count, rows, &setup->scroll);
	for (unsigned i = setup->scroll; i < count && i < setup->scroll + rows;
	    i++) {
		const struct hotkey *hotkey = &setup->work.hotkeys[tools[i]];
		bool selected = i == setup->hotkey_selected;
		uint16_t fg = selected ? MENU_BG : MENU_FG;
		uint16_t bg = selected ? MENU_FG : MENU_BG;
		unsigned row = top + i - setup->scroll;

		if (selected)
			grid_clear_span(setup->grid, row, LABEL_COL - 1,
			    setup->grid->cols - 2, grid_make_cell(
			    font_glyph_for(setup->font, ' '), fg, bg, 0));
		put(setup, row, LABEL_COL, tool_name((enum tool)tools[i]), fg,
		    bg);
		if (selected && setup->capturing)
			snprintf(name, sizeof(name), "press a key...");
		else
			keys_name((enum key_code)hotkey->code, hotkey->byte,
			    hotkey->mods, name, sizeof(name));
		put(setup, row, VALUE_COL, name, fg, bg);
	}
	draw_more_marks(setup, setup->scroll, count);
}

void
setup_draw(struct setup *setup)
{
	struct grid *grid = setup->grid;
	unsigned top = list_top(setup), rows = list_rows(setup);
	char value[64];

	for (unsigned row = 0; row < grid->rows; row++)
		fill_row(setup, row, MENU_FG, MENU_BG);

	fill_row(setup, 0, TITLE_FG, TITLE_BG);
	put(setup, 0, 1, setup->hotkeys_page ? "VeeTee 2350 SET-UP: Hotkeys" :
	    "VeeTee 2350 SET-UP", TITLE_FG, TITLE_BG);
	put(setup, 0, 30, setup->storage, TITLE_FG, TITLE_BG);
	if (grid->cols >= 80)
		put(setup, 0, grid->cols - 21, "BOOTSEL / F3: leave",
		    TITLE_FG, TITLE_BG);
	if (top > 1)
		put(setup, 1, LABEL_COL, setup->hotkeys_page ?
		    "Keys that run a tool from the terminal. Inside SET-UP "
		    "they don't apply." :
		    "Font, size, emulation, colours: reset the terminal.  "
		    "Resolution: restarts.", DIM_FG, MENU_BG);

	if (setup->hotkeys_page) {
		draw_hotkeys(setup);
	} else {
		follow(setup->selected, ITEM_COUNT, rows, &setup->scroll);
		for (unsigned item = setup->scroll;
		    item < ITEM_COUNT && item < setup->scroll + rows; item++) {
			bool selected = item == setup->selected;
			uint16_t fg = selected ? MENU_BG : MENU_FG;
			uint16_t bg = selected ? MENU_FG : MENU_BG;
			unsigned row = top + item - setup->scroll;

			if (selected) {
				grid_clear_span(grid, row, LABEL_COL - 1,
				    grid->cols - 2, grid_make_cell(
				    font_glyph_for(setup->font, ' '), fg, bg,
				    0));
			}
			put(setup, row, LABEL_COL, item_labels[item], fg, bg);
			format_value(setup, item, value, sizeof(value));
			put(setup, row, VALUE_COL, value, fg, bg);
		}
		draw_more_marks(setup, setup->scroll, ITEM_COUNT);
	}

	unsigned help_row = grid->rows - 2;
	const char *help = setup->editing ?
	    "Type the answerback; Enter or Esc to finish." :
	    setup->capturing ?
	    "Press the new key.  Esc: cancel" :
	    setup->hotkeys_page ?
	    "Up/Down select   Enter: new key   Delete: none   Esc: back" :
	    "Up/Down select   Left/Right/Enter change   S save   F3 leave";
	put(setup, help_row, LABEL_COL, help, DIM_FG, MENU_BG);
	put(setup, help_row + 1, LABEL_COL, setup->message, TITLE_BG,
	    MENU_BG);
}

void
setup_catalogue(struct setup *setup, const struct setup_catalogue *card)
{
	setup->card = *card;
	setup_draw(setup);
}

void
setup_storage(struct setup *setup, const char *text)
{
	snprintf(setup->storage, sizeof(setup->storage), "%s", text);
	setup_draw(setup);
}

void
setup_message(struct setup *setup, const char *text)
{
	snprintf(setup->message, sizeof(setup->message), "%s", text);
	setup_draw(setup);
}

void
setup_begin(struct setup *setup, const struct settings *current,
    struct grid *grid, const struct font *font, unsigned running_resolution)
{
	memset(setup, 0, sizeof(*setup));
	setup->work = *current;
	setup->grid = grid;
	setup->font = font;
	setup->running_resolution = running_resolution;
	setup->selected = ITEM_RESOLUTION;
	setup_draw(setup);
}

/* --- editing ---------------------------------------------------------- */

/*
 * Step `value` by +-1 within [0, count), wrapping. (Signed on purpose: an
 * earlier unsigned version computed (unsigned)-1 % 5 == 0, so "Left" did
 * nothing for 5-entry lists. Caught by test_setup.)
 */
static unsigned
cycle(unsigned value, unsigned count, int step)
{
	int next = (int)value + step;

	while (next < 0)
		next += (int)count;
	return (unsigned)next % count;
}

/* Change the selected item's value by `step` (+1 / -1). */
static void
change_value(struct setup *setup, int step)
{
	struct settings *work = &setup->work;

	switch (setup->selected) {
	case ITEM_RESOLUTION:
		work->resolution = (uint8_t)cycle(work->resolution,
		    VIDEO_MODE_COUNT, step);
		break;
	case ITEM_FONT: {
		unsigned total = font_builtin_count + setup->card.font_count;
		int current = work_font_index(setup);
		unsigned index = current < 0 ? 0 :
		    cycle((unsigned)current, total, step);
		if (index < font_builtin_count)
			snprintf(work->font, sizeof(work->font), "%s",
			    font_builtins[index].name);
		else
			snprintf(work->font, sizeof(work->font), "%s%s",
			    SETTINGS_CARD_PREFIX,
			    setup->card.fonts[index - font_builtin_count].name);
		break;
	}
	case ITEM_SIZE:
		work->term_size = (uint8_t)cycle(work->term_size, SIZE_COUNT, step);
		break;
	case ITEM_EMULATION: {
		/* A new level brings its own feature defaults. */
		work->emulation = (uint8_t)cycle(work->emulation, EMU_COUNT,
		    step);
		const struct term_emulation_info *info =
		    &term_emulations[work->emulation];
		work->colour_depth = info->depth;
		work->utf8 = info->utf8;
		work->alt_screen = info->alt_screen;
		work->sixel = info->sixel;
		break;
	}
	case ITEM_DEPTH:
		work->colour_depth = (uint8_t)cycle(work->colour_depth,
		    DEPTH_COUNT, step);
		break;
	case ITEM_UTF8:
		work->utf8 = !work->utf8;
		break;
	case ITEM_ALTSCREEN:
		work->alt_screen = !work->alt_screen;
		break;
	case ITEM_SIXEL: {
		/* off -> 8 -> 4 -> 2 -> 1 -> off (Right; Left the reverse) */
		static const uint8_t depths[] = {0, 8, 4, 2, 1};
		unsigned at = 0;
		for (unsigned i = 0; i < sizeof(depths); i++)
			if (depths[i] == work->sixel)
				at = i;
		work->sixel = depths[cycle(at, sizeof(depths), step)];
		break;
	}
	case ITEM_PRINTER:
		work->printer = !work->printer;
		break;
	case ITEM_PROFILER:
		work->profiler_bar = !work->profiler_bar;
		break;
	case ITEM_SCREENSHOT:
		work->screenshot_format = (uint8_t)cycle(
		    work->screenshot_format, SHOT_COUNT, step);
		break;
	case ITEM_SCHEME: {
		unsigned total = SCHEME_COUNT + setup->card.scheme_count;
		int current = work_scheme_index(setup);
		unsigned index = current < 0 ? 0 :
		    cycle((unsigned)current, total, step);
		if (index < SCHEME_COUNT) {
			work->scheme = (uint8_t)index;
			work->scheme_file[0] = '\0';
		} else {
			snprintf(work->scheme_file, sizeof(work->scheme_file),
			    "%s", setup->card.schemes[index - SCHEME_COUNT]);
		}
		break;
	}
	case ITEM_CURSOR:
		work->cursor_shape = (uint8_t)cycle(work->cursor_shape, 3, step);
		break;
	case ITEM_BLINK:
		work->cursor_blink = !work->cursor_blink;
		break;
	case ITEM_WRAP:
		work->autowrap = !work->autowrap;
		break;
	case ITEM_NEWLINE:
		work->newline_mode = !work->newline_mode;
		break;
	case ITEM_ECHO:
		work->local_echo = !work->local_echo;
		break;
	case ITEM_BELL:
		work->visual_bell = !work->visual_bell;
		break;
	default:
		break;
	}
}

static enum setup_action
edit_answerback(struct setup *setup, const struct key_event *key)
{
	char *text = setup->work.answerback;
	size_t len = strlen(text);

	switch (key->code) {
	case KEY_ENTER:
	case KEY_ESCAPE:
		setup->editing = false;
		break;
	case KEY_BACKSPACE:
		if (len > 0)
			text[len - 1] = '\0';
		break;
	case KEY_CHAR:
		if (key->mods == 0 && key->byte >= 0x20 && key->byte < 0x7f &&
		    len + 1 < SETTINGS_ANSWERBACK_MAX) {
			text[len] = (char)key->byte;
			text[len + 1] = '\0';
		}
		break;
	default:
		break;
	}
	setup_draw(setup);
	return SETUP_NONE;
}

static enum setup_action
activate(struct setup *setup)
{
	switch (setup->selected) {
	case ITEM_ANSWERBACK:
		setup->editing = true;
		return SETUP_NONE;
	case ITEM_HOTKEYS:
		setup->hotkeys_page = true;
		setup->hotkey_selected = 0;
		setup->scroll = 0;
		return SETUP_NONE;
	case ITEM_SAVE:
		return SETUP_SAVE;
	case ITEM_RECALL:
		return SETUP_RECALL;
	case ITEM_DEFAULTS:
		return SETUP_DEFAULTS;
	case ITEM_RESET:
		return SETUP_RESET_TERMINAL;
	case ITEM_EXIT:
		return SETUP_EXIT;
	default:
		change_value(setup, +1);
		return SETUP_NONE;
	}
}

/*
 * Bind the key just pressed to the selected tool. A key runs one tool:
 * if another had it, that one loses it (and the message says so).
 */
static void
bind_hotkey(struct setup *setup, enum tool tool, const struct key_event *key)
{
	struct hotkey bound = {
		.code = (uint8_t)key->code,
		.byte = key->code == KEY_CHAR ? key->byte : 0,
		.mods = key->mods,
	};
	char name[24];

	keys_name(key->code, bound.byte, bound.mods, name, sizeof(name));
	snprintf(setup->message, sizeof(setup->message), "%s: %s", name,
	    tool_name(tool));
	for (unsigned other = 0; other < TOOL_COUNT; other++) {
		if (other == tool ||
		    !hotkey_matches(&setup->work.hotkeys[other], key))
			continue;
		setup->work.hotkeys[other] = (struct hotkey){0};
		snprintf(setup->message, sizeof(setup->message),
		    "%s: %s (taken from %s)", name, tool_name(tool),
		    tool_name((enum tool)other));
	}
	setup->work.hotkeys[tool] = bound;
}

static void
hotkeys_key(struct setup *setup, const struct key_event *key)
{
	uint8_t tools[TOOL_COUNT];
	unsigned count = hotkey_tools(tools);
	enum tool tool = (enum tool)tools[setup->hotkey_selected];

	setup->message[0] = '\0';
	if (setup->capturing) {
		setup->capturing = false;
		if (key->code == KEY_ESCAPE || key->code == KEY_UNKNOWN)
			return;		/* Esc can't be a hotkey: it cancels */
		bind_hotkey(setup, tool, key);
		return;
	}
	switch (key->code) {
	case KEY_UP:
		setup->hotkey_selected = cycle(setup->hotkey_selected, count,
		    -1);
		break;
	case KEY_DOWN:
	case KEY_TAB:
		setup->hotkey_selected = cycle(setup->hotkey_selected, count,
		    +1);
		break;
	case KEY_ENTER:
		setup->capturing = true;
		break;
	case KEY_DELETE:
	case KEY_BACKSPACE:
		setup->work.hotkeys[tool] = (struct hotkey){0};
		break;
	case KEY_ESCAPE:
	case KEY_LEFT:
	case KEY_F3:
		setup->hotkeys_page = false;
		setup->scroll = 0;
		break;
	default:
		break;
	}
}

enum setup_action
setup_key(struct setup *setup, const struct key_event *key)
{
	enum setup_action action = SETUP_NONE;

	if (setup->editing)
		return edit_answerback(setup, key);
	if (setup->hotkeys_page) {
		hotkeys_key(setup, key);
		setup_draw(setup);
		return SETUP_NONE;
	}

	setup->message[0] = '\0';
	switch (key->code) {
	case KEY_UP:
		setup->selected = cycle(setup->selected, ITEM_COUNT, -1);
		break;
	case KEY_DOWN:
	case KEY_TAB:
		setup->selected = cycle(setup->selected, ITEM_COUNT, +1);
		break;
	case KEY_LEFT:
		change_value(setup, -1);
		break;
	case KEY_RIGHT:
		change_value(setup, +1);
		break;
	case KEY_ENTER:
		action = activate(setup);
		break;
	case KEY_ESCAPE:
	case KEY_F3:
		action = SETUP_EXIT;
		break;
	case KEY_CHAR:
		if (key->mods != 0)
			break;		/* Alt+s isn't "save" */
		switch (key->byte) {
		case ' ':
			if (setup->selected < ITEM_ANSWERBACK)
				change_value(setup, +1);
			break;
		case 's': case 'S':
			action = SETUP_SAVE;
			break;
		case 'r': case 'R':
			action = SETUP_RECALL;
			break;
		case 'd': case 'D':
			action = SETUP_DEFAULTS;
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
	setup_draw(setup);
	return action;
}

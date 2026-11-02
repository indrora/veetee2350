/*
 * setup.c — SET-UP screen: drawing and key handling.
 *
 * Drawing is plain ASCII on purpose: SET-UP must be readable in every
 * built-in font, and some (Not Jam Casual 10) have nothing beyond ASCII.
 */
#include "setup.h"

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
	const struct settings_size *want = &settings_sizes[size % SIZE_COUNT];

	font_builtin_cell(builtin, &cell_w, &cell_h);
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

static const struct font_builtin *
work_font(const struct setup *setup)
{
	const struct font_builtin *builtin =
	    font_builtin_find(setup->work.font);

	return builtin != nullptr ? builtin : &font_builtins[0];
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
	const struct font_builtin *builtin = work_font(setup);
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
		font_builtin_cell(builtin, &width, &height);
		setup_grid_for(builtin, SIZE_FULL, mode->width, mode->height,
		    &cols, &rows);
		snprintf(out, len, "%s  (%ux%u cell, up to %ux%u)",
		    builtin->name, width, height, cols, rows);
		break;
	}
	case ITEM_SIZE: {
		unsigned cols, rows;
		bool clamped = setup_grid_for(builtin, work->term_size, mode->width,
		    mode->height, &cols, &rows);
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
		snprintf(out, len, "%s", work->sixel ? "on" : "off");
		break;
	case ITEM_SCHEME:
		snprintf(out, len, "%s", settings_scheme_name(work->scheme));
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
	[ITEM_CURSOR] = "Cursor",
	[ITEM_BLINK] = "Cursor blink",
	[ITEM_WRAP] = "Auto wrap",
	[ITEM_NEWLINE] = "New line mode",
	[ITEM_ECHO] = "Local echo",
	[ITEM_BELL] = "Visual bell",
	[ITEM_ANSWERBACK] = "Answerback",
	[ITEM_SAVE] = "Save settings          (S)",
	[ITEM_RECALL] = "Recall saved settings  (R)",
	[ITEM_DEFAULTS] = "Factory defaults       (D)",
	[ITEM_RESET] = "Reset terminal",
	[ITEM_EXIT] = "Exit SET-UP            (F3 / Esc)",
};

/*
 * Screen row of each item. Everything (title, note, 20 items, help,
 * message) fits exactly in 24 rows, so SET-UP works on an 80x24 terminal.
 */
static unsigned
item_row(unsigned item)
{
	return FIRST_ROW + item;
}

void
setup_draw(struct setup *setup)
{
	struct grid *grid = setup->grid;
	char value[64];

	for (unsigned row = 0; row < grid->rows; row++)
		fill_row(setup, row, MENU_FG, MENU_BG);

	fill_row(setup, 0, TITLE_FG, TITLE_BG);
	put(setup, 0, 1, "VeeTee 2350 SET-UP", TITLE_FG, TITLE_BG);
	put(setup, 0, grid->cols - 21, "BOOTSEL / F3: leave", TITLE_FG,
	    TITLE_BG);
	put(setup, 0, 22, setup->storage, TITLE_FG, TITLE_BG);

	for (unsigned item = 0; item < ITEM_COUNT; item++) {
		bool selected = item == setup->selected;
		uint16_t fg = selected ? MENU_BG : MENU_FG;
		uint16_t bg = selected ? MENU_FG : MENU_BG;
		unsigned row = item_row(item);

		if (selected) {
			grid_clear_span(grid, row, LABEL_COL - 1,
			    grid->cols - 2, grid_make_cell(font_glyph_for(
			    setup->font, ' '), fg, bg, 0));
		}
		put(setup, row, LABEL_COL, item_labels[item], fg, bg);
		format_value(setup, item, value, sizeof(value));
		put(setup, row, VALUE_COL, value, fg, bg);
	}

	put(setup, 1, LABEL_COL, "Font, size, emulation, colours: reset the "
	    "terminal.  Resolution: restarts.", DIM_FG, MENU_BG);
	unsigned help_row = item_row(ITEM_EXIT) + 1;
	put(setup, help_row, LABEL_COL, setup->editing ?
	    "Type the answerback; Enter or Esc to finish." :
	    "Up/Down select   Left/Right/Enter change   S save   F3 leave",
	    DIM_FG, MENU_BG);
	put(setup, help_row + 1, LABEL_COL, setup->message, TITLE_BG,
	    MENU_BG);
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
		unsigned index = (unsigned)(work_font(setup) - font_builtins);
		index = cycle(index, font_builtin_count, step);
		snprintf(work->font, sizeof(work->font), "%s",
		    font_builtins[index].name);
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
	case ITEM_SIXEL:
		work->sixel = !work->sixel;
		break;
	case ITEM_SCHEME:
		work->scheme = (uint8_t)cycle(work->scheme, SCHEME_COUNT, step);
		break;
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
		if (key->byte >= 0x20 && key->byte < 0x7f &&
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

enum setup_action
setup_key(struct setup *setup, const struct key_event *key)
{
	enum setup_action action = SETUP_NONE;

	if (setup->editing)
		return edit_answerback(setup, key);

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

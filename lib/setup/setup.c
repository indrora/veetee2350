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

#include "build.h"
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
		    "  (restarts when saved)" : "");
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
	case ITEM_CAPTURE:
		snprintf(out, len, "%s", work->capture ?
		    "on: SD card  (/capture/NNNN.rec, ttyrec)" : "off");
		break;
	case ITEM_NEWLINES: {
		static const char *const choices[NEWLINES_COUNT] = {
			[NEWLINES_AS_IS] = "as in the file",
			[NEWLINES_RAW] = "LF -> CR LF in raw files",
			[NEWLINES_ALL] = "LF -> CR LF in all files",
		};
		snprintf(out, len, "%s",
		    choices[work->replay_newlines % NEWLINES_COUNT]);
		break;
	}
	case ITEM_STATUS: {
		static const char *const modes[STATUS_MODE_COUNT] = {
			[STATUS_HIDDEN] = "hidden  (lights in the corner)",
			[STATUS_BOTTOM] = "bottom  (a row of its own)",
			[STATUS_TOP] = "top  (a row of its own)",
			[STATUS_PEEK_BOTTOM] = "peek, bottom  (slides up on news)",
			[STATUS_PEEK_TOP] = "peek, top  (slides down on news)",
		};
		snprintf(out, len, "%s",
		    modes[work->status_line % STATUS_MODE_COUNT]);
		break;
	}
	case ITEM_PEEK:
		snprintf(out, len, "%u seconds",
		    settings_peek_seconds[work->peek_time %
		    SETTINGS_PEEK_TIMES]);
		break;
	case ITEM_PROFILER:
		snprintf(out, len, "%s",
		    work->profiler_bar == PROFILER_ON ?
		    "on  (cycles a line vs. budget, top of screen)" :
		    work->profiler_bar == PROFILER_DEBUG ?
		    (VEETEE_DEBUG_BUILD ? "debug builds  (this is one: on)" :
		    "debug builds  (off here)") : "off");
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
	case ITEM_LINE_PORT:
		snprintf(out, len, "%s", work->line_port == LINE_UART ?
		    "UART  (header pins 8 TX, 10 RX)" : "USB  (VeeTee line)");
		break;
	case ITEM_BAUD:
		snprintf(out, len, "%lu", (unsigned long)settings_bauds[
		    work->line_baud % SETTINGS_BAUDS]);
		break;
	case ITEM_DATA_BITS:
		snprintf(out, len, "%u", work->line_data_bits);
		break;
	case ITEM_PARITY: {
		static const char *const names[PARITY_COUNT] = {
			"none", "even", "odd",
		};
		snprintf(out, len, "%s", names[work->line_parity %
		    PARITY_COUNT]);
		break;
	}
	case ITEM_STOP_BITS:
		snprintf(out, len, "%u", work->line_stop_bits);
		break;
	case ITEM_FLOW:
		snprintf(out, len, "%s", work->line_flow == FLOW_RTS_CTS ?
		    "RTS/CTS  (pins 26 RTS, 31 CTS)" : "none");
		break;
	/* The directory shows what's on each page, in brief. */
	case ITEM_PAGE_DISPLAY:
		snprintf(out, len, "%s, %s", mode->name,
		    work_font_index(setup) < (int)font_builtin_count &&
		    work_font_index(setup) >= 0 ?
		    font_builtins[work_font_index(setup)].name : work->font);
		break;
	case ITEM_PAGE_TERMINAL:
		snprintf(out, len, "%s",
		    term_emulations[work->emulation % EMU_COUNT].name);
		break;
	case ITEM_PAGE_COMMS:
		settings_line_summary(work, out, (unsigned)len);
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
	[ITEM_CAPTURE] = "Capture",
	[ITEM_NEWLINES] = "Replay newlines",
	[ITEM_STATUS] = "Status line",
	[ITEM_PEEK] = "Peek time",
	[ITEM_PROFILER] = "Profiler bar",
	[ITEM_CURSOR] = "Cursor",
	[ITEM_BLINK] = "Cursor blink",
	[ITEM_WRAP] = "Auto wrap",
	[ITEM_NEWLINE] = "New line mode",
	[ITEM_ECHO] = "Local echo",
	[ITEM_BELL] = "Visual bell",
	[ITEM_ANSWERBACK] = "Answerback",
	[ITEM_LINE_PORT] = "Port",
	[ITEM_BAUD] = "Speed (baud)",
	[ITEM_DATA_BITS] = "Data bits",
	[ITEM_PARITY] = "Parity",
	[ITEM_STOP_BITS] = "Stop bits",
	[ITEM_FLOW] = "Flow control",
	[ITEM_PAGE_DISPLAY] = "Display...",
	[ITEM_PAGE_TERMINAL] = "Terminal...",
	[ITEM_PAGE_COMMS] = "Communications...",
	[ITEM_PAGE_CARD] = "Printer and card...",
	[ITEM_HOTKEYS] = "Hotkeys...",
	[ITEM_SAVE] = "Save settings          (S)",
	[ITEM_RECALL] = "Recall saved settings  (R)",
	[ITEM_DEFAULTS] = "Factory defaults       (D)",
	[ITEM_RESET] = "Reset terminal",
	[ITEM_SAVE_EXIT] = "Save and exit          (F4)",
	[ITEM_EXIT] = "Exit SET-UP            (F3 / Esc)",
};

/*
 * What each page lists, in order. SET-UP was one list of 30 lines; by
 * the time the serial line's six settings joined it, it scrolled on
 * most screens. The split follows the VT420/VT520's Set-Up Directory:
 * the directory is where you start and where Esc brings you back.
 */
struct page {
	const char	*title;		/* after "VeeTee 2350 SET-UP: " */
	const char	*note;		/* row 1, when there's room */
	unsigned	 count;
	const uint8_t	*items;
};

static const uint8_t directory_items[] = {
	ITEM_PAGE_DISPLAY, ITEM_PAGE_TERMINAL, ITEM_PAGE_COMMS, ITEM_PAGE_CARD,
	ITEM_HOTKEYS, ITEM_SAVE, ITEM_RECALL, ITEM_DEFAULTS, ITEM_RESET,
	ITEM_SAVE_EXIT, ITEM_EXIT,
};
static const uint8_t display_items[] = {
	ITEM_RESOLUTION, ITEM_FONT, ITEM_SIZE, ITEM_SCHEME, ITEM_CURSOR,
	ITEM_BLINK, ITEM_STATUS, ITEM_PEEK, ITEM_PROFILER,
};
static const uint8_t terminal_items[] = {
	ITEM_EMULATION, ITEM_DEPTH, ITEM_UTF8, ITEM_ALTSCREEN, ITEM_SIXEL,
	ITEM_WRAP, ITEM_NEWLINE, ITEM_ECHO, ITEM_BELL, ITEM_ANSWERBACK,
};
static const uint8_t comms_items[] = {
	ITEM_LINE_PORT, ITEM_BAUD, ITEM_DATA_BITS, ITEM_PARITY, ITEM_STOP_BITS,
	ITEM_FLOW,
};
static const uint8_t card_items[] = {
	ITEM_SCREENSHOT, ITEM_PRINTER, ITEM_CAPTURE, ITEM_NEWLINES,
};

#define PAGE(title, note, list) {title, note, sizeof(list), list}
static const struct page pages[PAGE_COUNT] = {
	[PAGE_DIRECTORY] = PAGE(nullptr,
	    "Enter: open a page.  Esc on a page comes back here.",
	    directory_items),
	[PAGE_DISPLAY] = PAGE("Display",
	    "Font, size, colours: reset the terminal.  Resolution: restarts.",
	    display_items),
	[PAGE_TERMINAL] = PAGE("Terminal",
	    "Emulation sets depth, UTF-8, alt screen and Sixel to its own.",
	    terminal_items),
	[PAGE_COMMS] = PAGE("Communications",
	    "Applies when saved.  Speed, bits, parity, flow: UART only.",
	    comms_items),
	[PAGE_CARD] = PAGE("Printer and card",
	    "Files go to the SD card.", card_items),
	[PAGE_HOTKEYS] = {"Hotkeys",
	    "Keys that run a tool from the terminal. Inside SET-UP they "
	    "don't apply.", 0, nullptr},
};

/* Where `item` is on the current page's list; -1 if it isn't there. */
static int
page_index(const struct setup *setup, unsigned item)
{
	const struct page *page = &pages[setup->page % PAGE_COUNT];

	for (unsigned i = 0; i < page->count; i++)
		if (page->items[i] == item)
			return (int)i;
	return -1;
}

/* The page a directory entry opens (PAGE_DIRECTORY if it opens none). */
static enum setup_page
page_opened_by(unsigned item)
{
	switch (item) {
	case ITEM_PAGE_DISPLAY:		return PAGE_DISPLAY;
	case ITEM_PAGE_TERMINAL:	return PAGE_TERMINAL;
	case ITEM_PAGE_COMMS:		return PAGE_COMMS;
	case ITEM_PAGE_CARD:		return PAGE_CARD;
	case ITEM_HOTKEYS:		return PAGE_HOTKEYS;
	default:			return PAGE_DIRECTORY;
	}
}

/* Open `page` on its first line; the directory remembers its entry. */
static void
open_page(struct setup *setup, enum setup_page page)
{
	unsigned from = setup->page;

	setup->page = page;
	setup->scroll = 0;
	setup->hotkey_selected = 0;
	if (page == PAGE_DIRECTORY) {
		/* Back on the entry that opened the page we came from. */
		static const uint8_t entry[PAGE_COUNT] = {
			[PAGE_DIRECTORY] = ITEM_PAGE_DISPLAY,
			[PAGE_DISPLAY] = ITEM_PAGE_DISPLAY,
			[PAGE_TERMINAL] = ITEM_PAGE_TERMINAL,
			[PAGE_COMMS] = ITEM_PAGE_COMMS,
			[PAGE_CARD] = ITEM_PAGE_CARD,
			[PAGE_HOTKEYS] = ITEM_HOTKEYS,
		};
		setup->selected = entry[from % PAGE_COUNT];
	} else if (pages[page].count > 0) {
		setup->selected = pages[page].items[0];
	}
}

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
	const struct page *page = &pages[setup->page % PAGE_COUNT];
	char value[64];

	for (unsigned row = 0; row < grid->rows; row++)
		fill_row(setup, row, MENU_FG, MENU_BG);

	fill_row(setup, 0, TITLE_FG, TITLE_BG);
	snprintf(value, sizeof(value), "VeeTee 2350 SET-UP%s%s",
	    page->title != nullptr ? ": " : "",
	    page->title != nullptr ? page->title : "");
	put(setup, 0, 1, value, TITLE_FG, TITLE_BG);
	/* The card status moves right of a long title. */
	put(setup, 0, strlen(value) + 3 > 30 ? (unsigned)strlen(value) + 3 : 30,
	    setup->storage, TITLE_FG, TITLE_BG);
	if (grid->cols >= 80)
		put(setup, 0, grid->cols - 21, "BOOTSEL / F3: leave",
		    TITLE_FG, TITLE_BG);
	if (top > 1)
		put(setup, 1, LABEL_COL, page->note, DIM_FG, MENU_BG);

	if (setup->page == PAGE_HOTKEYS) {
		draw_hotkeys(setup);
	} else {
		int at = page_index(setup, setup->selected);
		follow(at < 0 ? 0 : (unsigned)at, page->count, rows,
		    &setup->scroll);
		for (unsigned i = setup->scroll;
		    i < page->count && i < setup->scroll + rows; i++) {
			unsigned item = page->items[i];
			bool selected = item == setup->selected;
			uint16_t fg = selected ? MENU_BG : MENU_FG;
			uint16_t bg = selected ? MENU_FG : MENU_BG;
			unsigned row = top + i - setup->scroll;

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
		draw_more_marks(setup, setup->scroll, page->count);
	}

	unsigned help_row = grid->rows - 2;
	const char *help = setup->editing ?
	    "Type the answerback; Enter or Esc to finish." :
	    setup->capturing ?
	    "Press the new key.  Esc: cancel" :
	    setup->page == PAGE_HOTKEYS ?
	    "Up/Down select   Enter: new key   Delete: none   Esc: back" :
	    setup->page == PAGE_DIRECTORY ?
	    "Up/Down select  Enter open  S save  F4 save & exit" :
	    /* F3 is in the title bar; 64 columns don't fit both here. */
	    "Up/Down  Left/Right change  Esc back  S save  F4 save&exit";
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
	setup->baseline = *current;
	setup->grid = grid;
	setup->font = font;
	setup->running_resolution = running_resolution;
	setup->page = PAGE_DIRECTORY;
	setup->selected = ITEM_PAGE_DISPLAY;
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
	case ITEM_RESOLUTION: {
		/* In SET-UP's order (video_modes.h), not storage order. */
		unsigned at = 0;
		for (unsigned i = 0; i < VIDEO_MODE_COUNT; i++)
			if (video_mode_order[i] == work->resolution)
				at = i;
		work->resolution = video_mode_order[cycle(at, VIDEO_MODE_COUNT,
		    step)];
		break;
	}
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
	case ITEM_CAPTURE:
		work->capture = !work->capture;
		break;
	case ITEM_NEWLINES:
		work->replay_newlines = (uint8_t)cycle(work->replay_newlines,
		    NEWLINES_COUNT, step);
		break;
	case ITEM_STATUS:
		work->status_line = (uint8_t)cycle(work->status_line,
		    STATUS_MODE_COUNT, step);
		break;
	case ITEM_PEEK:
		work->peek_time = (uint8_t)cycle(work->peek_time,
		    SETTINGS_PEEK_TIMES, step);
		break;
	case ITEM_PROFILER:
		work->profiler_bar = (uint8_t)cycle(work->profiler_bar,
		    PROFILER_MODES, step);
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
	case ITEM_LINE_PORT:
		work->line_port = (uint8_t)cycle(work->line_port,
		    LINE_PORT_COUNT, step);
		break;
	case ITEM_BAUD:
		work->line_baud = (uint8_t)cycle(work->line_baud,
		    SETTINGS_BAUDS, step);
		break;
	case ITEM_DATA_BITS:	/* 5 to 8 */
		work->line_data_bits = (uint8_t)(5 + cycle(
		    (unsigned)(work->line_data_bits - 5) % 4, 4, step));
		break;
	case ITEM_PARITY:
		work->line_parity = (uint8_t)cycle(work->line_parity,
		    PARITY_COUNT, step);
		break;
	case ITEM_STOP_BITS:
		work->line_stop_bits = work->line_stop_bits == 1 ? 2 : 1;
		break;
	case ITEM_FLOW:
		work->line_flow = (uint8_t)cycle(work->line_flow, FLOW_COUNT,
		    step);
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
	case ITEM_PAGE_DISPLAY:
	case ITEM_PAGE_TERMINAL:
	case ITEM_PAGE_COMMS:
	case ITEM_PAGE_CARD:
	case ITEM_HOTKEYS:
		open_page(setup, page_opened_by(setup->selected));
		return SETUP_NONE;
	case ITEM_SAVE:
		return SETUP_SAVE;
	case ITEM_RECALL:
		return SETUP_RECALL;
	case ITEM_DEFAULTS:
		return SETUP_DEFAULTS;
	case ITEM_RESET:
		return SETUP_RESET_TERMINAL;
	case ITEM_SAVE_EXIT:
		return SETUP_SAVE_EXIT;
	case ITEM_EXIT:
		return setup_exit(setup);
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
		open_page(setup, PAGE_DIRECTORY);
		break;
	default:
		break;
	}
}

bool
setup_dirty(const struct setup *setup)
{
	return !settings_same(&setup->work, &setup->baseline);
}

bool
setup_dirty_reset(const struct setup *setup)
{
	return setup->work.resolution != setup->baseline.resolution;
}

void
setup_saved(struct setup *setup)
{
	setup->baseline = setup->work;
}

/*
 * Leaving with unsaved changes asks first, rather than saving them
 * (which this used to do for a new resolution, as the restart needed
 * it) or dropping them unasked. N means "as if never here": nothing is
 * applied, nothing written, no restart. The question goes in the
 * message line; the next key answers it (confirm_key).
 */
enum setup_action
setup_exit(struct setup *setup)
{
	if (!setup_dirty(setup))
		return SETUP_EXIT;
	setup->confirming = true;
	snprintf(setup->message, sizeof(setup->message), "%s",
	    setup_dirty_reset(setup) ?
	    "Save changes and restart? Y: yes  N: discard  Esc: back" :
	    "Save changes? Y: yes  N: discard  Esc: back");
	setup_draw(setup);
	return SETUP_NONE;
}

/* The key after "Save changes?": Y or F4, N, or back to the menu. */
static enum setup_action
confirm_key(struct setup *setup, const struct key_event *key)
{
	bool plain = key->code == KEY_CHAR && key->mods == 0;

	setup->confirming = false;
	setup->message[0] = '\0';
	if (key->code == KEY_F4 || (plain && (key->byte == 'y' ||
	    key->byte == 'Y')))
		return SETUP_SAVE_EXIT;
	if (plain && (key->byte == 'n' || key->byte == 'N'))
		return SETUP_DISCARD;
	setup_draw(setup);
	return SETUP_NONE;
}

enum setup_action
setup_key(struct setup *setup, const struct key_event *key)
{
	enum setup_action action = SETUP_NONE;

	if (setup->confirming)
		return confirm_key(setup, key);
	if (setup->editing)
		return edit_answerback(setup, key);
	/* F3 leaves and F4 saves and leaves from any page. */
	if (!setup->capturing && key->code == KEY_F3)
		return setup_exit(setup);
	if (!setup->capturing && key->code == KEY_F4)
		return SETUP_SAVE_EXIT;
	if (setup->page == PAGE_HOTKEYS) {
		hotkeys_key(setup, key);
		setup_draw(setup);
		return SETUP_NONE;
	}

	const struct page *page = &pages[setup->page % PAGE_COUNT];
	int at = page_index(setup, setup->selected);

	setup->message[0] = '\0';
	switch (key->code) {
	case KEY_UP:
		setup->selected = page->items[cycle(at < 0 ? 0 : (unsigned)at,
		    page->count, -1)];
		break;
	case KEY_DOWN:
	case KEY_TAB:
		setup->selected = page->items[at < 0 ? 0 :
		    cycle((unsigned)at, page->count, +1)];
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
		/* Esc backs out a level: a page to the directory, the
		 * directory out of SET-UP. */
		if (setup->page != PAGE_DIRECTORY)
			open_page(setup, PAGE_DIRECTORY);
		else
			action = setup_exit(setup);
		break;
	case KEY_CHAR:
		if (key->mods != 0)
			break;		/* Alt+s isn't "save" */
		switch (key->byte) {
		case ' ':
			if (setup->selected < ITEM_SETTINGS_END &&
			    setup->selected != ITEM_ANSWERBACK)
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

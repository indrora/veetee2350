/*
 * status.c — the status line and the VT100 user lights (see status.h).
 *
 * Everything here runs on core 0's main loop, once a frame at most
 * (term_status_poll) or from the CSI fallback (DECLL). The renderer
 * reads the cells from its IRQ while they're rewritten; a torn cell
 * shows for one frame at worst, as with any grid write.
 *
 * Peeking: when something the indicator shows changes for a reason
 * worth looking at -- a light, the app's text or a message, the status
 * type -- the row slides in over the text's edge row, stays up for the
 * peek time (SET-UP: 3, 5, 7 or 10 s) and slides away. Cursor movement
 * is shown while it's up but doesn't make it peek: it would never go
 * away. The slide moves an eighth of the font height a frame, so 16-px
 * text comes in over 8 frames, about 130 ms at 60 Hz.
 */
#include "status.h"

#include <stdio.h>
#include <string.h>

#include "term.h"
#include "vterm_internal.h"	/* mode.insert (vendored libvterm) */

#define SLIDE_FRAMES	8	/* frames for a full slide in or out */
#define LIGHTS_WIDTH	4	/* " L1 " in the indicator */

/* Signed difference, so the ms counter may wrap (every 49 days). */
static bool
before(uint32_t now, uint32_t until)
{
	return (int32_t)(now - until) < 0;
}

uint8_t
term_status_place(uint8_t mode)
{
	switch (mode) {
	case TERM_STATUS_BOTTOM:
		return RENDER_STATUS_BOTTOM;
	case TERM_STATUS_TOP:
		return RENDER_STATUS_TOP;
	default:
		return RENDER_STATUS_NONE;
	}
}

static bool
peek_mode(uint8_t mode)
{
	return mode == TERM_STATUS_PEEK_BOTTOM || mode == TERM_STATUS_PEEK_TOP;
}

void
term_status_text(struct term *term, const char *text)
{
	struct term_status *status = &term->status;

	if (strncmp(status->text, text, sizeof(status->text) - 1) == 0)
		return;
	snprintf(status->text, sizeof(status->text), "%s", text);
	status->text_serial++;
	status->events++;
}

void
term_status_message(struct term *term, const char *text, uint32_t now_ms)
{
	struct term_status *status = &term->status;

	snprintf(status->message, sizeof(status->message), "%s", text);
	status->message_until = now_ms +
	    (uint32_t)term->options.peek_seconds * 1000u;
	status->text_serial++;
	status->events++;
}

int
term_status_decll(struct term *term, const long args[], int argcount)
{
	/* "CSI q" has one missing argument; treat a bare one the same. */
	for (int i = 0; i < (argcount > 0 ? argcount : 1); i++) {
		long arg = argcount > 0 ? CSI_ARG(args[i]) : CSI_ARG_MISSING;

		if (arg == CSI_ARG_MISSING || arg == 0)
			term->status_lights = 0;
		else if (arg >= 1 && arg <= STATUS_LIGHTS)
			term->status_lights |= (uint8_t)(1u << (arg - 1));
		else if (arg >= 21 && arg <= 20 + STATUS_LIGHTS)
			term->status_lights &= (uint8_t)~(1u << (arg - 21));
		/* Anything else is ignored, as on a VT510. */
	}
	return 1;
}

void
term_status_reset(struct term *term)
{
	term->status_lights = 0;
	/* A hidden status line starts as "none" (nothing to show it in). */
	term->status_type = term->options.status_mode == TERM_STATUS_HIDDEN ?
	    TERM_STATUS_NONE : TERM_STATUS_INDICATOR;
}

/* --- drawing ----------------------------------------------------------- */

static uint16_t
glyph(const struct term *term, char c)
{
	uint16_t index = font_glyph_for(&term->font, (uint8_t)c);

	return index != FONT_GLYPH_MISSING ? index :
	    font_glyph_for(&term->font, '?');
}

/* `text` into cells[col...], stopping before `end`; returns the col after. */
static unsigned
put_text(const struct term *term, struct cell *cells, unsigned col,
    unsigned end, const char *text, uint16_t fg, uint16_t bg)
{
	for (; *text != '\0' && col < end; text++, col++)
		cells[col] = grid_make_cell(glyph(term, *text), fg, bg, 0);
	return col;
}

/*
 * The indicator: lights, then the app's text, Insert and the message,
 * left to right with two spaces between, and the cursor on the right.
 * Bar colours are the theme's swapped; a lit light is the theme's own.
 */
static void
draw_indicator(const struct term *term, struct cell *cells,
    const struct term_status_shown *shown)
{
	const uint16_t bar_fg = shown->bg, bar_bg = shown->fg;
	const unsigned cols = shown->cols;
	char where[16];

	snprintf(where, sizeof(where), "%u,%u", shown->row + 1u,
	    shown->col + 1u);
	unsigned where_col = cols > strlen(where) + 1 ?
	    cols - (unsigned)strlen(where) - 1 : 0;
	unsigned end = where_col > 2 ? where_col - 2 : 0;

	for (unsigned light = 0; light < STATUS_LIGHTS; light++) {
		bool lit = shown->lights & (1u << light);
		char name[LIGHTS_WIDTH + 1] = {' ', 'L', (char)('1' + light),
		    ' ', '\0'};
		(void)put_text(term, cells, light * LIGHTS_WIDTH, end, name,
		    lit ? shown->fg : bar_fg, lit ? shown->bg : bar_bg);
	}

	const char *segments[] = {
		term->status.text,
		shown->insert ? "Insert" : "",
		term->status.message,
	};
	unsigned col = STATUS_LIGHTS * LIGHTS_WIDTH + 1;
	for (size_t i = 0; i < sizeof(segments) / sizeof(segments[0]); i++) {
		if (segments[i][0] == '\0')
			continue;
		col = put_text(term, cells, col, end, segments[i], bar_fg,
		    bar_bg) + 2;
	}
	(void)put_text(term, cells, where_col, cols, where, bar_fg, bar_bg);
}

/* The corner overlay: just the lit lights, " L1 L3 ". */
static void
draw_lights(struct term *term, const struct term_status_shown *shown)
{
	struct term_status *status = &term->status;
	const uint16_t bar_fg = shown->bg, bar_bg = shown->fg;
	unsigned width = 1;

	status->light_cells[0] = grid_make_cell(glyph(term, ' '), bar_fg,
	    bar_bg, 0);
	for (unsigned light = 0; light < STATUS_LIGHTS; light++) {
		if (!(shown->lights & (1u << light)))
			continue;
		char name[4] = {'L', (char)('1' + light), ' ', '\0'};
		width = put_text(term, status->light_cells, width,
		    STATUS_LIGHTS_COLS, name, bar_fg, bar_bg);
	}
	status->lights = (struct render_overlay){
		.row = 0,
		.rows = 1,
		.col = (uint16_t)(shown->cols > width ? shown->cols - width : 0),
		.cols = (uint16_t)width,
		.cells = status->light_cells,
	};
}

static void
redraw(struct term *term, const struct term_status_shown *shown)
{
	struct cell *cells = term->status.cells;
	const bool indicator = shown->type == TERM_STATUS_INDICATOR;
	/* "None" is a blank row in the screen's colours, not the bar's. */
	struct cell blank = grid_make_cell(glyph(term, ' '),
	    indicator ? shown->bg : shown->fg,
	    indicator ? shown->fg : shown->bg, 0);

	for (unsigned col = 0; col < GRID_MAX_COLS; col++)
		cells[col] = blank;
	if (indicator)
		draw_indicator(term, cells, shown);
	draw_lights(term, shown);
}

/* --- once a frame -------------------------------------------------------- */

static void
expire_message(struct term_status *status, uint32_t now_ms)
{
	if (status->message[0] != '\0' &&
	    !before(now_ms, status->message_until)) {
		status->message[0] = '\0';
		status->text_serial++;
	}
}

/*
 * A light, the status type, the app's text or a message changed: worth
 * a peek. (A message running out is not: that redraws, quietly.)
 */
static bool
peek_worthy(struct term *term)
{
	struct term_status *status = &term->status;
	bool changed = term->status_lights != status->peek_lights ||
	    term->status_type != status->peek_type ||
	    status->events != status->peek_events;

	status->peek_lights = term->status_lights;
	status->peek_type = term->status_type;
	status->peek_events = status->events;
	return changed;
}

/* Slide a peeking row one step toward where it should be. */
static void
slide(struct term *term, uint32_t now_ms)
{
	struct term_status *status = &term->status;
	const uint8_t mode = term->options.status_mode;
	const unsigned height = term->font.height;
	unsigned step = height / SLIDE_FRAMES ? height / SLIDE_FRAMES : 1;
	unsigned target = 0;

	if (peek_mode(mode) && term->status_type != TERM_STATUS_NONE &&
	    before(now_ms, status->peek_until))
		target = height;
	unsigned lines = status->peek_lines;
	if (lines < target)
		lines = lines + step < target ? lines + step : target;
	else if (lines > target)
		lines = lines > target + step ? lines - step : target;
	if (lines == status->peek_lines)
		return;
	status->peek_lines = (uint8_t)lines;
	render_status_peek(term->render, mode == TERM_STATUS_PEEK_TOP, lines);
}

void
term_status_poll(struct term *term, uint32_t now_ms, bool over_terminal)
{
	struct term_status *status = &term->status;
	struct term_status_shown shown;
	VTermPos cursor;

	expire_message(status, now_ms);
	if (peek_worthy(term))
		status->peek_until = now_ms +
		    (uint32_t)term->options.peek_seconds * 1000u;

	/* memset, not an initialiser: the struct is compared bytewise. */
	memset(&shown, 0, sizeof(shown));
	vterm_state_get_cursorpos(term->state, &cursor);
	shown.fg = term->default_fg;
	shown.bg = term->default_bg;
	shown.cols = term->screen->cols;
	shown.row = (uint16_t)cursor.row;
	shown.col = (uint16_t)cursor.col;
	shown.lights = term->status_lights;
	shown.type = term->status_type;
	shown.insert = term->state->mode.insert;
	shown.text_serial = status->text_serial;
	if (!status->drawn || memcmp(&shown, &status->shown,
	    sizeof(shown)) != 0) {
		redraw(term, &shown);
		status->shown = shown;
		status->drawn = true;
	}

	slide(term, now_ms);

	/*
	 * The corner lights: while one is lit and the indicator isn't on
	 * screen to show it (hidden, blanked, or not peeking right now).
	 */
	const uint8_t mode = term->options.status_mode;
	bool indicator_up = term->status_type == TERM_STATUS_INDICATOR &&
	    (term_status_place(mode) != RENDER_STATUS_NONE ||
	    (peek_mode(mode) && status->peek_lines == term->font.height));
	bool corner = over_terminal && term->status_lights != 0 &&
	    !indicator_up;
	term->render->overlay[RENDER_OVERLAY_LIGHTS] = corner ?
	    &status->lights : nullptr;
}

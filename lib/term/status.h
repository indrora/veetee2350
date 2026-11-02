/*
 * status.h — the status line and the VT100 user lights (DECLL).
 *
 * Layering: part of lib/term (pure, host-testable). The terminal keeps
 * the state the host and the app set -- which lights are lit (DECLL),
 * what the status line shows (DECSSDT), the app's text -- and once a
 * frame the main loop calls term_status_poll(), which turns it into
 * cells for the renderer:
 *
 *   - the status row, a row of cells outside the grid (render.h,
 *     status_cells). In the Bottom and Top modes it has a scanline row
 *     of its own (the terminal is a row shorter, see term.c's
 *     configured_size); in the Peek modes it slides in over the text's
 *     edge row when something changes and slides away again;
 *   - the lights in a corner overlay (render.h, RENDER_OVERLAY_LIGHTS),
 *     top right, while a light is lit and the indicator isn't on screen
 *     to show them.
 *
 * The status row is drawn in the theme's colours swapped, so it reads as
 * the terminal's chrome rather than host text, and follows the colour
 * scheme. Nothing here is in the grid: libvterm, readback (DECRQCRA) and
 * printing never see it; screenshots do, as they read scanlines.
 *
 *   " L1  L2  L3  L4   Printer  Insert  Screenshot saved     12,40 "
 *
 * A lit light is drawn in the theme's own colours, a dark box on the
 * bar. The host's DECSSDT 0 blanks the row (it stays reserved, as line
 * 25 did on a VT320); 1 is the indicator. 2 (host-writable) isn't
 * implemented yet and is refused, so DECSASD 1 is refused with it.
 */
#ifndef STATUS_H
#define STATUS_H

#include <stdbool.h>
#include <stdint.h>

#include "grid.h"
#include "render.h"

struct term;

/* Status line placement: SET-UP "Status line" (settings.h, same order). */
enum term_status_mode {
	TERM_STATUS_HIDDEN,
	TERM_STATUS_BOTTOM,
	TERM_STATUS_TOP,
	TERM_STATUS_PEEK_BOTTOM,
	TERM_STATUS_PEEK_TOP,
	TERM_STATUS_MODES
};

/* DECSSDT, "CSI Ps $ ~": what the status line shows. */
enum term_status_type {
	TERM_STATUS_NONE,		/* blank */
	TERM_STATUS_INDICATOR,
	TERM_STATUS_HOST_WRITABLE,	/* not yet: refused */
};

#define STATUS_LIGHTS		4	/* L1-L4, as on the VT100 keyboard */
#define STATUS_LIGHTS_COLS	(1 + 3 * STATUS_LIGHTS)	/* " L1 L2 L3 L4" */
#define STATUS_TEXT_MAX		24
#define STATUS_MESSAGE_MAX	48

/* What the status row was last drawn from: redrawn when this changes. */
struct term_status_shown {
	uint16_t	 fg, bg;	/* the theme's colours */
	uint16_t	 cols;
	uint16_t	 row, col;	/* cursor, 0-based */
	uint8_t		 lights;
	uint8_t		 type;
	bool		 insert;
	uint32_t	 text_serial;	/* text[] or message[] changed */
};

struct term_status {
	struct cell	 cells[GRID_MAX_COLS];	/* render->status_cells */
	struct cell	 light_cells[STATUS_LIGHTS_COLS];
	struct render_overlay lights;	/* points into light_cells */
	char		 text[STATUS_TEXT_MAX];	/* the app's: "Printer" */
	char		 message[STATUS_MESSAGE_MAX];	/* for a while */
	uint32_t	 message_until;	/* ms */
	uint32_t	 text_serial;	/* text/message changed: redraw */
	uint32_t	 events;	/* the app set one: worth a peek */
	uint32_t	 peek_until;	/* ms: peeking until then */
	uint8_t		 peek_lines;	/* scanlines of the row showing */
	uint8_t		 peek_lights;	/* what the last peek was for */
	uint8_t		 peek_type;
	uint32_t	 peek_events;
	bool		 drawn;		/* shown is valid */
	struct term_status_shown shown;
};

/*
 * The status row a mode reserves (render.h's enum render_status_place):
 * Bottom and Top take a row of their own, the others none.
 */
uint8_t	term_status_place(uint8_t mode);

/*
 * The app's standing text (printer, capture, transfer), "" for none.
 * A change makes a Peek-mode status line peek.
 */
void	term_status_text(struct term *term, const char *text);

/* A message for the next peek-time seconds ("Screenshot saved"). */
void	term_status_message(struct term *term, const char *text,
	    uint32_t now_ms);

/*
 * Once a frame, from the main loop: redraw the status row if anything it
 * shows changed, slide a peeking one, and show or hide the lights
 * overlay. `over_terminal`: false while SET-UP covers the terminal, so
 * the lights stay off its title row.
 */
void	term_status_poll(struct term *term, uint32_t now_ms,
	    bool over_terminal);

/*
 * DECLL, "CSI Ps ; ... q": 0 all off, 1-4 on, 21-24 off (VT510). Called
 * from term.c's CSI fallback; returns 1 (handled).
 */
int	term_status_decll(struct term *term, const long args[], int argcount);

/* RIS: lights off, the status line type back to SET-UP's. */
void	term_status_reset(struct term *term);

#endif /* STATUS_H */

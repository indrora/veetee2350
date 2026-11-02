/*
 * term.h — the terminal emulator: host bytes in, cell grid out.
 *
 * Layering: pure module above render/ and font/ (STYLE.md §4).
 *
 * Escape-sequence parsing and terminal state (cursor, modes, margins,
 * charsets, SGR pen, tab stops, DA/DSR replies...) are libvterm's state
 * layer. This module is the glue: libvterm's state callbacks become writes
 * into our render-ready grid (lib/render/grid.h), and its output callback
 * (device replies) goes back to the host through `output`.
 *
 * libvterm's own screen layer is deliberately NOT used: it keeps ~36 bytes
 * per cell for two screens, where our grid needs 12 (docs/spike-libghostty-
 * vt.md has the memory budget). The cost is that this file implements
 * scrolling, rectangle moves and erases itself.
 *
 * The primary and alternate screens are two grids; switching (DECSET
 * 47/1047/1049) just points the renderer at the other one.
 *
 * Runs on core 0. Not reentrant; one struct term per screen.
 */
#ifndef TERM_H
#define TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "font.h"
#include "grid.h"
#include "render.h"
#include "vterm.h"

/* Sink for bytes the terminal sends to the host (DA/DSR replies, ...). */
typedef void (*term_output_fn)(const char *bytes, size_t len, void *ctx);

/* Current SGR state, as libvterm reports it through setpenattr. */
struct term_pen {
	VTermColor	fg;
	VTermColor	bg;
	bool		bold;
	bool		underline;
	bool		italic;
	bool		blink;
	bool		reverse;
	bool		conceal;
	bool		strike;
};

struct term {
	VTerm		*vt;
	VTermState	*state;
	const struct font *font;
	struct render	*render;
	struct grid	 primary;
	struct grid	 alternate;
	struct grid	*screen;	/* the grid being written and shown */
	struct term_pen	 pen;
	uint16_t	 default_fg;	/* RGB565 */
	uint16_t	 default_bg;
	uint16_t	 space_glyph;
	term_output_fn	 output;
	void		*output_ctx;
	uint32_t	 bells;		/* BEL count, for a future visual bell */

	/* Presentation modes kept here so both grids can be updated
	 * when the screen switches. */
	bool		 cursor_visible;
	bool		 cursor_blink;
	uint8_t		 cursor_shape;
	bool		 reverse_video;
};

/*
 * Build a terminal sized for `font` (see render_grid_size), allocate both
 * screens, and point `render` at the primary one. Init-time allocation
 * only. Returns false if memory runs out.
 */
[[nodiscard]] bool term_init(struct term *term, const struct font *font,
	    struct render *render, term_output_fn output, void *output_ctx);
void	term_free(struct term *term);

/* Feed bytes received from the host. Any amount, any split. */
void	term_feed(struct term *term, const uint8_t *bytes, size_t len);

/* Send keyboard input towards the host (encoded by the caller). */
static inline void
term_send(struct term *term, const char *bytes, size_t len)
{
	term->output(bytes, len, term->output_ctx);
}

#endif /* TERM_H */

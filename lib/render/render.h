/*
 * render.h — turns the cell grid + font into RGB565 scanlines.
 *
 * Layering: pure module (hardware-free; host-testable). Its scanline
 * function has the same shape as hal/video.h's video_scanline_fn, and the
 * app wires the two together; render never includes video.h.
 *
 * render_scanline() runs on core 1 inside the DVI line budget. Everything
 * it needs per line is precomputed by render_init(): which grid row and
 * glyph row each scanline shows, plus the 2-pixel mask tables.
 */
#ifndef RENDER_H
#define RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "font.h"
#include "grid.h"

/* Largest screen the renderer supports (1280x720, see video_modes.h). */
#define RENDER_MAX_WIDTH	1280
#define RENDER_MAX_HEIGHT	720

/* Every combination of enum cell_attr bits (5 bits today). */
#define RENDER_ATTR_COMBOS	32
#define RENDER_MAX_CELL_HEIGHT	32

/* line_row[] value for scanlines above/below the grid. */
#define RENDER_LINE_BORDER	0xff
/* line_row[] value for the status row's scanlines (status_cells). */
#define RENDER_LINE_STATUS	0xfe
static_assert(GRID_MAX_ROWS < RENDER_LINE_STATUS, "row numbers clash");

/*
 * A status row of its own, below or above the text, laid out by
 * render_init together with the grid (centred as one block). A peeking
 * status line has none: it slides over the text's edge row instead
 * (render_status_peek).
 */
enum render_status_place {
	RENDER_STATUS_NONE,
	RENDER_STATUS_BOTTOM,
	RENDER_STATUS_TOP
};

/*
 * Tile pixels are 8, 4, 2 or 1 bits (the Sixel depth in SET-UP), packed
 * LSB-first within each byte. The all-ones value of the depth is
 * "transparent": the cell's background shows (255 at 8 bits).
 */
#define RENDER_TRANSPARENT	255	/* at 8 bits per pixel */

/*
 * A rectangle of cells drawn over the grid: the Tools menu, the user
 * lights. The screen underneath is untouched; the renderer composes the
 * covered rows on the fly (render.c, compose_row) and leaves out the
 * cursor where an overlay hides it. Slots draw in order, later on top.
 */
enum render_overlay_slot {
	RENDER_OVERLAY_LIGHTS,
	RENDER_OVERLAY_MENU,
	RENDER_OVERLAYS
};

struct render_overlay {
	uint8_t		 row;		/* first text row covered */
	uint8_t		 rows;
	uint16_t	 col;		/* first column covered */
	uint16_t	 cols;
	const struct cell *cells;	/* rows x cols, row-major */
};

/*
 * The frame plan (render_plan): what each text row is made of, worked
 * out once a frame by the main loop so the scanline only carries it out.
 *
 * A row is a few spans of cells, each from the grid's row (`cells`
 * nullptr: looked up at scanline time, so scrolling is never a frame
 * behind) or from an overlay; the Tools menu and the corner lights are
 * just spans, with nothing copied or composed. Each span is one of:
 *
 *   PAIR   text without attributes, all in the frame's commonest fg/bg
 *          pair: drawn from a table, built by the planner, of each
 *          glyph byte's eight pixels already in that pair's colours
 *   PLAIN  text without attributes, other colours: a lean loop
 *   MIXED  attributes or Sixel cells: the full loop
 *
 * A plan may be a frame old -- a span the host has just underlined may
 * still say PAIR -- so the lean loops still draw any cell right, only
 * more slowly (a PAIR span whose colours just changed shows the old
 * ones for that frame); and a removed overlay may show for one frame
 * more, so overlay cells must stay valid memory. Measured costs and the
 * reasoning: devlog 024.
 */
#define RENDER_PLAN_SPANS	5	/* grid, overlay, grid, overlay, grid */

enum render_span_kind {
	RENDER_SPAN_PAIR,
	RENDER_SPAN_PLAIN,
	RENDER_SPAN_MIXED,
};

struct render_span {
	const struct cell *cells;	/* nullptr: the grid row, from col */
	uint16_t	 col;
	uint8_t		 count;		/* GRID_MAX_COLS fits */
	uint8_t		 kind;		/* enum render_span_kind */
};

struct render_row_plan {
	uint8_t		 spans;
	bool		 covered;	/* an overlay covers part of it */
	struct render_span span[RENDER_PLAN_SPANS];
};

struct render_plan {
	struct render_row_plan row[GRID_MAX_ROWS];
};

/*
 * The profiler bar (SET-UP "Profiler bar"): the top RENDER_METER_LINES
 * scanlines become two bar graphs of the last frame's per-line cycle
 * counts against the line budget -- core 0's render (lines 0-2) and core
 * 1's TMDS encode (lines 4-6). Each bar stacks its segments left to
 * right, a dark tick between them, a red tick at the frame's worst line
 * and a white one at the budget, which sits at 80% of the width so an
 * overrun still shows. Filled in once a frame by the main loop.
 */
#define RENDER_METER_LINES	8
#define RENDER_METER_SEGMENTS	3

struct render_meter_bar {
	uint16_t	 max;		/* worst line this frame, cycles */
	uint8_t		 count;		/* segments used */
	struct {
		uint16_t cycles;	/* this segment's share of a line */
		uint16_t colour;	/* RGB565 */
	} segment[RENDER_METER_SEGMENTS];
};

struct render_meter {
	bool		 on;
	uint16_t	 budget;	/* cycles per line */
	struct render_meter_bar bar[2];	/* core 0 render, core 1 encode */
};

struct render {
	const struct font *font;
	struct grid * volatile grid;	/* swapped by render_set_grid */
	uint16_t	 width;		/* screen size, from render_set_screen */
	uint16_t	 height;
	uint16_t	 x_offset;	/* even, so cell pixels stay word-aligned */
	uint16_t	 y_offset;
	uint8_t		 strike_row;
	uint8_t		 overline_row;
	volatile bool	 active;	/* false: draw border only (see pause) */
	bool		 live;		/* scanout running: pause must wait */
	volatile uint32_t frame;	/* bumped by core 1 at line 0 */
	uint8_t		 line_row[RENDER_MAX_HEIGHT];	/* grid row, BORDER
							   or STATUS */
	uint8_t		 line_sub[RENDER_MAX_HEIGHT];	/* glyph row */
	/*
	 * The status line (lib/term/status.c): a row of cells outside the
	 * grid, at least grid->cols long, drawn on RENDER_LINE_STATUS lines
	 * without cursor, overlays or reverse video (DECSCNM). nullptr: no
	 * status line. status_place is read by render_init; text_top and
	 * text_end are the grid's first and one-past-last scanlines.
	 */
	const struct cell *status_cells;
	uint8_t		 status_place;	/* enum render_status_place */
	uint16_t	 text_top;
	uint16_t	 text_end;
	/*
	 * Sixel tiles for CELL_GRAPHIC cells (set by lib/term/graphics.c):
	 * one byte per pixel, font-cell sized, indexing `palette`;
	 * RENDER_TRANSPARENT shows the cell's background instead.
	 */
	const uint8_t	*tiles;
	uint16_t	*palette;	/* 256 x RGB565; the all-ones entry is
					   scratch for the renderer */
	uint16_t	 tile_bytes;	/* tile_row_bytes x font height */
	uint8_t		 tile_row_bytes;
	uint8_t		 tile_bpp;	/* 8, 4, 2 or 1 */
	/*
	 * Overlays, or nullptr. Set and cleared by core 0's main loop; the
	 * scanline runs in core 0's IRQ (or the main loop, for screenshots),
	 * so one pointer store is atomic for it. The cells must stay put
	 * while the pointer is set.
	 */
	const struct render_overlay *volatile overlay[RENDER_OVERLAYS];
	/*
	 * VEETEE_RENDER_PROFILE (hot.h): cycles spent composing overlay rows
	 * and how many lines needed it: running totals (they wrap), which
	 * readers difference. Always present, so the layout doesn't change.
	 */
	volatile uint32_t profile_compose_cycles;
	volatile uint32_t profile_compose_lines;
	/* ... and the column loop of text lines (the rest of a line's cost
	 * is set-up, borders and the cursor). */
	volatile uint32_t profile_loop_cycles;
	volatile uint32_t profile_loop_lines;
	/* VEETEE_RENDER_EXPERIMENTS: cycles of each variant (render.c),
	 * running totals, and how many times they ran. */
	volatile uint32_t experiment_cycles[8];
	volatile uint32_t experiment_runs;
	struct render_meter meter;	/* the profiler bar */
	/*
	 * The frame plan (render_plan): two, one being read by the scanline
	 * (`plan`, nullptr until the first) and one free for the next.
	 */
	struct render_plan plans[2];
	const struct render_plan *volatile plan;
	/*
	 * The PAIR table's colours (a cell's fg | bg << 16) and reverse
	 * video, and the commonest pair seen while planning, for the next.
	 */
	uint32_t	 pair;
	bool		 pair_invert;
	bool		 pair_ready;
	uint32_t	 pair_next;
	/* Per glyph row and attr combination: or-bits << 16 | and-bits. */
	uint32_t	 attr_ops[RENDER_MAX_CELL_HEIGHT][RENDER_ATTR_COMBOS];
};

/*
 * Set the output size (once, before anything else; from the video mode).
 * Clears the whole struct first, so it may live on the stack.
 */
void	render_set_screen(struct render *render, unsigned width,
	    unsigned height);

/* Grid dimensions a font produces on a screen of the given size. */
void	render_grid_size(const struct font *font, unsigned width,
	    unsigned height, unsigned *cols, unsigned *rows);

/*
 * Bind a font and a grid (whose size should come from render_grid_size)
 * and precompute the scanline tables. Leaves the renderer active.
 */
void	render_init(struct render *render, const struct font *font,
	    struct grid *grid);

/*
 * Stop the scanline callback touching font/grid so they can be swapped
 * (font change). If scanout is live (render->live, set by the app after
 * video_init), blocks until the line that might still be using them has
 * finished. Resume with render_init() on the new pair.
 */
void	render_pause(struct render *render);

/*
 * Show a different grid of the same size (alternate screen switch). A
 * single pointer store: core 1 reads render->grid once per scanline, so
 * the switch lands on a line boundary.
 */
static inline void
render_set_grid(struct render *render, struct grid *grid)
{
	render->grid = grid;
}

/*
 * Show the top `lines` scanlines' worth of the status row over the text:
 * sliding up over the bottom row (glyph rows 0..lines-1 visible) or, with
 * `top`, down over the top row (the row's last `lines` glyph rows). 0
 * puts the text back. Rewrites only that row's line tables; the IRQ may
 * see one line half-updated for a frame. Main loop only.
 */
void	render_status_peek(struct render *render, bool top, unsigned lines);

/*
 * Plan the next frame from the grid and the overlays (see struct
 * render_plan): main loop, once a frame, and after changing an overlay
 * when the change must show at once (a screenshot after closing the
 * menu). A few thousand cycles: one look at each cell's attributes.
 */
void	render_plan(struct render *render);

/* video_scanline_fn-compatible: ctx is the struct render. */
void	render_scanline(uint16_t *pixels, unsigned line, void *ctx);

#endif /* RENDER_H */

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
	uint8_t		 line_row[RENDER_MAX_HEIGHT];	/* grid row or BORDER */
	uint8_t		 line_sub[RENDER_MAX_HEIGHT];	/* glyph row */
	/* Per glyph row and attr combination: or-bits << 16 | and-bits. */
	uint32_t	 attr_ops[RENDER_MAX_CELL_HEIGHT][RENDER_ATTR_COMBOS];
};

/* Set the output size (once, before anything else; from the video mode). */
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

/* video_scanline_fn-compatible: ctx is the struct render. */
void	render_scanline(uint16_t *pixels, unsigned line, void *ctx);

#endif /* RENDER_H */

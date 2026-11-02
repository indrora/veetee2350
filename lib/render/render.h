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

#define RENDER_WIDTH	640
#define RENDER_HEIGHT	480

/* line_row[] value for scanlines above/below the grid. */
#define RENDER_LINE_BORDER	0xff

struct render {
	const struct font *font;
	struct grid	*grid;
	uint16_t	 x_offset;	/* even, so cell pixels stay word-aligned */
	uint16_t	 y_offset;
	uint8_t		 strike_row;
	uint8_t		 overline_row;
	volatile bool	 active;	/* false: draw border only (see pause) */
	volatile uint32_t frame;	/* bumped by core 1 at line 0 */
	uint8_t		 line_row[RENDER_HEIGHT];	/* grid row or BORDER */
	uint8_t		 line_sub[RENDER_HEIGHT];	/* glyph row */
};

/* Grid dimensions a font produces on the 640x480 screen. */
void	render_grid_size(const struct font *font, unsigned *cols,
	    unsigned *rows);

/*
 * Bind a font and a grid (whose size should come from render_grid_size)
 * and precompute the scanline tables. Leaves the renderer active.
 */
void	render_init(struct render *render, const struct font *font,
	    struct grid *grid);

/*
 * Stop core 1 touching font/grid so they can be swapped (font change).
 * Blocks until the scanline that might still be using them has finished.
 * Resume with render_init() on the new pair.
 */
void	render_pause(struct render *render);

/* video_scanline_fn-compatible: ctx is the struct render. */
void	render_scanline(uint16_t *pixels, unsigned line, void *ctx);

#endif /* RENDER_H */

/*
 * grid.h — the character cell grid shared by the terminal and the renderer.
 *
 * Layering: pure module. Written by the terminal emulator on core 0, read
 * by the scanline renderer on core 1.
 *
 * Cells are stored in a render-ready form (see struct cell): colours are
 * already RGB565, duplicated into both halves of a word, and fg is stored
 * as fg XOR bg. That makes the renderer's inner loop a handful of ALU ops
 * per two pixels, which is what lets text fit in the ~2900 spare cycles
 * per scanline (docs/hardware.md). Semantic state (SGR, palette indices)
 * lives in the terminal layer, which re-derives cells when it needs to.
 *
 * Scrolling never moves cell data: `row_map` maps logical (on-screen) rows
 * to physical storage rows, so a scroll is a rotation of a <=64-byte table
 * plus clearing the rows that scrolled in.
 *
 * Concurrency: core 1 may observe a cell or row_map mid-update. The worst
 * case is one frame (16 ms) showing a half-written cell or a duplicated
 * row, which is invisible in practice and avoids any locking on the
 * scanline path.
 */
#ifndef GRID_H
#define GRID_H

#include <stdbool.h>
#include <stdint.h>

#define GRID_MAX_COLS	160
#define GRID_MAX_ROWS	64

enum cell_attr {
	CELL_UNDERLINE	= 1 << 0,
	CELL_BLINK	= 1 << 1,
	CELL_INVISIBLE	= 1 << 2,
	CELL_STRIKE	= 1 << 3,
	CELL_OVERLINE	= 1 << 4,
};

struct cell {
	uint16_t	glyph;		/* font glyph index */
	uint16_t	attr;		/* enum cell_attr bits */
	uint32_t	bg2;		/* bg RGB565 in both halves */
	uint32_t	diff2;		/* (fg ^ bg) RGB565 in both halves */
};
static_assert(sizeof(struct cell) == 12);

enum cursor_shape {
	CURSOR_BLOCK,
	CURSOR_UNDERLINE,
	CURSOR_BAR,
};

struct grid {
	uint16_t	 cols;
	uint16_t	 rows;
	struct cell	*cells;			/* rows * cols, physical order */
	uint8_t		 row_map[GRID_MAX_ROWS];/* logical -> physical row */

	/* Presentation state, written by core 0, read per line by core 1. */
	volatile uint16_t cursor_row;
	volatile uint16_t cursor_col;
	volatile bool	 cursor_visible;
	volatile bool	 cursor_blink;
	volatile uint8_t cursor_shape;		/* enum cursor_shape */
	volatile bool	 reverse_video;		/* DECSCNM */
	volatile uint32_t border2;		/* colour outside the grid */
};

static inline uint32_t
grid_dup565(uint16_t colour)
{
	return (uint32_t)colour * 0x00010001u;
}

static inline uint16_t
grid_rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
	return (uint16_t)((red >> 3) << 11 | (green >> 2) << 5 | blue >> 3);
}

static inline struct cell
grid_make_cell(uint16_t glyph, uint16_t fg, uint16_t bg, uint16_t attr)
{
	return (struct cell){
		.glyph = glyph,
		.attr = attr,
		.bg2 = grid_dup565(bg),
		.diff2 = grid_dup565(fg ^ bg),
	};
}

/* Allocate a cols x rows grid filled with `blank`. Init-time only. */
[[nodiscard]] bool grid_init(struct grid *grid, unsigned cols, unsigned rows,
	    struct cell blank);
void	grid_free(struct grid *grid);

/* Cells of logical row `row` (0 = top of screen). */
static inline struct cell *
grid_row(struct grid *grid, unsigned row)
{
	return grid->cells + (unsigned)grid->row_map[row] * grid->cols;
}

/* Fill columns [col_start, col_end) of logical row `row` with `blank`. */
void	grid_clear_span(struct grid *grid, unsigned row, unsigned col_start,
	    unsigned col_end, struct cell blank);

/*
 * Scroll logical rows [top, bottom) by `count`: up moves content towards
 * row `top` (new blank rows appear at the bottom), down the reverse.
 */
void	grid_scroll_up(struct grid *grid, unsigned top, unsigned bottom,
	    unsigned count, struct cell blank);
void	grid_scroll_down(struct grid *grid, unsigned top, unsigned bottom,
	    unsigned count, struct cell blank);

#endif /* GRID_H */

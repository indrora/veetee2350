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
	CELL_ATTR_ALL	= (1 << 5) - 1,	/* render.c tables: 32 combos */
	/*
	 * Not a visual attribute: the cell was erased rather than written
	 * (a written space and an erased cell look alike but read back
	 * differently, e.g. DECRQCRA checksums treat erased as 0). The
	 * renderer masks it off with CELL_ATTR_ALL.
	 */
	CELL_ERASED	= 1 << 15,
	/* Not visual either: DECSCA-protected, skipped by DECSED/DECSEL. */
	CELL_PROTECTED	= 1 << 14,
	/*
	 * A Sixel graphic cell: `glyph` is a tile index into the graphics
	 * pool (lib/term/graphics.c) instead of a font glyph, and the
	 * renderer copies the tile's pixels through the graphics palette.
	 * Writing or erasing the cell makes it text again; the tile is
	 * reclaimed by the pool's next sweep.
	 */
	CELL_GRAPHIC	= 1 << 13,
};

/*
 * 8 bytes. Colours are plain RGB565, exactly what reaches the screen, so
 * nothing is lost; the renderer builds the doubled bg and (fg ^ bg)
 * words it works with from them (a handful of ALU ops per cell). Cells
 * used to carry those words pre-built, at 12 bytes: at 1280x720 the two
 * screens of 160x45 cells were 173 KB, now 115 KB (phase G).
 */
struct cell {
	uint16_t	glyph;		/* font glyph index */
	uint16_t	attr;		/* enum cell_attr bits */
	uint16_t	fg;		/* RGB565 */
	uint16_t	bg;		/* RGB565 */
} __attribute__((aligned(4)));
static_assert(sizeof(struct cell) == 8);
/* The renderer fetches fg+bg as one little-endian word, too. */
static_assert(__builtin_offsetof(struct cell, fg) == 4);
/* The renderer fetches glyph+attr as one little-endian word. */
static_assert(__builtin_offsetof(struct cell, glyph) == 0);
static_assert(__builtin_offsetof(struct cell, attr) == 2);

/* Line size (DECSWL / DECDWL / DECDHL), per physical row. */
enum row_attr {
	ROW_NORMAL,
	ROW_DOUBLE_WIDTH,	/* DECDWL */
	ROW_DOUBLE_TOP,		/* DECDHL top half (also double width) */
	ROW_DOUBLE_BOTTOM,	/* DECDHL bottom half */
};

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
	/* enum row_attr, indexed by PHYSICAL row so it moves with row_map
	 * on scroll, exactly as libvterm moves its own line info. */
	uint8_t		 row_attr[GRID_MAX_ROWS];

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
		.fg = fg,
		.bg = bg,
	};
}

/* Allocate a cols x rows grid filled with `blank`. Init-time only. */
[[nodiscard]] bool grid_init(struct grid *grid, unsigned cols, unsigned rows,
	    struct cell blank);
/*
 * The same in the caller's `cells` (cols x rows of them), for a grid that
 * must never fail for want of heap: SET-UP's (app.c). Never grid_free it.
 */
[[nodiscard]] bool grid_init_in(struct grid *grid, unsigned cols,
	    unsigned rows, struct cell *cells, struct cell blank);
void	grid_free(struct grid *grid);

/* Cells of logical row `row` (0 = top of screen). */
static inline struct cell *
grid_row(struct grid *grid, unsigned row)
{
	return grid->cells + (unsigned)grid->row_map[row] * grid->cols;
}

static inline uint8_t *
grid_row_attr(struct grid *grid, unsigned row)
{
	return &grid->row_attr[grid->row_map[row]];
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

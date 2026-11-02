/*
 * graphics.h — Sixel images as graphic cells (internal to lib/term).
 *
 * Storage model: every character cell an image touches becomes a
 * CELL_GRAPHIC cell whose `glyph` is the index of a tile -- one byte per
 * pixel, exactly one font cell in size -- in a pool allocated once at
 * boot. Everything the grid already does (scrolling by row map, erase,
 * DECCRA, the alternate screen, text overwriting a cell) then applies to
 * pictures for free. Tiles are never freed explicitly: when the pool runs
 * dry, a mark-sweep over both screens keeps the tiles some cell still
 * points at and recycles the rest.
 *
 * Colour: tile pixels index one shared 256-entry RGB565 palette (index
 * 255 = transparent, the cell's background shows). Sixel colour
 * registers map onto palette slots by value -- defining a register never
 * recolours an image already on screen -- so up to 255 distinct colours
 * can be visible at once; past that, new colours take the nearest slot.
 * The palette empties again when no graphic cell is left.
 */
#ifndef TERM_GRAPHICS_H
#define TERM_GRAPHICS_H

#include <stddef.h>
#include <stdint.h>

#include "sixel.h"

#define GRAPHICS_MAX_TILES	16384	/* fits cell.glyph; bitmap is 2 KB */
#define GRAPHICS_NO_SLOT	0xffff

struct term;

/* One six-pixel band of an image being dithered (graphics.c). */
struct dither {
	unsigned	 width;		/* image pixels across (to the edge) */
	unsigned	 y;		/* top pixel row of the band held */
	bool		 dirty;
	uint16_t	*band;		/* 6 x width: register + 1, 0 unpainted */
	int16_t		(*err_this)[3];	/* width + 2: error onto this row */
	int16_t		(*err_next)[3];	/* width + 2: ... and the next */
};

/* Bytes the heap can still hand out (board-specific). */
typedef size_t (*graphics_heap_fn)(void);

struct graphics {
	uint8_t		*pool;		/* tile pixels (malloc'd), or null */
	size_t		 pool_bytes;
	size_t		 budget;	/* most the pool may take; 0 = off */
	graphics_heap_fn heap_free;
	unsigned	 tile_width;	/* = font cell */
	unsigned	 tile_height;
	unsigned	 tile_bytes;
	unsigned	 tile_row_bytes;
	unsigned	 bpp;		/* 8, 4, 2 or 1 (term options) */
	unsigned	 transparent;	/* all-ones index at that depth */
	unsigned	 tiles;		/* capacity, <= GRAPHICS_MAX_TILES */
	unsigned	 in_use;
	unsigned	 next;		/* allocation scan position */
	uint8_t		 used[GRAPHICS_MAX_TILES / 8];

	uint16_t	*palette;	/* 256 x RGB565 (graphics.c: in scratch
					   RAM, for the renderer) */
	unsigned	 palette_used;
	uint16_t	 reg_rgb[SIXEL_REGISTERS];	/* register -> RGB565 */
	uint16_t	 reg_slot[SIXEL_REGISTERS];	/* -> slot, lazily */

	/* Below 8 bits a pixel: a fixed palette, and dithering into it. */
	uint8_t		 fixed_rgb[16][3];
	unsigned	 fixed_count;	/* candidates (1 bit: ink + bg) */
	struct dither	*dither;	/* while an image arrives, if memory */
	void		(*keepalive)(void);	/* during long work: feed the
						   watchdog (set by the app) */

	/* The image being received. */
	struct sixel_decoder decoder;
	bool		 receiving;
	int		 origin_row;	/* top-left cell; < 0 once scrolled */
	unsigned	 origin_col;
};

/*
 * The pool is malloc'd and is the last big thing on the heap, so it is
 * given back before anything that rebuilds the grids (a font switch,
 * DECCOLM) and taken again afterwards: that way it can use whatever the
 * current grids leave over without starving the rebuild. Its size is
 * min(budget, free heap - reserve), the reserve covering what may still
 * be allocated while it's held: SET-UP's own grid and a DECCRA copy
 * (one screen of cells each) plus a margin.
 */
void	graphics_configure(struct term *term, size_t budget,
	    graphics_heap_fn heap_free);
void	graphics_release(struct term *term);
void	graphics_acquire(struct term *term);
/* Drop every tile and re-slice the pool for the current font. */
void	graphics_reset(struct term *term);
/* DCS "P1;P2;P3 q": stream one fragment of Sixel data. */
void	graphics_sixel(struct term *term, const char *params, size_t len,
	    const char *data, size_t data_len, bool initial, bool final);
/* Tiles currently referenced (after a sweep). For tests and stats. */
unsigned graphics_tiles_in_use(struct term *term);

#endif /* TERM_GRAPHICS_H */

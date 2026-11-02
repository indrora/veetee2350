/*
 * graphics.c — Sixel images as graphic cells. See graphics.h for the
 * storage model; this file is the tile pool, the palette, and the sink
 * that turns decoded sixels into tile pixels.
 *
 * Placement follows xterm (which follows the VT340):
 *
 *  - The image's top-left is the top-left of the cursor cell, or of the
 *    screen with DECSDM (mode 80, "sixel display mode") set.
 *  - With sixel scrolling (DECSDM reset, the default) an image taller
 *    than the room below the cursor scrolls the whole screen up as its
 *    bands arrive, and afterwards the cursor sits at the image's left
 *    column on the line below it -- or, with mode 8452, just right of
 *    the image on its last line. With DECSDM set the image is clipped
 *    at the bottom and the cursor doesn't move.
 *  - Pixels past the right edge are clipped.
 *  - With mode 1070 (private colour registers, set by default as in
 *    xterm) every image starts from the VT340's default registers;
 *    reset, register definitions carry over between images.
 *
 * Simplification: pixels an image never paints stay transparent (the
 * text cell's background shows), whatever P2 asks for. P2=1 means
 * exactly that; for P2=0/2 DEC fills them with colour register 0, which
 * for nearly all images is the terminal background anyway.
 */
#include "graphics.h"
#include "hot.h"
#include "term.h"
#include "vterm_internal.h"

#include <stdlib.h>
#include <string.h>

#define TILE_NONE	UINT32_MAX

/*
 * The image palette, read by the renderer for every picture pixel. In
 * main SRAM those reads queue behind core 1's DVI encoder (which has
 * bus priority); in scratch RAM they don't: a full-width picture line
 * went from ~9800 to ~ cycles. One terminal, one palette.
 */
static uint16_t palette_store[256] SCRATCH_DATA;

/* term.c: DEC private mode state for the modes kept in its table. */
bool	term_dec_mode(const struct term *term, unsigned mode);

static uint16_t
rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
	return (uint16_t)((red >> 3) << 11 | (green >> 2) << 5 | blue >> 3);
}

/* --- palette --------------------------------------------------------- */

static void
reset_registers(struct graphics *graphics)
{
	for (unsigned reg = 0; reg < SIXEL_REGISTERS; reg++) {
		const uint8_t *rgb = sixel_vt340_palette[reg < 16 ? reg : 0];
		graphics->reg_rgb[reg] = rgb565(rgb[0], rgb[1], rgb[2]);
		graphics->reg_slot[reg] = GRAPHICS_NO_SLOT;
	}
}

static void
reset_palette(struct graphics *graphics)
{
	graphics->palette_used = 0;
	for (unsigned reg = 0; reg < SIXEL_REGISTERS; reg++)
		graphics->reg_slot[reg] = GRAPHICS_NO_SLOT;
}

/* Distance between two RGB565 colours, channels scaled to 6 bits. */
static unsigned
colour_distance(uint16_t a, uint16_t b)
{
	int red = ((a >> 11) - (b >> 11)) * 2;
	int green = ((a >> 5) & 63) - ((b >> 5) & 63);
	int blue = ((a & 31) - (b & 31)) * 2;

	return (unsigned)(red * red + green * green + blue * blue);
}

/* The palette slot showing `colour`: existing, new, or nearest. */
static unsigned
palette_slot(struct graphics *graphics, uint16_t colour)
{
	unsigned best = 0, best_distance = UINT32_MAX;

	for (unsigned slot = 0; slot < graphics->palette_used; slot++) {
		unsigned distance = colour_distance(graphics->palette[slot],
		    colour);
		if (distance < best_distance) {
			best = slot;
			best_distance = distance;
		}
		if (distance == 0)
			return slot;
	}
	/* At low depths most colours land on their nearest: 15 at 4 bpp. */
	if (graphics->palette_used < graphics->transparent) {
		graphics->palette[graphics->palette_used] = colour;
		return graphics->palette_used++;
	}
	return best;
}

static unsigned
register_slot(struct graphics *graphics, unsigned reg)
{
	if (graphics->reg_slot[reg] == GRAPHICS_NO_SLOT)
		graphics->reg_slot[reg] = (uint16_t)palette_slot(graphics,
		    graphics->reg_rgb[reg]);
	return graphics->reg_slot[reg];
}

/* --- tile pool ------------------------------------------------------- */

static void
mark_grid(struct graphics *graphics, const struct grid *grid)
{
	if (grid->cells == nullptr)
		return;
	for (unsigned row = 0; row < grid->rows; row++) {
		const struct cell *cells = grid_row((struct grid *)grid, row);
		for (unsigned col = 0; col < grid->cols; col++) {
			if (!(cells[col].attr & CELL_GRAPHIC))
				continue;
			unsigned tile = cells[col].glyph;
			uint8_t bit = (uint8_t)(1u << (tile & 7));
			if (tile < graphics->tiles &&
			    !(graphics->used[tile >> 3] & bit)) {
				graphics->used[tile >> 3] |= bit;
				graphics->in_use++;
			}
		}
	}
}

/* Keep the tiles some cell on either screen points at; free the rest. */
static void
sweep(struct term *term)
{
	struct graphics *graphics = &term->graphics;

	memset(graphics->used, 0, sizeof(graphics->used));
	graphics->in_use = 0;
	mark_grid(graphics, &term->primary);
	mark_grid(graphics, &term->alternate);
}

static uint32_t
alloc_tile(struct term *term)
{
	struct graphics *graphics = &term->graphics;

	if (graphics->in_use >= graphics->tiles)
		sweep(term);
	if (graphics->in_use >= graphics->tiles)
		return TILE_NONE;
	for (unsigned n = 0; n < graphics->tiles; n++) {
		unsigned tile = (graphics->next + n) % graphics->tiles;
		uint8_t bit = (uint8_t)(1u << (tile & 7));
		if (!(graphics->used[tile >> 3] & bit)) {
			graphics->used[tile >> 3] |= bit;
			graphics->in_use++;
			graphics->next = tile + 1;
			return tile;
		}
	}
	return TILE_NONE;
}

/*
 * The tile behind cell (row, col) of the current screen, turning a text
 * cell into a transparent graphic cell first. An image drawn over an
 * earlier one reuses its tiles, so unpainted pixels keep the old picture
 * (as on a VT340) and an animation doesn't eat the pool.
 */
static uint8_t *
cell_tile(struct term *term, unsigned row, unsigned col)
{
	struct graphics *graphics = &term->graphics;
	struct cell *cell = &grid_row(term->screen, row)[col];
	uint32_t tile;

	if (cell->attr & CELL_GRAPHIC)
		return graphics->pool + (size_t)cell->glyph *
		    graphics->tile_bytes;
	tile = alloc_tile(term);
	if (tile == TILE_NONE)
		return nullptr;
	uint8_t *pixels = graphics->pool + (size_t)tile * graphics->tile_bytes;
	memset(pixels, 0xff, graphics->tile_bytes);	/* all transparent */
	cell->glyph = (uint16_t)tile;
	cell->attr = CELL_GRAPHIC;	/* fg/bg kept: bg shows under the image */
	return pixels;
}

/* --- sixel sink ------------------------------------------------------ */

static void
sink_define(void *ctx, unsigned reg, uint8_t red, uint8_t green,
    uint8_t blue)
{
	struct graphics *graphics = &((struct term *)ctx)->graphics;

	graphics->reg_rgb[reg] = rgb565(red, green, blue);
	graphics->reg_slot[reg] = GRAPHICS_NO_SLOT;
}

/* Scroll the whole screen up `count` lines, as sixel scrolling does. */
static void
scroll_screen(struct term *term, unsigned count)
{
	struct cell blank = grid_make_cell(term->space_glyph,
	    term->default_fg, term->default_bg, CELL_ERASED);

	grid_scroll_up(term->screen, 0, term->screen->rows, count, blank);
	term->graphics.origin_row -= (int)count;
}

static void
sink_paint(void *ctx, unsigned x, unsigned y, uint8_t bits, unsigned count,
    unsigned reg)
{
	struct term *term = ctx;
	struct graphics *graphics = &term->graphics;
	struct grid *grid = term->screen;
	const unsigned tile_width = graphics->tile_width;
	const unsigned tile_height = graphics->tile_height;
	uint8_t slot = (uint8_t)register_slot(graphics, reg);

	for (unsigned bit = 0; bit < 6; bit++) {
		if (!(bits & (1u << bit)))
			continue;
		unsigned py = y + bit;
		int row = graphics->origin_row + (int)(py / tile_height);
		unsigned sub = py % tile_height;

		if (row >= (int)grid->rows) {
			if (term_dec_mode(term, 80))	/* DECSDM: clip */
				return;
			scroll_screen(term, (unsigned)row - grid->rows + 1);
			row = grid->rows - 1;
		}
		if (row < 0)
			continue;	/* scrolled off the top */

		/* Walk the run cell by cell. */
		unsigned px = x, end = x + count;
		while (px < end) {
			unsigned col = graphics->origin_col + px / tile_width;
			unsigned first = px % tile_width;
			unsigned span = tile_width - first;
			if (span > end - px)
				span = end - px;
			if (col >= grid->cols)
				break;		/* clipped at the right */
			uint8_t *tile = cell_tile(term, (unsigned)row, col);
			if (tile == nullptr)
				return;		/* pool exhausted */
			uint8_t *line = tile + sub * graphics->tile_row_bytes;
			if (graphics->bpp == 8) {
				memset(line + first, slot, span);
			} else {
				unsigned bpp = graphics->bpp;
				unsigned mask = (1u << bpp) - 1;
				for (unsigned i = first; i < first + span; i++) {
					unsigned bit = i * bpp;
					uint8_t *byte = &line[bit >> 3];
					*byte = (uint8_t)((*byte & ~(mask <<
					    (bit & 7))) | (slot << (bit & 7)));
				}
			}
			px += span;
		}
	}
}

static const struct sixel_sink sink = {
	.define = sink_define,
	.paint = sink_paint,
	.raster = nullptr,
};

/* --- public (to term.c) ---------------------------------------------- */

#define POOL_MARGIN	(32u * 1024u)	/* libvterm, font loads, slack */
#define POOL_MIN	(8u * 1024u)	/* smaller isn't worth having */

void
graphics_configure(struct term *term, size_t budget,
    graphics_heap_fn heap_free)
{
	graphics_release(term);
	term->graphics.budget = budget;
	term->graphics.heap_free = heap_free;
	graphics_acquire(term);
}

/* Give the pool back; graphic cells left in the grids are cleared. */
void
graphics_release(struct term *term)
{
	struct graphics *graphics = &term->graphics;
	struct grid *grids[2] = {&term->primary, &term->alternate};
	struct cell blank = grid_make_cell(term->space_glyph,
	    term->default_fg, term->default_bg, CELL_ERASED);

	if (graphics->pool == nullptr)
		return;
	for (unsigned i = 0; i < 2; i++) {
		struct grid *grid = grids[i];
		if (grid->cells == nullptr)
			continue;
		for (unsigned row = 0; row < grid->rows; row++) {
			struct cell *cells = grid_row(grid, row);
			for (unsigned col = 0; col < grid->cols; col++)
				if (cells[col].attr & CELL_GRAPHIC)
					cells[col] = blank;
		}
	}
	if (term->render != nullptr)
		term->render->tiles = nullptr;
	free(graphics->pool);
	graphics->pool = nullptr;
	graphics->pool_bytes = 0;
	graphics_reset(term);
}

void
graphics_acquire(struct term *term)
{
	struct graphics *graphics = &term->graphics;
	size_t reserve = (size_t)term->screen->cols * term->screen->rows *
	    sizeof(struct cell) + POOL_MARGIN;
	size_t avail, bytes;

	if (graphics->pool != nullptr || graphics->budget == 0 ||
	    graphics->heap_free == nullptr)
		return;
	avail = graphics->heap_free();
	if (avail < reserve + POOL_MIN)
		return;
	bytes = avail - reserve;
	if (bytes > graphics->budget)
		bytes = graphics->budget;
	/* No more than the tile cap can use (it bites at low depths). */
	unsigned bpp = term->options.sixel_bpp ? term->options.sixel_bpp : 8;
	size_t tile = (size_t)(term->font.width * bpp + 7) / 8 *
	    term->font.height;
	if (tile > 0 && bytes > GRAPHICS_MAX_TILES * tile)
		bytes = GRAPHICS_MAX_TILES * tile;
	/*
	 * Free bytes aren't one block: a font loaded while the old pool was
	 * held, say, can sit in the middle of its old space. Step down until
	 * a block that size exists.
	 */
	while (bytes >= POOL_MIN &&
	    (graphics->pool = malloc(bytes)) == nullptr)
		bytes -= bytes / 8;
	graphics->pool_bytes = graphics->pool != nullptr ? bytes : 0;
	graphics_reset(term);
}

void
graphics_reset(struct term *term)
{
	struct graphics *graphics = &term->graphics;
	struct render *render = term->render;

	graphics->palette = palette_store;
	graphics->bpp = term->options.sixel_bpp;
	if (graphics->bpp != 1 && graphics->bpp != 2 && graphics->bpp != 4)
		graphics->bpp = 8;
	graphics->transparent = (1u << graphics->bpp) - 1;
	graphics->tile_width = term->font.width;
	graphics->tile_height = term->font.height;
	graphics->tile_row_bytes = (graphics->tile_width * graphics->bpp +
	    7) / 8;
	graphics->tile_bytes = graphics->tile_row_bytes * graphics->tile_height;
	graphics->tiles = graphics->tile_bytes == 0 ? 0 :
	    (unsigned)(graphics->pool_bytes / graphics->tile_bytes);
	if (graphics->tiles > GRAPHICS_MAX_TILES)
		graphics->tiles = GRAPHICS_MAX_TILES;
	graphics->receiving = false;
	reset_registers(graphics);
	reset_palette(graphics);
	sweep(term);	/* a DECCOLM regrid may have kept graphic cells */
	if (render != nullptr) {
		render->tiles = graphics->pool;
		render->palette = graphics->palette;
		render->tile_bytes = (uint16_t)graphics->tile_bytes;
		render->tile_row_bytes = (uint8_t)graphics->tile_row_bytes;
		render->tile_bpp = (uint8_t)graphics->bpp;
	}
}

unsigned
graphics_tiles_in_use(struct term *term)
{
	sweep(term);
	return term->graphics.in_use;
}

static void
begin_image(struct term *term)
{
	struct graphics *graphics = &term->graphics;
	VTermState *state = term->state;

	sweep(term);
	if (graphics->in_use == 0)
		graphics->palette_used = 0;	/* nothing on screen uses it */
	/* Private registers: start from the defaults. Shared: keep the
	 * definitions, but re-resolve slots (the palette may be new). */
	if (term_dec_mode(term, 1070))
		reset_registers(graphics);
	else
		for (unsigned reg = 0; reg < SIXEL_REGISTERS; reg++)
			graphics->reg_slot[reg] = GRAPHICS_NO_SLOT;

	if (term_dec_mode(term, 80)) {
		graphics->origin_row = 0;
		graphics->origin_col = 0;
	} else {
		graphics->origin_row = state->pos.row;
		graphics->origin_col = (unsigned)state->pos.col;
	}
	sixel_begin(&graphics->decoder, &sink, term);
	graphics->receiving = true;
}

/* Put the cursor after the image (see the top of the file). */
static void
end_image(struct term *term)
{
	struct graphics *graphics = &term->graphics;
	struct sixel_decoder *decoder = &graphics->decoder;
	VTermState *state = term->state;
	struct grid *grid = term->screen;

	sixel_end(decoder);
	graphics->receiving = false;
	if (term_dec_mode(term, 80) || decoder->height == 0)
		return;

	unsigned text_rows = (decoder->height + graphics->tile_height - 1) /
	    graphics->tile_height;
	int row = graphics->origin_row + (int)text_rows - 1;
	unsigned col = graphics->origin_col;

	if (term_dec_mode(term, 8452)) {
		col += (decoder->width + graphics->tile_width - 1) /
		    graphics->tile_width;
	} else {
		row++;
	}
	if (row >= (int)grid->rows) {
		scroll_screen(term, (unsigned)row - grid->rows + 1);
		row = grid->rows - 1;
	}
	if (row < 0)
		row = 0;
	if (col >= grid->cols)
		col = grid->cols - 1u;
	state->pos.row = row;
	state->pos.col = (int)col;
	state->at_phantom = 0;
	grid->cursor_row = (uint16_t)row;
	grid->cursor_col = (uint16_t)col;
}

void
graphics_sixel(struct term *term, const char *params, size_t len,
    const char *data, size_t data_len, bool initial, bool final)
{
	(void)params;
	(void)len;
	if (term->graphics.tiles == 0)
		return;		/* no pool: swallow the image */
	if (initial)
		begin_image(term);
	if (!term->graphics.receiving)
		return;
	sixel_feed(&term->graphics.decoder, data, data_len);
	if (final)
		end_image(term);
}

/*
 * render.c — scanline renderer for the cell grid.
 *
 * Cost model (core 1, 252 MHz): the DVI encoder leaves roughly 2900
 * cycles per line for rendering (docs/hardware.md), which at 80 columns is
 * ~36 cycles per 8-pixel cell. The inner loop is built around that:
 *
 *   pixel pair (p0, p1) = bg2 ^ (diff2 & mask[bits])
 *
 * where bg2/diff2 are the cell's bg and (fg ^ bg) RGB565 in both halves
 * (built per cell from its plain RGB565 colours: a few ALU ops) and mask[] is a constant table of {0, 0xffff} halfword patterns indexed
 * by glyph bits. The table is indexed by a whole glyph byte (256 entries
 * x 4 words = 4 KB per polarity), so an 8-pixel cell is one address
 * calculation, one 4-word load, eight ALU ops and one 4-word store.
 * (A first version used a 16-entry nibble table: smaller, but ~40 cycles
 * per cell, which overran the line budget at 80 columns.)
 * RGB565 pixels are little-endian halfwords, so the LEFT pixel of a pair
 * is the LOW halfword.
 *
 * Reverse video (DECSCNM) and a block cursor both swap fg and bg, which is
 * the same as inverting the glyph bits; for the whole screen we do that by
 * switching to an inverted mask table, so it costs nothing per cell.
 */
#include "render.h"

#include <string.h>

#include "hot.h"

/* The two colour words emit8() wants, from a cell's fg/bg. */
#define CELL_BG2(cell)		grid_dup565((cell)->bg)
#define CELL_DIFF2(cell)	grid_dup565((uint16_t)((cell)->fg ^ (cell)->bg))

/*
 * byte_masks[invert][glyph byte] = four words covering 8 pixels, built at
 * init into RAM (8 KB; core 1 reads one entry per cell). 16-byte aligned
 * so an entry never straddles anything awkward for ldm.
 */
static uint32_t byte_masks[2][256][4] __attribute__((aligned(16)));

/*
 * Pixel-doubling table for double-width lines: byte b -> 16 bits where
 * each bit of b is repeated (b7 b7 b6 b6 ...). Built once at init.
 */
static uint16_t double_bits[256];

static void
build_mask_tables(void)
{
	for (unsigned byte = 0; byte < 256; byte++) {
		uint16_t wide = 0;
		for (unsigned bit = 0; bit < 8; bit++)
			if (byte & (1u << bit))
				wide |= (uint16_t)(3u << (bit * 2));
		double_bits[byte] = wide;
	}

	for (unsigned invert = 0; invert < 2; invert++) {
		for (unsigned byte = 0; byte < 256; byte++) {
			unsigned bits = invert ? ~byte & 0xff : byte;

			/* Bit 7 is the leftmost pixel; pixel pairs fill
			 * one word each, left pixel in the low half. */
			for (unsigned pair = 0; pair < 4; pair++) {
				uint32_t word = 0;
				if (bits & (0x80 >> (pair * 2)))
					word |= 0x0000ffff;
				if (bits & (0x40 >> (pair * 2)))
					word |= 0xffff0000;
				byte_masks[invert][byte][pair] = word;
			}
		}
	}
}

void
render_set_screen(struct render *render, unsigned width, unsigned height)
{
	if (width > RENDER_MAX_WIDTH)
		width = RENDER_MAX_WIDTH;
	if (height > RENDER_MAX_HEIGHT)
		height = RENDER_MAX_HEIGHT;
	render->width = (uint16_t)width;
	render->height = (uint16_t)height;
}

void
render_grid_size(const struct font *font, unsigned width, unsigned height,
    unsigned *cols, unsigned *rows)
{
	*cols = width / font->width;
	*rows = height / font->height;
	if (*cols > GRID_MAX_COLS)
		*cols = GRID_MAX_COLS;
	if (*rows > GRID_MAX_ROWS)
		*rows = GRID_MAX_ROWS;
}

void
render_init(struct render *render, const struct font *font, struct grid *grid)
{
	unsigned used_height = (unsigned)grid->rows * font->height;

	build_mask_tables();
	render->font = font;
	render->grid = grid;
	render->x_offset = (uint16_t)(((render->width -
	    (unsigned)grid->cols * font->width) / 2) & ~1u);
	render->y_offset = (uint16_t)((render->height - used_height) / 2);
	/* Strike through the middle of lowercase letters, about 3/5 of the
	 * way up to the baseline; overline on the top row. */
	render->strike_row = (uint8_t)(font->baseline * 3 / 5);
	render->overline_row = 0;

	uint32_t full = (1u << (font->row_bytes * 8)) - 1;
	for (unsigned sub = 0; sub < font->height; sub++) {
		for (unsigned attr = 0; attr < RENDER_ATTR_COMBOS; attr++) {
			uint32_t keep = full;
			uint32_t set = 0;

			if (attr & (CELL_INVISIBLE | CELL_BLINK))
				keep = 0;
			if (((attr & CELL_UNDERLINE) &&
			    sub == font->underline_row) ||
			    ((attr & CELL_STRIKE) && sub == render->strike_row) ||
			    ((attr & CELL_OVERLINE) &&
			    sub == render->overline_row))
				set = full;
			render->attr_ops[sub][attr] = set << 16 | keep;
		}
	}

	for (unsigned line = 0; line < render->height; line++) {
		if (line < render->y_offset ||
		    line >= render->y_offset + used_height) {
			render->line_row[line] = RENDER_LINE_BORDER;
			render->line_sub[line] = 0;
		} else {
			unsigned inside = line - render->y_offset;
			render->line_row[line] = (uint8_t)(inside / font->height);
			render->line_sub[line] = (uint8_t)(inside % font->height);
		}
	}
	render->active = true;
}

void
render_pause(struct render *render)
{
	uint32_t frame = render->frame;

	render->active = false;
	if (!render->live)
		return;
	/*
	 * A scanline that read `active` before we cleared it may still be in
	 * flight. Once the frame counter moves, the renderer has started a
	 * fresh line after our store, so nothing references the old
	 * font/grid. (The renderer runs in a core 0 IRQ, which preempts this
	 * loop, so this must not be called with interrupts disabled.)
	 */
	while (render->frame == frame)
		;
}

static void
HOT_FUNC(fill_words)(uint32_t *out, unsigned words, uint32_t value)
{
	for (unsigned i = 0; i < words; i++)
		out[i] = value;
}

/*
 * Apply text attributes to one cell row's glyph bits via the per-row
 * (and, or) table built in render_init(): bits = (bits & and) | or. Inline
 * and branch-free, so a cell with attributes costs a few cycles more than
 * one without. (An out-of-line version cost ~35 cycles per attributed
 * cell, ~700 cycles/line on a screen with every 4th cell underlined.)
 */
HOT_INLINE uint32_t
attr_bits(const struct render *render, uint32_t bits, unsigned attr,
    unsigned sub, unsigned attr_mask)
{
	uint32_t op = render->attr_ops[sub][attr & attr_mask];

	return (bits & op) | (op >> 16);
}

/* Blink is implemented by masking CELL_BLINK out of the attr index during
 * the visible half of the cycle; the table entries assume "hidden". */
HOT_INLINE unsigned
attr_mask_for(const struct render *render)
{
	bool blink_off = (render->frame >> 5) & 1;

	return blink_off ? RENDER_ATTR_COMBOS - 1 :
	    (RENDER_ATTR_COMBOS - 1) & ~(unsigned)CELL_BLINK;
}

HOT_INLINE uint32_t
read_bits(const uint8_t *row, unsigned row_bytes)
{
	return row_bytes == 1 ? row[0] : (uint32_t)row[0] << 8 | row[1];
}

/* Write one 8-pixel group (bits 7..0, MSB leftmost) as four words. */
HOT_INLINE void
emit8(uint32_t *out, uint32_t bits, uint32_t bg2, uint32_t diff2,
    const uint32_t (*masks)[4])
{
	const uint32_t *mask = masks[bits & 0xff];
	uint32_t mask0 = mask[0], mask1 = mask[1];
	uint32_t mask2 = mask[2], mask3 = mask[3];

	out[0] = bg2 ^ (diff2 & mask0);
	out[1] = bg2 ^ (diff2 & mask1);
	out[2] = bg2 ^ (diff2 & mask2);
	out[3] = bg2 ^ (diff2 & mask3);
}

/* Cell widths that aren't 8 or 16: one halfword store per pixel. */
static void
HOT_FUNC(emit_generic)(uint16_t *out, uint32_t bits, unsigned width,
    unsigned row_bytes, uint32_t bg2, uint32_t diff2, bool invert)
{
	uint16_t bg = (uint16_t)bg2;
	uint16_t fg = (uint16_t)(bg2 ^ diff2);
	unsigned top_bit = row_bytes * 8 - 1;

	for (unsigned x = 0; x < width; x++) {
		bool ink = (bits >> (top_bit - x)) & 1;
		out[x] = (ink != invert) ? fg : bg;
	}
}

static void
HOT_FUNC(draw_cell)(const struct render *render, uint16_t *out,
    const struct cell *cell, uint32_t bits, bool invert)
{
	const struct font *font = render->font;
	const uint32_t (*masks)[4] = byte_masks[invert];

	if (font->width == 8) {
		emit8((uint32_t *)out, bits, CELL_BG2(cell), CELL_DIFF2(cell),
		    masks);
	} else if (font->width == 16) {
		emit8((uint32_t *)out, bits >> 8, CELL_BG2(cell),
		    CELL_DIFF2(cell), masks);
		emit8((uint32_t *)out + 4, bits, CELL_BG2(cell),
		    CELL_DIFF2(cell), masks);
	} else {
		emit_generic(out, bits, font->width, font->row_bytes,
		    CELL_BG2(cell), CELL_DIFF2(cell), invert);
	}
}

/*
 * Sixel tiles, decoded with plain palette lookups -- no per-pixel
 * compare and branch -- a whole byte at a time, two pixels per 32-bit
 * store. The trick for transparency: the all-ones index of the depth
 * (255 at 8 bits, 15 at 4) is never handed out as a colour, so its
 * palette entry is free, and each cell writes its own background there
 * before decoding. (Two contexts render -- the display interrupt and a
 * screenshot in the main loop -- so in theory one can briefly see the
 * other's cell background in that entry: at worst one transparent
 * pixel of one line in the wrong background, which nobody will see.)
 *
 * The first version tested every pixel for transparency, unpacked bit
 * by bit, and then copied the palette into a table with memcpy, which
 * on this build runs from flash: a full-width picture line cost ~13800
 * cycles against a budget of 8000, and the main loop starved until the
 * watchdog reset the board. Now ~30-40 cycles per cell, all from RAM.
 */
/*
 * One 8-pixel tile row into four pixel-pair words. Reads the row as
 * whole words (rows are 1, 2, 4 or 8 bytes, word-aligned since tiles are
 * 8 x height) and pulls the pixels out with shifts: the pool is in main
 * SRAM, which core 1's DVI encoder has bus priority over, so every load
 * counts. The caller has already put the cell's background into
 * colour[transparent].
 */
HOT_INLINE void
draw_tile8(uint32_t *words, const uint8_t *src, const uint16_t *colour,
    unsigned bpp)
{
	uint32_t w0, w1;

	switch (bpp) {
	case 8:
		memcpy(&w0, src, 4);
		memcpy(&w1, src + 4, 4);
		words[0] = colour[w0 & 0xff] |
		    (uint32_t)colour[(w0 >> 8) & 0xff] << 16;
		words[1] = colour[(w0 >> 16) & 0xff] |
		    (uint32_t)colour[w0 >> 24] << 16;
		words[2] = colour[w1 & 0xff] |
		    (uint32_t)colour[(w1 >> 8) & 0xff] << 16;
		words[3] = colour[(w1 >> 16) & 0xff] |
		    (uint32_t)colour[w1 >> 24] << 16;
		return;
	/* Unrolled: straight-line code, one shift-and-mask per pixel. */
	case 4:
		memcpy(&w0, src, 4);
		words[0] = colour[w0 & 15] | (uint32_t)colour[(w0 >> 4) & 15] << 16;
		words[1] = colour[(w0 >> 8) & 15] |
		    (uint32_t)colour[(w0 >> 12) & 15] << 16;
		words[2] = colour[(w0 >> 16) & 15] |
		    (uint32_t)colour[(w0 >> 20) & 15] << 16;
		words[3] = colour[(w0 >> 24) & 15] |
		    (uint32_t)colour[w0 >> 28] << 16;
		return;
	case 2:
		w0 = (uint32_t)src[0] | (uint32_t)src[1] << 8;
		words[0] = colour[w0 & 3] | (uint32_t)colour[(w0 >> 2) & 3] << 16;
		words[1] = colour[(w0 >> 4) & 3] |
		    (uint32_t)colour[(w0 >> 6) & 3] << 16;
		words[2] = colour[(w0 >> 8) & 3] |
		    (uint32_t)colour[(w0 >> 10) & 3] << 16;
		words[3] = colour[(w0 >> 12) & 3] |
		    (uint32_t)colour[(w0 >> 14) & 3] << 16;
		return;
	default:	/* 1 */
		w0 = src[0];
		words[0] = colour[w0 & 1] | (uint32_t)colour[(w0 >> 1) & 1] << 16;
		words[1] = colour[(w0 >> 2) & 1] |
		    (uint32_t)colour[(w0 >> 3) & 1] << 16;
		words[2] = colour[(w0 >> 4) & 1] |
		    (uint32_t)colour[(w0 >> 5) & 1] << 16;
		words[3] = colour[(w0 >> 6) & 1] |
		    (uint32_t)colour[w0 >> 7] << 16;
		return;
	}
}

static void
HOT_FUNC(draw_tile)(const struct render *render, uint16_t *out,
    const struct cell *cell, unsigned sub, unsigned width)
{
	const uint8_t *src = render->tiles +
	    (size_t)cell->glyph * render->tile_bytes +
	    sub * render->tile_row_bytes;
	const unsigned bpp = render->tile_bpp;
	const unsigned mask = (1u << bpp) - 1;	/* also "transparent" */
	uint16_t *colour = render->palette;

	colour[mask] = cell->bg;		/* see-through: this cell's bg */

	if (width == 8 && ((uintptr_t)out & 3) == 0 &&
	    ((uintptr_t)src & 3) == 0) {
		draw_tile8((uint32_t *)out, src, colour, bpp);
		return;
	}
	/* Other cell widths: pixel x is bits (x * bpp) % 8 up of byte x * bpp / 8. */
	for (unsigned x = 0; x < width; x++) {
		unsigned bit = x * bpp;
		out[x] = colour[(src[bit >> 3] >> (bit & 7)) & mask];
	}
}

static void
HOT_FUNC(render_double_line)(struct render *render, uint16_t *out,
    const struct cell *cells, unsigned sub, uint8_t line_size, bool invert,
    unsigned attr_mask, unsigned text_row, bool blink_off)
{
	const struct font *font = render->font;
	struct grid *grid = render->grid;
	const unsigned half = grid->cols / 2;
	unsigned glyph_sub = sub;

	if (line_size == ROW_DOUBLE_TOP)
		glyph_sub = sub / 2;
	else if (line_size == ROW_DOUBLE_BOTTOM)
		glyph_sub = (sub + font->height) / 2;
	const uint8_t *glyph_rows = font->glyphs + glyph_sub;

	bool cursor_here = grid->cursor_visible &&
	    grid->cursor_row == text_row && grid->cursor_col < half &&
	    !(grid->cursor_blink && blink_off);

	for (unsigned col = 0; col < half; col++) {
		const struct cell *cell = &cells[col];
		/* Graphics on a double-size line: not drawn (bg only). */
		uint32_t bits = (cell->attr & CELL_GRAPHIC) ? 0 :
		    glyph_rows[cell->glyph * font->glyph_bytes];
		bool cell_invert = invert;

		if (cell->attr & CELL_ATTR_ALL)
			bits = attr_bits(render, bits, cell->attr, glyph_sub,
			    attr_mask);
		if (cursor_here && col == grid->cursor_col) {
			if (grid->cursor_shape == CURSOR_UNDERLINE)
				bits = sub >= font->height - 2u ? 0xff : bits;
			else if (grid->cursor_shape == CURSOR_BAR)
				bits |= 0x80;
			else
				cell_invert = !invert;
		}
		uint16_t wide = double_bits[bits & 0xff];
		const uint32_t (*masks)[4] = byte_masks[cell_invert];
		emit8((uint32_t *)(out + col * 16), wide >> 8, CELL_BG2(cell),
		    CELL_DIFF2(cell), masks);
		emit8((uint32_t *)(out + col * 16 + 8), wide & 0xff,
		    CELL_BG2(cell), CELL_DIFF2(cell), masks);
	}
	/* Odd column count: the last 8 px have no cell; use the border. */
	for (unsigned x = half * 16; x < grid->cols * 8u; x++)
		out[x] = (uint16_t)grid->border2;
}

void
HOT_FUNC(render_scanline)(uint16_t *pixels, unsigned line, void *ctx)
{
	struct render *render = ctx;
	struct grid *grid = render->grid;

	if (line == 0)
		render->frame = render->frame + 1;

	unsigned text_row = render->line_row[line];
	if (!render->active || text_row == RENDER_LINE_BORDER) {
		fill_words((uint32_t *)pixels, render->width / 2u,
		    render->active ? grid->border2 : 0);
		return;
	}

	const struct font *font = render->font;
	const unsigned sub = render->line_sub[line];
	const unsigned width = font->width;
	const unsigned row_bytes = font->row_bytes;
	const unsigned glyph_bytes = font->glyph_bytes;
	const uint8_t *glyph_rows = font->glyphs + sub * row_bytes;
	const struct cell *cells = grid_row(grid, text_row);
	const unsigned cols = grid->cols;
	const bool invert = grid->reverse_video;
	const bool blink_off = (render->frame >> 5) & 1;
	const unsigned attr_mask = attr_mask_for(render);
	uint16_t *out = pixels + render->x_offset;

	/* Side borders (zero width when the font divides 640 evenly). */
	fill_words((uint32_t *)pixels, render->x_offset / 2, grid->border2);
	unsigned right = render->x_offset + cols * width;
	for (unsigned x = right; x < render->width; x++)
		pixels[x] = (uint16_t)grid->border2;

	/*
	 * Double-width / double-height lines (DECDWL/DECDHL): only the first
	 * cols/2 cells exist, each drawn 16 px wide; double-height halves
	 * stretch glyph rows 0..h/2-1 (top) or h/2..h-1 (bottom) over the
	 * whole cell. Slow path, 8-px fonts only (all built-ins).
	 */
	uint8_t line_size = grid->row_attr[grid->row_map[text_row]];
	if (line_size != ROW_NORMAL && width == 8) {
		render_double_line(render, out, cells, sub, line_size, invert,
		    attr_mask, text_row, blink_off);
		return;
	}

	if (width == 8) {
		/*
		 * Hot loop for the common case. Kept free of calls unless
		 * a cell has attributes, so GCC keeps everything in
		 * registers.
		 */
		const uint32_t (*masks)[4] = byte_masks[invert];
		uint32_t *words = (uint32_t *)out;
		/* Picture cells: per-line state, so the loop only indexes. */
		const uint8_t *tile_rows = render->tiles +
		    sub * render->tile_row_bytes;
		const unsigned tile_bytes = render->tile_bytes;
		const unsigned tile_bpp = render->tile_bpp;
		uint16_t *tile_colour = render->palette;
		for (unsigned col = 0; col < cols; col++, words += 4) {
			const struct cell *cell = &cells[col];
			uint32_t glyph_attr;

			/* glyph (low half) and attr (high half) in one load. */
			memcpy(&glyph_attr, cell, sizeof(glyph_attr));
			uint32_t bits;
			/*
			 * Only text cells may index the font: a picture cell's
			 * "glyph" is a tile number, up to 16383, and reading
			 * glyph_rows that far out ran off the end of RAM
			 * (a bus fault, first seen at 1 bit per pixel).
			 */
			if (glyph_attr & ((CELL_ATTR_ALL | CELL_GRAPHIC) <<
			    16)) {
				if (glyph_attr & (CELL_GRAPHIC << 16)) {
					tile_colour[(1u << tile_bpp) - 1] =
					    cell->bg;	/* see-through */
					draw_tile8(words, tile_rows +
					    (glyph_attr & 0xffff) *
					    tile_bytes, tile_colour,
					    tile_bpp);
					continue;
				}
				bits = attr_bits(render, glyph_rows[
				    (glyph_attr & 0xffff) * glyph_bytes],
				    glyph_attr >> 16, sub, attr_mask);
			} else {
				bits = glyph_rows[(glyph_attr & 0xffff) *
				    glyph_bytes];
			}
			/* fg (low half) and bg (high half) in one load too. */
			uint32_t colours;
			memcpy(&colours, &cell->fg, sizeof(colours));
			uint32_t bg = colours >> 16;
			uint32_t diff = (colours ^ bg) & 0xffff;
			emit8(words, bits, bg | bg << 16, diff | diff << 16,
			    masks);
		}
	} else {
		for (unsigned col = 0; col < cols; col++) {
			const struct cell *cell = &cells[col];
			if (cell->attr & CELL_GRAPHIC) {
				draw_tile(render, out + col * width,
				    cell, sub,
				    width);
				continue;
			}
			uint32_t bits = read_bits(glyph_rows +
			    cell->glyph * glyph_bytes, row_bytes);

			if (cell->attr & CELL_ATTR_ALL)
				bits = attr_bits(render, bits, cell->attr, sub,
				    attr_mask);
			draw_cell(render, out + col * width, cell, bits, invert);
		}
	}

	/*
	 * Cursor: redraw its cell with modified bits. Block = inverted
	 * colours; underline = solid underline row; bar = left column.
	 */
	unsigned cursor_col = grid->cursor_col;
	if (grid->cursor_visible && grid->cursor_row == text_row &&
	    cursor_col < cols && !(grid->cursor_blink && blink_off)) {
		const struct cell *cell = &cells[cursor_col];
		uint32_t bits = (cell->attr & CELL_GRAPHIC) ? 0 :
		    read_bits(glyph_rows + cell->glyph * glyph_bytes,
		    row_bytes);
		uint32_t full = (1u << (row_bytes * 8)) - 1;
		uint32_t leftmost = 1u << (row_bytes * 8 - 1);
		bool cursor_invert = invert;

		if (cell->attr & CELL_ATTR_ALL)
			bits = attr_bits(render, bits, cell->attr, sub,
			    attr_mask);
		switch (grid->cursor_shape) {
		case CURSOR_UNDERLINE:
			if (sub >= font->height - 2u)
				bits = full;
			break;
		case CURSOR_BAR:
			bits |= leftmost;
			break;
		default:
			cursor_invert = !invert;
			break;
		}
		draw_cell(render, out + cursor_col * width, cell, bits,
		    cursor_invert);
	}
}

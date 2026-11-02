/*
 * render.c — scanline renderer for the cell grid.
 *
 * Cost model (core 1, 252 MHz): the DVI encoder leaves roughly 2900
 * cycles per line for rendering (docs/hardware.md), which at 80 columns is
 * ~36 cycles per 8-pixel cell. The inner loop is built around that:
 *
 *   pixel pair (p0, p1) = bg2 ^ (diff2 & mask[bits])
 *
 * where bg2/diff2 come straight from the cell (precomputed by the writer)
 * and mask[] is a constant table of {0, 0xffff} halfword patterns indexed
 * by glyph bits. With a 16-entry table indexed by a nibble, one table load
 * covers 4 pixels, so an 8-pixel cell is two lookups and four word stores.
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

/*
 * nibble_masks[invert][nibble] = two words covering 4 pixels. Built at
 * init into RAM (core 1 reads it ~160 times per line).
 */
static uint32_t nibble_masks[2][16][2];

static void
build_mask_tables(void)
{
	for (unsigned invert = 0; invert < 2; invert++) {
		for (unsigned nibble = 0; nibble < 16; nibble++) {
			unsigned bits = invert ? ~nibble & 0xf : nibble;
			uint32_t pair0 = 0;
			uint32_t pair1 = 0;

			/* Bit 3 is the leftmost of the four pixels. */
			if (bits & 0x8)
				pair0 |= 0x0000ffff;
			if (bits & 0x4)
				pair0 |= 0xffff0000;
			if (bits & 0x2)
				pair1 |= 0x0000ffff;
			if (bits & 0x1)
				pair1 |= 0xffff0000;
			nibble_masks[invert][nibble][0] = pair0;
			nibble_masks[invert][nibble][1] = pair1;
		}
	}
}

void
render_grid_size(const struct font *font, unsigned *cols, unsigned *rows)
{
	*cols = RENDER_WIDTH / font->width;
	*rows = RENDER_HEIGHT / font->height;
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
	render->x_offset = (uint16_t)(((RENDER_WIDTH -
	    (unsigned)grid->cols * font->width) / 2) & ~1u);
	render->y_offset = (uint16_t)((RENDER_HEIGHT - used_height) / 2);
	/* Strike through the middle of lowercase letters, about 3/5 of the
	 * way up to the baseline; overline on the top row. */
	render->strike_row = (uint8_t)(font->baseline * 3 / 5);
	render->overline_row = 0;

	for (unsigned line = 0; line < RENDER_HEIGHT; line++) {
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
	/*
	 * A scanline that read `active` before we cleared it may still be in
	 * flight. Once the frame counter moves, core 1 has started a fresh
	 * line after our store, so nothing references the old font/grid.
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
 * Glyph bits for one cell row, left-aligned in the low `row_bytes * 8`
 * bits, with text attributes applied. Out of line: most cells have
 * attr == 0 and never call it.
 */
static uint32_t
HOT_FUNC(apply_attr)(const struct render *render, uint32_t bits,
    uint16_t attr, unsigned sub, bool blink_off)
{
	const struct font *font = render->font;
	uint32_t full = (1u << (font->row_bytes * 8)) - 1;

	if ((attr & CELL_INVISIBLE) || ((attr & CELL_BLINK) && blink_off))
		bits = 0;
	if ((attr & CELL_UNDERLINE) && sub == font->underline_row)
		bits = full;
	if ((attr & CELL_STRIKE) && sub == render->strike_row)
		bits = full;
	if ((attr & CELL_OVERLINE) && sub == render->overline_row)
		bits = full;
	return bits;
}

static inline uint32_t
read_bits(const uint8_t *row, unsigned row_bytes)
{
	return row_bytes == 1 ? row[0] : (uint32_t)row[0] << 8 | row[1];
}

/* Write one 8-pixel group (bits 7..0, MSB leftmost) as four words. */
static inline void
emit8(uint32_t *out, uint32_t bits, uint32_t bg2, uint32_t diff2,
    const uint32_t (*masks)[2])
{
	const uint32_t *high = masks[(bits >> 4) & 0xf];
	const uint32_t *low = masks[bits & 0xf];

	out[0] = bg2 ^ (diff2 & high[0]);
	out[1] = bg2 ^ (diff2 & high[1]);
	out[2] = bg2 ^ (diff2 & low[0]);
	out[3] = bg2 ^ (diff2 & low[1]);
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
	const uint32_t (*masks)[2] = nibble_masks[invert];

	if (font->width == 8) {
		emit8((uint32_t *)out, bits, cell->bg2, cell->diff2, masks);
	} else if (font->width == 16) {
		emit8((uint32_t *)out, bits >> 8, cell->bg2, cell->diff2,
		    masks);
		emit8((uint32_t *)out + 4, bits, cell->bg2, cell->diff2,
		    masks);
	} else {
		emit_generic(out, bits, font->width, font->row_bytes,
		    cell->bg2, cell->diff2, invert);
	}
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
		fill_words((uint32_t *)pixels, RENDER_WIDTH / 2,
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
	uint16_t *out = pixels + render->x_offset;

	/* Side borders (zero width when the font divides 640 evenly). */
	fill_words((uint32_t *)pixels, render->x_offset / 2, grid->border2);
	unsigned right = render->x_offset + cols * width;
	for (unsigned x = right; x < RENDER_WIDTH; x++)
		pixels[x] = (uint16_t)grid->border2;

	if (width == 8) {
		/*
		 * Hot loop for the common case. Kept free of calls unless
		 * a cell has attributes, so GCC keeps everything in
		 * registers.
		 */
		const uint32_t (*masks)[2] = nibble_masks[invert];
		uint32_t *words = (uint32_t *)out;

		for (unsigned col = 0; col < cols; col++, words += 4) {
			const struct cell *cell = &cells[col];
			uint32_t bits = glyph_rows[cell->glyph * glyph_bytes];

			if (__builtin_expect(cell->attr != 0, 0))
				bits = apply_attr(render, bits, cell->attr, sub,
				    blink_off);
			emit8(words, bits, cell->bg2, cell->diff2, masks);
		}
	} else {
		for (unsigned col = 0; col < cols; col++) {
			const struct cell *cell = &cells[col];
			uint32_t bits = read_bits(glyph_rows +
			    cell->glyph * glyph_bytes, row_bytes);

			if (cell->attr != 0)
				bits = apply_attr(render, bits, cell->attr, sub,
				    blink_off);
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
		uint32_t bits = read_bits(glyph_rows + cell->glyph *
		    glyph_bytes, row_bytes);
		uint32_t full = (1u << (row_bytes * 8)) - 1;
		uint32_t leftmost = 1u << (row_bytes * 8 - 1);
		bool cursor_invert = invert;

		if (cell->attr != 0)
			bits = apply_attr(render, bits, cell->attr, sub,
			    blink_off);
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

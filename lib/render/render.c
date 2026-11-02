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
	/* The first call: start clean (no overlays, no status line). */
	memset(render, 0, sizeof(*render));
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

/*
 * What a scanline shows when nothing peeks: border, the reserved status
 * row, or a grid row. render_init lays the screen out with this and
 * render_status_peek uses it to put lines back.
 */
static void
base_line(struct render *render, unsigned line)
{
	const unsigned height = render->font->height;
	unsigned row = RENDER_LINE_BORDER, sub = 0;

	if (line >= render->text_top && line < render->text_end) {
		row = (line - render->text_top) / height;
		sub = (line - render->text_top) % height;
	} else if (render->status_place == RENDER_STATUS_TOP &&
	    line < render->text_top && line + height >= render->text_top) {
		row = RENDER_LINE_STATUS;
		sub = line + height - render->text_top;
	} else if (render->status_place == RENDER_STATUS_BOTTOM &&
	    line >= render->text_end && line < render->text_end + height) {
		row = RENDER_LINE_STATUS;
		sub = line - render->text_end;
	}
	render->line_row[line] = (uint8_t)row;
	render->line_sub[line] = (uint8_t)sub;
}

void
render_init(struct render *render, const struct font *font, struct grid *grid)
{
	/* A status row of its own only if there are cells to draw in it. */
	if (render->status_cells == nullptr)
		render->status_place = RENDER_STATUS_NONE;
	const bool status_row = render->status_place != RENDER_STATUS_NONE;
	unsigned used_height = ((unsigned)grid->rows + status_row) *
	    font->height;

	build_mask_tables();
	render->font = font;
	render->grid = grid;
	render->x_offset = (uint16_t)(((render->width -
	    (unsigned)grid->cols * font->width) / 2) & ~1u);
	render->y_offset = (uint16_t)((render->height - used_height) / 2);
	render->text_top = (uint16_t)(render->y_offset +
	    (render->status_place == RENDER_STATUS_TOP ? font->height : 0));
	render->text_end = (uint16_t)(render->text_top +
	    (unsigned)grid->rows * font->height);
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

	for (unsigned line = 0; line < render->height; line++)
		base_line(render, line);
	render->plan = nullptr;		/* new geometry: plan afresh */
	render->active = true;
}

void
render_status_peek(struct render *render, bool top, unsigned lines)
{
	const unsigned height = render->font->height;

	if (render->status_cells == nullptr ||
	    render->text_end < render->text_top + height)
		return;
	if (lines > height)
		lines = height;
	/* The edge row the status line slides over. */
	unsigned first = top ? render->text_top : render->text_end - height;
	for (unsigned line = first; line < first + height; line++)
		base_line(render, line);
	for (unsigned i = 0; i < lines; i++) {
		/* Bottom: rows 0.. from text_end - lines; top: the last ones. */
		unsigned line = top ? render->text_top + i :
		    render->text_end - lines + i;
		render->line_row[line] = RENDER_LINE_STATUS;
		render->line_sub[line] = (uint8_t)(top ? height - lines + i :
		    i);
	}
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

/*
 * GCC turns a loop storing 0 into a call to memset, which lives in
 * flash: from the render IRQ that's an XIP stall, or worse. It did, for
 * the profiler bar's blank lines (found in devlog 024). The empty asm
 * hides the value from the optimiser, so the loop stays a loop. (An
 * optimize("no-tree-loop-distribute-patterns") attribute did too, but
 * stopped fill_words inlining and cost PLAIN rows 7%.)
 */
static void
HOT_FUNC(fill_words)(uint32_t *out, unsigned words, uint32_t value)
{
	__asm__ ("" : "+r" (value));
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

/*
 * emit8 for the first `words` pixel pairs only: 3 for a 6-px cell (the
 * glyph's bits are the top six of its byte, so they're the first three
 * pairs), 2 for the right half of a doubled one. `words` is a constant
 * at every call, so each use compiles to straight stores.
 */
HOT_INLINE void
emit_cell(uint32_t *out, uint32_t bits, uint32_t bg2, uint32_t diff2,
    const uint32_t (*masks)[4], unsigned words)
{
	if (words == 4) {
		/* emit8's shape: all loads first (the interleaved version
		 * cost colourful 8x16 text 5%). */
		emit8(out, bits, bg2, diff2, masks);
		return;
	}
	const uint32_t *mask = masks[bits & 0xff];
	uint32_t mask0 = mask[0], mask1 = mask[1];
	uint32_t mask2 = words > 2 ? mask[2] : 0;

	out[0] = bg2 ^ (diff2 & mask0);
	out[1] = bg2 ^ (diff2 & mask1);
	if (words > 2)
		out[2] = bg2 ^ (diff2 & mask2);
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
draw_tile_cell(uint32_t *words, const uint8_t *src, const uint16_t *colour,
    unsigned bpp, unsigned cell_words)
{
	/*
	 * Pixels are packed LSB-first, so a 6-px cell (cell_words 3) is the
	 * first three pixel pairs of the 8-px expressions; only the reads
	 * differ, so as not to run past a 6-px row: 6, 3, 2 (12 bits) and 1
	 * byte at 8, 4, 2 and 1 bits a pixel.
	 */
	const bool eight = cell_words == 4;
	uint32_t w0, w1;

	switch (bpp) {
	case 8:
		memcpy(&w0, src, 4);
		if (eight)
			memcpy(&w1, src + 4, 4);
		else
			w1 = (uint32_t)src[4] | (uint32_t)src[5] << 8;
		words[0] = colour[w0 & 0xff] |
		    (uint32_t)colour[(w0 >> 8) & 0xff] << 16;
		words[1] = colour[(w0 >> 16) & 0xff] |
		    (uint32_t)colour[w0 >> 24] << 16;
		words[2] = colour[w1 & 0xff] |
		    (uint32_t)colour[(w1 >> 8) & 0xff] << 16;
		if (eight)
			words[3] = colour[(w1 >> 16) & 0xff] |
			    (uint32_t)colour[w1 >> 24] << 16;
		return;
	/* Unrolled: straight-line code, one shift-and-mask per pixel. */
	case 4:
		if (eight)
			memcpy(&w0, src, 4);
		else
			w0 = (uint32_t)src[0] | (uint32_t)src[1] << 8 |
			    (uint32_t)src[2] << 16;
		words[0] = colour[w0 & 15] | (uint32_t)colour[(w0 >> 4) & 15] << 16;
		words[1] = colour[(w0 >> 8) & 15] |
		    (uint32_t)colour[(w0 >> 12) & 15] << 16;
		words[2] = colour[(w0 >> 16) & 15] |
		    (uint32_t)colour[(w0 >> 20) & 15] << 16;
		if (eight)
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
		if (eight)
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
		if (eight)
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

	/* 8- and 6-px cells, word-aligned: whole pixel pairs. (A 6-px tile
	 * row isn't word-aligned in the pool; the M33 loads it anyway.) */
	if ((width == 8 || width == 6) && ((uintptr_t)out & 3) == 0) {
		draw_tile_cell((uint32_t *)out, src, colour, bpp, width / 2);
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
	const unsigned width = font->width;	/* 8 or 6 */
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
		/* 16 px from an 8-px cell; 12 from a 6-px one, whose bits
		 * are the top six, so the last 4 doubled ones are unused. */
		uint32_t *at = (uint32_t *)(out + col * 2u * width);
		emit8(at, wide >> 8, CELL_BG2(cell), CELL_DIFF2(cell), masks);
		if (width == 8)
			emit8(at + 4, wide & 0xff, CELL_BG2(cell),
			    CELL_DIFF2(cell), masks);
		else
			emit_cell(at + 4, wide & 0xff, CELL_BG2(cell),
			    CELL_DIFF2(cell), masks, 2);
	}
	/* Odd column count: the last cell's width has none; border. */
	for (unsigned x = half * 2u * width; x < grid->cols * width; x++)
		out[x] = (uint16_t)grid->border2;
}

/* --- the frame plan (render.h) ------------------------------------------ */

/*
 * The PAIR table: for each glyph byte, its eight pixels as emit8 would
 * write them in the pair's colours. 4 KB, rebuilt only when the pair or
 * reverse video changes.
 */
static uint32_t pair_words[256][4] __attribute__((aligned(16)));

/* A cell's colours as one word, fg | bg << 16 (struct cell's layout). */
static uint32_t
cell_colours(const struct cell *cell)
{
	uint32_t colours;

	memcpy(&colours, &cell->fg, sizeof(colours));
	return colours;
}

static void
build_pair_table(struct render *render, uint32_t pair, bool invert)
{
	struct cell sample = {0};
	uint32_t bg2, diff2;

	memcpy(&sample.fg, &pair, sizeof(pair));
	bg2 = CELL_BG2(&sample);
	diff2 = CELL_DIFF2(&sample);
	/* The scanline mustn't use it half-built (tearing is fine, a
	 * table of two colours mixed is a frame of wrong colours too). */
	render->pair_ready = false;
	for (unsigned byte = 0; byte < 256; byte++)
		emit8(pair_words[byte], byte, bg2, diff2, byte_masks[invert]);
	render->pair = pair;
	render->pair_invert = invert;
	render->pair_ready = true;
}

/*
 * What kind of span `count` cells make (render.h), and a vote towards
 * the next frame's pair: Boyer-Moore majority, one candidate and a
 * count, so a pair on more than half the cells always wins.
 */
struct vote {
	uint32_t	 candidate;
	uint32_t	 count;
};

static uint8_t
span_kind(const struct render *render, const struct cell *cells,
    unsigned count, struct vote *vote)
{
	bool pair = render->pair_ready;

	for (unsigned col = 0; col < count; col++) {
		if (cells[col].attr & (CELL_ATTR_ALL | CELL_GRAPHIC))
			return RENDER_SPAN_MIXED;
		uint32_t colours = cell_colours(&cells[col]);
		if (vote->count == 0)
			vote->candidate = colours;
		vote->count += colours == vote->candidate ? 1 : -1;
		pair = pair && colours == render->pair;
	}
	return pair ? RENDER_SPAN_PAIR : RENDER_SPAN_PLAIN;
}

/*
 * One row: which slot owns each column (0 the grid, slot + 1 an
 * overlay, later slots on top), then runs of owners become spans.
 */
static void
plan_row(const struct render *render, const struct grid *grid,
    unsigned text_row, struct render_row_plan *plan, struct vote *vote)
{
	const unsigned cols = grid->cols;
	const struct render_overlay *over[RENDER_OVERLAYS];
	uint8_t owner[GRID_MAX_COLS];
	bool covered = false;

	for (unsigned slot = 0; slot < RENDER_OVERLAYS; slot++) {
		over[slot] = render->overlay[slot];
		if (over[slot] != nullptr &&
		    text_row - over[slot]->row < (unsigned)over[slot]->rows &&
		    over[slot]->col < cols)
			covered = true;
		else
			over[slot] = nullptr;
	}
	plan->covered = covered;
	if (!covered) {
		plan->spans = 1;
		plan->span[0] = (struct render_span){nullptr, 0, (uint8_t)cols,
		    span_kind(render, grid_row(grid, text_row), cols, vote)};
		return;
	}

	memset(owner, 0, cols);
	for (unsigned slot = 0; slot < RENDER_OVERLAYS; slot++) {
		if (over[slot] == nullptr)
			continue;
		unsigned end = over[slot]->col + over[slot]->cols;
		for (unsigned col = over[slot]->col; col < end && col < cols;
		    col++)
			owner[col] = (uint8_t)(slot + 1);
	}
	plan->spans = 0;
	for (unsigned col = 0; col < cols;) {
		unsigned start = col;
		uint8_t who = owner[col];
		while (col < cols && owner[col] == who)
			col++;
		const struct cell *cells = nullptr;
		if (who != 0) {
			const struct render_overlay *o = over[who - 1];
			cells = o->cells + (text_row - o->row) * o->cols +
			    (start - o->col);
		}
		/* Five is the most two overlays can make; more can't happen,
		 * but stop rather than overrun. */
		if (plan->spans == RENDER_PLAN_SPANS)
			break;
		plan->span[plan->spans++] = (struct render_span){
			cells, (uint16_t)start, (uint8_t)(col - start),
			span_kind(render, cells != nullptr ? cells :
			    grid_row(grid, text_row) + start, col - start,
			    vote),
		};
	}
}

void
render_plan(struct render *render)
{
	const struct grid *grid = render->grid;
	struct render_plan *next = render->plan == &render->plans[0] ?
	    &render->plans[1] : &render->plans[0];
	struct vote vote = {0, 0};

#ifdef VEETEE_NO_FRAME_PLAN
	return;		/* A/B measurement: no plan, the whole-row path */
#endif
	/* Last frame's commonest pair, in this frame's reverse video. */
	if (!render->pair_ready || render->pair != render->pair_next ||
	    render->pair_invert != grid->reverse_video)
		build_pair_table(render, render->pair_next,
		    grid->reverse_video);
	for (unsigned row = 0; row < grid->rows; row++)
		plan_row(render, grid, row, &next->row[row], &vote);
	render->pair_next = vote.candidate;
	/* One pointer store: the next scanline reads the new plan. */
	render->plan = next;
}

/*
 * Overlays (render.h): two scratch rows, one for the render IRQ and one
 * for the main loop -- screenshots call render_scanline too, and the IRQ
 * may interrupt them in the middle of a row.
 */
alignas(8) static struct cell overlay_scratch[2][GRID_MAX_COLS];
/*
 * What each scratch row holds. Composing costs about 2100 cycles a line
 * at 80 columns (measured with VEETEE_RENDER_PROFILE), so a row is
 * composed once per frame and reused for its other glyph lines. A cell
 * that changes mid-row shows a frame late, as in any scanline race.
 */
static const struct cell *overlay_scratch_row[2];
static uint32_t overlay_scratch_frame[2];

HOT_INLINE bool
in_interrupt(void)
{
#if defined(__arm__)
	uint32_t ipsr;
	__asm__ volatile ("mrs %0, ipsr" : "=r" (ipsr));
	return (ipsr & 0x1ff) != 0;
#else
	return false;
#endif
}

/* Cells are two words; memcpy may live in flash, so copy by hand. */
HOT_INLINE void
copy_cells(struct cell *to, const struct cell *from, unsigned count)
{
	uint32_t *dst = (uint32_t *)to;
	const uint32_t *src = (const uint32_t *)from;

	for (unsigned i = 0; i < count * 2; i++)
		dst[i] = src[i];
}

/* Columns of `over` on screen, clipped to the grid (0 if off it). */
HOT_INLINE unsigned
overlay_span(const struct render_overlay *over, const struct grid *grid)
{
	if (over->col >= grid->cols)
		return 0;
	unsigned room = (unsigned)grid->cols - over->col;
	return over->cols < room ? over->cols : room;
}

/*
 * The cells to draw for `text_row`: the grid's own row, or -- if an
 * overlay covers it -- a copy with the overlays' cells laid over, later
 * slots on top. *cursor_covered says whether the cursor's cell is under
 * one. Off the overlays' rows this costs a compare per slot.
 */
HOT_INLINE const struct cell *
compose_row(struct render *render, struct grid *grid,
    unsigned text_row, bool *cursor_covered)
{
	const struct cell *row = grid_row(grid, text_row);
	uint32_t started = HOT_CYCLES();
	bool covered = false;

	for (unsigned slot = 0; slot < RENDER_OVERLAYS; slot++) {
		const struct render_overlay *over = render->overlay[slot];
		if (over == nullptr ||
		    text_row - over->row >= (unsigned)over->rows)
			continue;
		covered = true;
		if (grid->cursor_row == text_row &&
		    (unsigned)(grid->cursor_col - over->col) <
		    overlay_span(over, grid))
			*cursor_covered = true;
	}
	if (!covered)
		return row;

	unsigned which = in_interrupt() ? 0 : 1;
	struct cell *scratch = overlay_scratch[which];
	if (overlay_scratch_row[which] != row ||
	    overlay_scratch_frame[which] != render->frame) {
		copy_cells(scratch, row, grid->cols);
		for (unsigned slot = 0; slot < RENDER_OVERLAYS; slot++) {
			const struct render_overlay *over =
			    render->overlay[slot];
			if (over == nullptr ||
			    text_row - over->row >= (unsigned)over->rows)
				continue;
			copy_cells(scratch + over->col, over->cells +
			    (text_row - over->row) * over->cols,
			    overlay_span(over, grid));
		}
		overlay_scratch_row[which] = row;
		overlay_scratch_frame[which] = render->frame;
	}
	if (HOT_PROFILE) {
		render->profile_compose_cycles += HOT_CYCLES() - started;
		render->profile_compose_lines++;
	}
	return scratch;
}

/* One profiler bar (render.h) across a scanline. */
HOT_INLINE void
draw_meter_bar(const struct render *render, uint16_t *pixels,
    const struct render_meter_bar *bar)
{
	const unsigned width = render->width;
	/* budget at 80%: x = cycles * width * 4 / (budget * 5) */
	const unsigned scale = render->meter.budget * 5u;
	unsigned x = 0;

	if (scale == 0)
		return;
	for (unsigned i = 0; i < bar->count && x < width; i++) {
		unsigned end = x + bar->segment[i].cycles * width * 4u / scale;
		if (end > width)
			end = width;
		for (; x < end; x++)
			pixels[x] = bar->segment[i].colour;
		if (x < width)
			pixels[x++] = 0x0000;		/* tick after it */
	}
	for (; x < width; x++)
		pixels[x] = 0x2104;			/* dark grey: unused */
	unsigned worst = bar->max * width * 4u / scale;
	if (worst < width)
		pixels[worst] = 0xf800;			/* red: worst line */
	unsigned budget = width * 4u / 5u;
	pixels[budget] = 0xffff;			/* white: the budget */
	pixels[budget + 1] = 0xffff;
}

HOT_INLINE void
draw_meter(const struct render *render, uint16_t *pixels, unsigned line)
{
	if (line == 3 || line == 7) {
		fill_words((uint32_t *)pixels, render->width / 2u, 0);
		return;
	}
	draw_meter_bar(render, pixels, &render->meter.bar[line < 3 ? 0 : 1]);
}

/*
 * A line of the status row (render.h): its own cells, plain text in two
 * colours -- no attributes, pictures, cursor, overlays or DECSCNM.
 * noinline keeps it out of render_scanline: inlined there, it cost the
 * text path 231 cycles a line (see there).
 *
 * 8-px fonts get the text path's inline emit8 loop: through draw_cell,
 * a call per cell, the status row's lines took 6747 cycles at 80
 * columns, against 4160 for a line of text (VEETEE_RENDER_PROFILE).
 */
static void __attribute__((noinline))
HOT_FUNC(draw_status_line)(const struct render *render, uint16_t *pixels,
    unsigned sub)
{
	const struct font *font = render->font;
	const struct grid *grid = render->grid;
	const unsigned cols = grid->cols;
	const uint8_t *glyph_rows = font->glyphs + sub * font->row_bytes;
	uint16_t *out = pixels + render->x_offset;

	fill_words((uint32_t *)pixels, render->x_offset / 2, grid->border2);
	for (unsigned x = render->x_offset + cols * font->width;
	    x < render->width; x++)
		pixels[x] = (uint16_t)grid->border2;
	if (font->width == 8 || font->width == 6) {
		const uint32_t (*masks)[4] = byte_masks[0];
		const unsigned glyph_bytes = font->glyph_bytes;
		uint32_t *words = (uint32_t *)out;
		for (unsigned col = 0; col < cols; col++) {
			const struct cell *cell = &render->status_cells[col];
			uint32_t bits = glyph_rows[cell->glyph * glyph_bytes];
			if (font->width == 8) {
				emit8(words, bits, CELL_BG2(cell),
				    CELL_DIFF2(cell), masks);
				words += 4;
			} else {
				emit_cell(words, bits, CELL_BG2(cell),
				    CELL_DIFF2(cell), masks, 3);
				words += 3;
			}
		}
		return;
	}
	for (unsigned col = 0; col < cols; col++) {
		const struct cell *cell = &render->status_cells[col];
		draw_cell(render, out + col * font->width, cell,
		    read_bits(glyph_rows + cell->glyph * font->glyph_bytes,
		    font->row_bytes), false);
	}
}

/*
 * The column loops (render_scanline, for 8-px fonts), one span of a row
 * at a time; `words` is where the span starts in the line.
 */
struct span_context {
	const struct render *render;
	const uint8_t	*glyph_rows;	/* this glyph row of glyph 0 */
	const uint32_t	(*masks)[4];
	unsigned	 glyph_bytes;
	unsigned	 sub;
	unsigned	 attr_mask;
};

/*
 * PLAIN spans: text without attributes, so the loop holds only what
 * text needs. It stops at the first cell that isn't (the plan being a
 * frame behind the grid) and returns how far it got; the caller draws
 * the rest with the full loop, so a stale plan costs speed, never
 * pixels. (A call per such cell instead starved core 1 while a Sixel
 * picture arrived and filled rows planned as text: devlog 024.)
 */
HOT_INLINE unsigned
draw_plain_span(const struct span_context *span, uint32_t *words,
    const struct cell *cells, unsigned count, unsigned cell_words)
{
	const uint8_t *glyph_rows = span->glyph_rows;
	const unsigned glyph_bytes = span->glyph_bytes;
	const uint32_t (*masks)[4] = span->masks;

	for (unsigned col = 0; col < count; col++, words += cell_words) {
		const struct cell *cell = &cells[col];
		uint32_t glyph_attr, colours;

		memcpy(&glyph_attr, cell, sizeof(glyph_attr));
		if (__builtin_expect(glyph_attr & ((CELL_ATTR_ALL |
		    CELL_GRAPHIC) << 16), 0))
			return col;
		uint32_t bits = glyph_rows[(glyph_attr & 0xffff) * glyph_bytes];
		memcpy(&colours, &cell->fg, sizeof(colours));
		uint32_t bg = colours >> 16;
		uint32_t diff = (colours ^ bg) & 0xffff;
		emit_cell(words, bits, bg | bg << 16, diff | diff << 16, masks,
		    cell_words);
	}
	return count;
}

/*
 * PAIR spans: plain text in the table's colours, so each cell is a glyph
 * byte and 16 bytes copied from pair_words. Like the PLAIN loop, it
 * stops at a cell that isn't plain text and says where.
 */
HOT_INLINE unsigned
draw_pair_span(const struct span_context *span, uint32_t *words,
    const struct cell *cells, unsigned count, unsigned cell_words)
{
	const uint8_t *glyph_rows = span->glyph_rows;
	const unsigned glyph_bytes = span->glyph_bytes;

	for (unsigned col = 0; col < count; col++, words += cell_words) {
		uint32_t glyph_attr;

		memcpy(&glyph_attr, &cells[col], sizeof(glyph_attr));
		if (__builtin_expect(glyph_attr & ((CELL_ATTR_ALL |
		    CELL_GRAPHIC) << 16), 0))
			return col;
		const uint32_t *from = pair_words[glyph_rows[(glyph_attr &
		    0xffff) * glyph_bytes]];
		/* All loads, then all stores: block transfers. (Storing the
		 * fourth word after the others cost 8x16 text 18%.) */
		if (cell_words == 4) {
			uint32_t w0 = from[0], w1 = from[1], w2 = from[2],
			    w3 = from[3];
			words[0] = w0;
			words[1] = w1;
			words[2] = w2;
			words[3] = w3;
		} else {
			uint32_t w0 = from[0], w1 = from[1], w2 = from[2];
			words[0] = w0;
			words[1] = w1;
			words[2] = w2;
		}
	}
	return count;
}

/* MIXED spans: attributes and pictures handled in line, as before. */
HOT_INLINE void
draw_mixed_span(const struct span_context *span, uint32_t *words,
    const struct cell *cells, unsigned count, unsigned cell_words)
{
	const struct render *render = span->render;
	const uint8_t *glyph_rows = span->glyph_rows;
	const unsigned glyph_bytes = span->glyph_bytes;
	const uint32_t (*masks)[4] = span->masks;
	const unsigned sub = span->sub;
	const unsigned attr_mask = span->attr_mask;
	/* Picture cells: per-line state, so the loop only indexes. */
	const uint8_t *tile_rows = render->tiles + sub * render->tile_row_bytes;
	const unsigned tile_bytes = render->tile_bytes;
	const unsigned tile_bpp = render->tile_bpp;
	uint16_t *tile_colour = render->palette;

	for (unsigned col = 0; col < count; col++, words += cell_words) {
		const struct cell *cell = &cells[col];
		uint32_t glyph_attr;

		/* glyph (low half) and attr (high half) in one load. */
		memcpy(&glyph_attr, cell, sizeof(glyph_attr));
		uint32_t bits;
		/*
		 * Only text cells may index the font: a picture cell's
		 * "glyph" is a tile number, up to 16383, and reading
		 * glyph_rows that far out ran off the end of RAM (a bus
		 * fault, first seen at 1 bit per pixel).
		 */
		if (glyph_attr & ((CELL_ATTR_ALL | CELL_GRAPHIC) << 16)) {
			if (glyph_attr & (CELL_GRAPHIC << 16)) {
				tile_colour[(1u << tile_bpp) - 1] =
				    cell->bg;	/* see-through */
				draw_tile_cell(words, tile_rows + (glyph_attr &
				    0xffff) * tile_bytes, tile_colour,
				    tile_bpp, cell_words);
				continue;
			}
			bits = attr_bits(render, glyph_rows[(glyph_attr &
			    0xffff) * glyph_bytes], glyph_attr >> 16, sub,
			    attr_mask);
		} else {
			bits = glyph_rows[(glyph_attr & 0xffff) * glyph_bytes];
		}
		/* fg (low half) and bg (high half) in one load too. */
		uint32_t colours;
		memcpy(&colours, &cell->fg, sizeof(colours));
		uint32_t bg = colours >> 16;
		uint32_t diff = (colours ^ bg) & 0xffff;
		emit_cell(words, bits, bg | bg << 16, diff | diff << 16, masks,
		    cell_words);
	}
}

/*
 * Preload (devlog 024): a PAIR row's cells are copied, at the
 * row's first scanline, into SCRATCH_X -- a bank the DVI DMA and core
 * 1's TMDS encoding leave alone, unlike main SRAM -- and its other
 * scanlines read them from there. IRQ only: a screenshot drawing from
 * the main loop reads the grid (the IRQ may refill this meanwhile).
 */
#if defined(__arm__)
#define IN_SCRATCH_X(name)	__attribute__((section(".scratch_x." name)))
#else
#define IN_SCRATCH_X(name)
#endif
static struct cell preload_row[GRID_MAX_COLS] IN_SCRATCH_X("preload")
    __attribute__((aligned(8)));
static const struct cell *preload_from;
static uint32_t preload_frame;

/* Two words a cell, moved as one 64-bit load and store. */
HOT_INLINE void
copy_cells_wide(struct cell *to, const struct cell *from, unsigned count)
{
	uint64_t *dst = (uint64_t *)to;
	const uint64_t *src = (const uint64_t *)from;

	for (unsigned i = 0; i < count; i++)
		dst[i] = src[i];
}

/* The cells to read for an unshared row: the preloaded copy if we can. */
HOT_INLINE const struct cell *
preloaded(const struct render *render, const struct cell *row_cells,
    unsigned cols)
{
	if (!in_interrupt())
		return row_cells;
	if (preload_from != row_cells || preload_frame != render->frame) {
		copy_cells_wide(preload_row, row_cells, cols);
		preload_from = row_cells;
		preload_frame = render->frame;
	}
	return preload_row;
}


#ifdef VEETEE_RENDER_EXPERIMENTS
/*
 * Experiments (devlog 024): where does the column loop's time go? Once a
 * frame, after drawing EXPERIMENT_LINE as usual, its cells are drawn
 * again by variants of the plain-text loop into spare lines nobody sees,
 * each timed with the cycle counter:
 *
 *   0  the loop as it is, into a line in main SRAM (the ring's kind)
 *   1  the same, into a line in SCRATCH_X (a bank core 1's TMDS work
 *      and the DVI DMA don't use)
 *   2  loads and arithmetic only: no stores (results xor'ed together)
 *   3  stores only: the same emit8 with fixed bits and colours
 *   4  the row's cells copied into SCRATCH_Y (cost alone)
 *   5  the loop reading those scratch cells, into the scratch line
 *
 * Plain text only (no attributes or pictures), so run it on plain text.
 */
#define EXPERIMENT_LINE	240
static uint32_t experiment_main[RENDER_MAX_WIDTH / 2]
    __attribute__((aligned(16)));
#define EXPERIMENT_COLS	80
/* SCRATCH_X has ~1.9 KB to spare, and the preload row has it: borrow it
 * (80 columns of pixels is its 1280 bytes) and invalidate it after. */
static_assert(EXPERIMENT_COLS * 16 <= sizeof(preload_row));
#define experiment_scratch	((uint32_t *)preload_row)
static struct cell experiment_cells[EXPERIMENT_COLS]
    __attribute__((section(".scratch_y.experiment"), aligned(8)));

enum experiment_mode { EXP_FULL, EXP_LOADS, EXP_STORES };

HOT_INLINE uint32_t
experiment_loop(const struct cell *cells, const uint8_t *glyph_rows,
    unsigned glyph_bytes, unsigned cols, uint32_t *words,
    enum experiment_mode mode)
{
	const uint32_t (*masks)[4] = byte_masks[0];
	uint32_t sink = 0;

	for (unsigned col = 0; col < cols; col++, words += 4) {
		uint32_t bits = 0x5a, bg = 0x1234, diff = 0x4321;
		if (mode != EXP_STORES) {
			uint32_t glyph_attr, colours;
			memcpy(&glyph_attr, &cells[col], sizeof(glyph_attr));
			bits = glyph_rows[(glyph_attr & 0xffff) * glyph_bytes];
			memcpy(&colours, &cells[col].fg, sizeof(colours));
			bg = colours >> 16;
			diff = (colours ^ bg) & 0xffff;
		}
		if (mode == EXP_LOADS) {
			const uint32_t *mask = masks[bits];
			sink ^= (bg ^ (diff & mask[0])) + (bg ^ (diff & mask[1])) +
			    (bg ^ (diff & mask[2])) + (bg ^ (diff & mask[3]));
			continue;
		}
		emit8(words, bits, bg | bg << 16, diff | diff << 16, masks);
	}
	return sink;
}

static void __attribute__((noinline))
HOT_FUNC(run_experiments)(struct render *render, const struct cell *cells,
    const uint8_t *glyph_rows)
{
	const unsigned glyph_bytes = render->font->glyph_bytes;
	const unsigned cols = render->grid->cols < EXPERIMENT_COLS ?
	    render->grid->cols : EXPERIMENT_COLS;
	volatile uint32_t sink;
	uint32_t t;

#define TIME(slot, expr) do {						\
		t = HOT_CYCLES();					\
		expr;							\
		render->experiment_cycles[slot] += HOT_CYCLES() - t;	\
	} while (0)
	TIME(0, (void)experiment_loop(cells, glyph_rows, glyph_bytes, cols,
	    experiment_main, EXP_FULL));
	TIME(1, (void)experiment_loop(cells, glyph_rows, glyph_bytes, cols,
	    experiment_scratch, EXP_FULL));
	TIME(2, sink = experiment_loop(cells, glyph_rows, glyph_bytes, cols,
	    experiment_main, EXP_LOADS));
	TIME(3, (void)experiment_loop(cells, glyph_rows, glyph_bytes, cols,
	    experiment_main, EXP_STORES));
	TIME(4, copy_cells(experiment_cells, cells, cols));
	TIME(5, (void)experiment_loop(experiment_cells, glyph_rows,
	    glyph_bytes, cols, experiment_scratch, EXP_FULL));
#undef TIME
	(void)sink;
	preload_from = nullptr;		/* we wrote over it */
	render->experiment_runs++;
}
#endif /* VEETEE_RENDER_EXPERIMENTS */

/* Without a plan yet: the whole grid row, the full loop. */
static const struct render_row_plan whole_row = {
	.spans = 1,
	.span = {{nullptr, 0, GRID_MAX_COLS, RENDER_SPAN_MIXED}},
};




void
HOT_FUNC(render_scanline)(uint16_t *pixels, unsigned line, void *ctx)
{
	struct render *render = ctx;
	struct grid *grid = render->grid;

	if (line == 0)
		render->frame = render->frame + 1;

	if (line < RENDER_METER_LINES && render->meter.on) {
		draw_meter(render, pixels, line);
		return;
	}

	/*
	 * BORDER and STATUS are the two values above any row number, so one
	 * compare keeps both off the text path. (Testing for STATUS inside
	 * the text path cost 231 cycles a line -- 4019 -> 4250 on a full
	 * 80x30 screen, VEETEE_RENDER_PROFILE -- though it only added
	 * per-line work: the column loop compiled worse around it.)
	 */
	unsigned text_row = render->line_row[line];
	if (!render->active || text_row >= RENDER_LINE_STATUS) {
		if (render->active && text_row == RENDER_LINE_STATUS) {
			draw_status_line(render, pixels, render->line_sub[line]);
			return;
		}
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
	const unsigned cols = grid->cols;
	const struct render_plan *plan = render->plan;
	const struct render_row_plan *row_plan = plan != nullptr ?
	    &plan->row[text_row] : &whole_row;
	const struct cell *row_cells = grid_row(grid, text_row);
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
	 * whole cell. Slow path, 8- and 6-px fonts (all built-ins); an overlay
	 * over one is composed here, the rare case spans don't cover.
	 */
	uint8_t line_size = grid->row_attr[grid->row_map[text_row]];
	if (line_size != ROW_NORMAL && (width == 8 || width == 6)) {
		bool covered_ignored = false;
		const struct cell *cells = row_plan->covered ?
		    compose_row(render, grid, text_row, &covered_ignored) :
		    row_cells;
		render_double_line(render, out, cells, sub, line_size, invert,
		    attr_mask, text_row, blink_off);
		return;
	}

	const bool pair_ok = render->pair_ready &&
	    render->pair_invert == invert;
	uint32_t loop_started = HOT_CYCLES();
	for (unsigned i = 0; i < row_plan->spans; i++) {
		const struct render_span *span = &row_plan->span[i];
		if (span->col >= cols)
			break;
		unsigned count = cols - span->col < span->count ?
		    cols - span->col : span->count;
		const struct cell *cells = span->cells != nullptr ?
		    span->cells : row_cells + span->col;
		/* Measured: PAIR rows gain 10%; PLAIN rows, whose loop is
		 * arithmetic rather than loads, lost 5%. */
		if (!row_plan->covered && span->kind == RENDER_SPAN_PAIR &&
		    pair_ok)
			cells = preloaded(render, row_cells, cols);

		if (width == 8 || width == 6) {
			const struct span_context context = {
				.render = render,
				.glyph_rows = glyph_rows,
				.masks = byte_masks[invert],
				.glyph_bytes = glyph_bytes,
				.sub = sub,
				.attr_mask = attr_mask,
			};
			/* Two pixels a word: 4 a cell at 8 px, 3 at 6. Each
			 * width gets its own copy of the loops (the constant
			 * is propagated), so neither pays for the other. */
			const unsigned cell_words = width / 2;
			uint32_t *words = (uint32_t *)out +
			    span->col * cell_words;
			unsigned done = 0;
			/* The table is only right in the reverse video it was
			 * built for (a visual bell flips it for a moment). */
			bool pair = span->kind == RENDER_SPAN_PAIR && pair_ok;
			bool plain = span->kind != RENDER_SPAN_MIXED;
			if (width == 8) {
				if (pair)
					done = draw_pair_span(&context, words,
					    cells, count, 4);
				else if (plain)
					done = draw_plain_span(&context, words,
					    cells, count, 4);
				if (done < count)
					draw_mixed_span(&context, words +
					    done * 4u, cells + done,
					    count - done, 4);
			} else {
				if (pair)
					done = draw_pair_span(&context, words,
					    cells, count, 3);
				else if (plain)
					done = draw_plain_span(&context, words,
					    cells, count, 3);
				if (done < count)
					draw_mixed_span(&context, words +
					    done * 3u, cells + done,
					    count - done, 3);
			}
			continue;
		}
		for (unsigned col = 0; col < count; col++) {
			const struct cell *cell = &cells[col];
			uint16_t *at = out + (span->col + col) * width;
			if (cell->attr & CELL_GRAPHIC) {
				draw_tile(render, at, cell, sub, width);
				continue;
			}
			uint32_t bits = read_bits(glyph_rows +
			    cell->glyph * glyph_bytes, row_bytes);

			if (cell->attr & CELL_ATTR_ALL)
				bits = attr_bits(render, bits, cell->attr, sub,
				    attr_mask);
			draw_cell(render, at, cell, bits, invert);
		}
	}
	if (HOT_PROFILE) {
		render->profile_loop_cycles += HOT_CYCLES() - loop_started;
		render->profile_loop_lines++;
	}
#ifdef VEETEE_RENDER_EXPERIMENTS
	if (line == EXPERIMENT_LINE && width == 8)
		run_experiments(render, row_cells, glyph_rows);
#endif

	/* An overlay over the cursor's cell hides the cursor. */
	unsigned cursor_col = grid->cursor_col;
	bool cursor_covered = false;
	if (row_plan->covered)
		for (unsigned i = 0; i < row_plan->spans; i++)
			if (row_plan->span[i].cells != nullptr &&
			    cursor_col - row_plan->span[i].col <
			    row_plan->span[i].count)
				cursor_covered = true;

	/*
	 * Cursor: redraw its cell with modified bits. Block = inverted
	 * colours; underline = solid underline row; bar = left column.
	 */
	if (grid->cursor_visible && grid->cursor_row == text_row &&
	    !cursor_covered &&
	    cursor_col < cols && !(grid->cursor_blink && blink_off)) {
		const struct cell *cell = &row_cells[cursor_col];
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

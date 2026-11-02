/*
 * box.c — overlay boxes (see box.h).
 */
#include "box.h"

#include <string.h>

uint16_t
box_glyph(const struct box *box, uint32_t codepoint, char fallback)
{
	uint16_t index = font_glyph_for(box->font, codepoint);

	return index != FONT_GLYPH_MISSING ? index :
	    font_glyph_for(box->font, (uint8_t)fallback);
}

static void
put(const struct box *box, unsigned row, unsigned col, uint16_t index,
    bool chosen)
{
	/* Swapped for the box, the theme's own for a chosen row. */
	uint16_t fg = chosen ? box->fg : box->bg;
	uint16_t bg = chosen ? box->bg : box->fg;

	if (row < box->rows && col < box->cols)
		box->cells[row * box->stride + col] =
		    grid_make_cell(index, fg, bg, 0);
}

unsigned
box_text(const struct box *box, unsigned row, unsigned col, unsigned end,
    const char *text, bool chosen)
{
	if (end > box->cols - 1)
		end = box->cols - 1;	/* never over the right border */
	for (; *text != '\0' && col < end; text++, col++) {
		uint8_t c = (uint8_t)*text;
		put(box, row, col, box_glyph(box, c < 0x80 ? c : '?', '?'),
		    chosen);
	}
	return col;
}

void
box_text_right(const struct box *box, unsigned row, const char *text,
    unsigned max, bool chosen)
{
	size_t len = strlen(text);

	if (len > max)
		len = max;
	if (len + 2 > box->cols)
		return;
	char cut[64];
	if (len >= sizeof(cut))
		len = sizeof(cut) - 1;
	memcpy(cut, text, len);
	cut[len] = '\0';
	(void)box_text(box, row, box->cols - 2 - (unsigned)len, box->cols - 1,
	    cut, chosen);
}

void
box_row(const struct box *box, unsigned row, bool chosen)
{
	const uint16_t space = box_glyph(box, ' ', ' ');

	for (unsigned col = 1; col + 1 < box->cols; col++)
		put(box, row, col, space, chosen);
}

void
box_frame(const struct box *box, const char *title)
{
	const uint16_t space = box_glyph(box, ' ', ' ');
	const uint16_t horizontal = box_glyph(box, 0x2500, '-');
	const uint16_t vertical = box_glyph(box, 0x2502, '|');
	const unsigned last_row = box->rows - 1, last_col = box->cols - 1;

	for (unsigned row = 0; row < box->rows; row++)
		for (unsigned col = 0; col < box->cols; col++)
			put(box, row, col, space, false);
	for (unsigned col = 1; col < last_col; col++) {
		put(box, 0, col, horizontal, false);
		put(box, last_row, col, horizontal, false);
	}
	for (unsigned row = 1; row < last_row; row++) {
		put(box, row, 0, vertical, false);
		put(box, row, last_col, vertical, false);
	}
	put(box, 0, 0, box_glyph(box, 0x250c, '+'), false);
	put(box, 0, last_col, box_glyph(box, 0x2510, '+'), false);
	put(box, last_row, 0, box_glyph(box, 0x2514, '+'), false);
	put(box, last_row, last_col, box_glyph(box, 0x2518, '+'), false);

	char spaced[64];
	size_t len = strlen(title);
	if (len + 4 > box->cols)
		len = box->cols > 4 ? box->cols - 4 : 0;
	if (len > sizeof(spaced) - 3)
		len = sizeof(spaced) - 3;
	spaced[0] = ' ';
	memcpy(spaced + 1, title, len);
	spaced[len + 1] = ' ';
	spaced[len + 2] = '\0';
	(void)box_text(box, 0, (box->cols - (unsigned)len - 2) / 2,
	    box->cols - 1, spaced, false);
}

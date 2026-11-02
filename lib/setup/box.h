/*
 * box.h — drawing the little boxes that go over the screen as overlays
 * (render.h): the Tools menu, the file picker.
 *
 * Layering: pure module (host-testable). A box draws into a caller's
 * array of cells, `stride` cells to a row, in the theme's colours
 * swapped (theme background on theme foreground) so it reads as the
 * terminal's own chrome; a chosen row is in the theme's own colours.
 * Lines use U+250x box drawing where the font has it, else + - |.
 */
#ifndef BOX_H
#define BOX_H

#include <stdbool.h>
#include <stdint.h>

#include "font.h"
#include "grid.h"

struct box {
	const struct font *font;
	struct cell	*cells;
	unsigned	 stride;	/* cells per row in `cells` */
	unsigned	 cols;		/* this box's size */
	unsigned	 rows;
	uint16_t	 fg;		/* the theme's colours */
	uint16_t	 bg;
};

/* A glyph, or `fallback`'s where the font lacks the codepoint. */
uint16_t box_glyph(const struct box *box, uint32_t codepoint, char fallback);

/* `text` from (row, col), stopping before column `end`; returns the
 * column after it. ASCII; anything else shows as '?'. */
unsigned box_text(const struct box *box, unsigned row, unsigned col,
	    unsigned end, const char *text, bool chosen);

/* `text` ending just inside the right border (cut to `max`). */
void	box_text_right(const struct box *box, unsigned row, const char *text,
	    unsigned max, bool chosen);

/* Fill the inside of `row` with blanks, chosen or not. */
void	box_row(const struct box *box, unsigned row, bool chosen);

/* Blank the whole box, draw its border, centre " title " in the top. */
void	box_frame(const struct box *box, const char *title);

#endif /* BOX_H */

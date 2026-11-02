/*
 * font.h — fixed-cell bitmap fonts loaded from PSF2 images.
 *
 * Layering: pure module (no hardware headers), below render/ and term/.
 *
 * A font is a set of glyph bitmaps, all cell width x cell height, plus a
 * sorted codepoint -> glyph index map. Glyph rows are packed MSB-first,
 * ceil(width / 8) bytes per row, exactly as in the PSF2 file.
 *
 * font_load_psf2() copies glyph data and the map into heap memory: the
 * renderer reads glyph rows ~80 times per scanline from core 1, and doing
 * that through the XIP flash cache would blow the line budget. Loading is
 * an init-time operation (STYLE.md §6: malloc only during init/reconfig).
 *
 * PSF2 extension: tools/ttf2psf.py appends "VTEX" + baseline + underline
 * row after the 32-byte header (honouring PSF2's headersize field), so we
 * know where to draw underlines. Fonts without it get a heuristic.
 */
#ifndef FONT_H
#define FONT_H

#include <stddef.h>
#include <stdint.h>

/* Glyph index 0 is always "missing glyph" in our own fonts. */
#define FONT_GLYPH_MISSING	0

struct font {
	uint8_t		 width;		/* cell width in pixels (1..16) */
	uint8_t		 height;	/* cell height in pixels (1..32) */
	uint8_t		 baseline;	/* row index of the baseline */
	uint8_t		 underline_row;	/* row index drawn by SGR 4 */
	uint8_t		 row_bytes;	/* bytes per glyph row */
	uint16_t	 glyph_bytes;	/* bytes per glyph = row_bytes * height */
	uint32_t	 glyph_count;
	uint8_t		*glyphs;	/* glyph_count * glyph_bytes, in RAM */
	uint32_t	 map_count;
	uint32_t	*map_codepoints;/* sorted ascending */
	uint16_t	*map_glyphs;	/* parallel to map_codepoints */
	uint32_t	*glyph_codepoints;/* glyph -> lowest codepoint (0: none) */
};

enum font_error {
	FONT_OK = 0,
	FONT_ERR_FORMAT = -1,	/* not PSF2, or truncated/inconsistent */
	FONT_ERR_SIZE = -2,	/* cell size outside what the renderer does */
	FONT_ERR_NOMEM = -3,
};

/* A PSF2 image compiled into the firmware (fonts_builtin.c). */
struct font_builtin {
	const char		*name;
	const unsigned char	*data;
	unsigned int		 len;
};

extern const struct font_builtin font_builtins[];
extern const unsigned int font_builtin_count;

/* Built-in font by name (as in tools/fonts.toml), or nullptr. */
const struct font_builtin *font_builtin_find(const char *name);

/*
 * Cell size of a built-in font straight from its PSF2 header, without
 * loading it (SET-UP lists every font with the grid size it gives).
 */
void	font_builtin_cell(const struct font_builtin *builtin, unsigned *width,
	    unsigned *height);

/*
 * Parse a PSF2 image into `font`, copying everything it needs (the image
 * may be freed afterwards). On failure `font` is left zeroed.
 */
[[nodiscard]] enum font_error
	font_load_psf2(struct font *font, const uint8_t *data, size_t len);

/* Release memory owned by `font` and zero it. */
void	font_free(struct font *font);

/*
 * Glyph index for a codepoint, or FONT_GLYPH_MISSING. Binary search over
 * the map: call it when a character is written, never per scanline.
 */
uint16_t font_glyph_for(const struct font *font, uint32_t codepoint);

/*
 * A codepoint that maps to `glyph` (the lowest, so ASCII wins when
 * bitmaps are shared), or 0. For reading the screen back (DECRQCRA).
 */
static inline uint32_t
font_codepoint_for(const struct font *font, uint16_t glyph)
{
	return glyph < font->glyph_count ? font->glyph_codepoints[glyph] : 0;
}

/* Pointer to row `row` of glyph `glyph` (no bounds checks). */
static inline const uint8_t *
font_glyph_row(const struct font *font, uint16_t glyph, unsigned row)
{
	return font->glyphs + (size_t)glyph * font->glyph_bytes +
	    row * font->row_bytes;
}

#endif /* FONT_H */

/*
 * font.c — PSF2 loader and codepoint lookup.
 *
 * PSF2 layout (all integers little-endian), from the Linux kbd docs:
 *
 *   0  magic       72 b5 4a 86
 *   4  version     0
 *   8  headersize  offset of the glyph bitmaps
 *  12  flags       bit 0: a Unicode table follows the bitmaps
 *  16  length      number of glyphs
 *  20  charsize    bytes per glyph
 *  24  height      rows per glyph
 *  28  width       pixels per row
 *
 * The Unicode table has one entry per glyph: UTF-8 codepoints, then
 * optionally sequences each introduced by 0xFE (combining sequences, which
 * we ignore), and a 0xFF terminator.
 */
#include "font.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define PSF2_HEADER_MIN		32
#define PSF2_FLAG_UNICODE	0x01
#define PSF2_SEQ_START		0xFE
#define PSF2_SEPARATOR		0xFF
#define VTEX_SIZE		8

/* Renderer limits: at most two bytes per glyph row, at most 32 rows. */
#define FONT_MAX_WIDTH		16
#define FONT_MAX_HEIGHT		32

static uint32_t
read_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24;
}

/*
 * Decode one UTF-8 sequence starting at *pos, stopping before `end`.
 * Returns the codepoint, or UINT32_MAX for malformed input (and advances
 * past one byte so the caller always makes progress). Overlong forms and
 * surrogates are rejected; a font table has no business containing them.
 */
static uint32_t
utf8_next(const uint8_t **pos, const uint8_t *end)
{
	const uint8_t *p = *pos;
	uint32_t codepoint;
	int extra;

	if (p[0] < 0x80) {
		*pos = p + 1;
		return p[0];
	} else if ((p[0] & 0xe0) == 0xc0) {
		codepoint = p[0] & 0x1f;
		extra = 1;
	} else if ((p[0] & 0xf0) == 0xe0) {
		codepoint = p[0] & 0x0f;
		extra = 2;
	} else if ((p[0] & 0xf8) == 0xf0) {
		codepoint = p[0] & 0x07;
		extra = 3;
	} else {
		*pos = p + 1;
		return UINT32_MAX;
	}

	if (end - p <= extra) {
		*pos = end;
		return UINT32_MAX;
	}
	for (int i = 1; i <= extra; i++) {
		if ((p[i] & 0xc0) != 0x80) {
			*pos = p + 1;
			return UINT32_MAX;
		}
		codepoint = codepoint << 6 | (p[i] & 0x3f);
	}
	*pos = p + 1 + extra;

	static const uint32_t min_for_length[4] = {0, 0x80, 0x800, 0x10000};
	if (codepoint < min_for_length[extra] || codepoint > 0x10ffff ||
	    (codepoint >= 0xd800 && codepoint <= 0xdfff))
		return UINT32_MAX;
	return codepoint;
}

/*
 * Walk the Unicode table. With `pairs` == nullptr it only counts entries;
 * otherwise it stores (codepoint << 16 | glyph) for each. Packing both into
 * one 64-bit key lets a single qsort order by codepoint and, for
 * duplicates, by lowest glyph index.
 */
static size_t
walk_unicode_table(const uint8_t *table, const uint8_t *end,
    uint32_t glyph_count, uint64_t *pairs)
{
	const uint8_t *pos = table;
	size_t count = 0;

	for (uint32_t glyph = 0; glyph < glyph_count && pos < end; glyph++) {
		bool in_sequence = false;

		while (pos < end && *pos != PSF2_SEPARATOR) {
			if (*pos == PSF2_SEQ_START) {
				in_sequence = true;
				pos++;
				continue;
			}
			uint32_t codepoint = utf8_next(&pos, end);
			if (in_sequence || codepoint == UINT32_MAX)
				continue;
			if (pairs != nullptr)
				pairs[count] = (uint64_t)codepoint << 16 | glyph;
			count++;
		}
		pos++;	/* skip the separator */
	}
	return count;
}

static int
compare_u64(const void *a, const void *b)
{
	uint64_t left = *(const uint64_t *)a;
	uint64_t right = *(const uint64_t *)b;

	return (left > right) - (left < right);
}

enum font_error
font_load_psf2(struct font *font, const uint8_t *data, size_t len)
{
	static const uint8_t magic[4] = {0x72, 0xb5, 0x4a, 0x86};
	uint64_t *pairs = nullptr;

	memset(font, 0, sizeof(*font));
	if (len < PSF2_HEADER_MIN || memcmp(data, magic, sizeof(magic)) != 0)
		return FONT_ERR_FORMAT;

	uint32_t header_size = read_le32(data + 8);
	uint32_t flags = read_le32(data + 12);
	uint32_t glyph_count = read_le32(data + 16);
	uint32_t glyph_bytes = read_le32(data + 20);
	uint32_t height = read_le32(data + 24);
	uint32_t width = read_le32(data + 28);

	if (width == 0 || width > FONT_MAX_WIDTH || height == 0 ||
	    height > FONT_MAX_HEIGHT)
		return FONT_ERR_SIZE;
	uint32_t row_bytes = (width + 7) / 8;
	/* glyph_count is bounded so the size product below can't overflow. */
	if (glyph_bytes != row_bytes * height || glyph_count == 0 ||
	    glyph_count > UINT16_MAX || header_size < PSF2_HEADER_MIN ||
	    header_size > len ||
	    (size_t)glyph_count * glyph_bytes > len - header_size)
		return FONT_ERR_FORMAT;

	font->width = (uint8_t)width;
	font->height = (uint8_t)height;
	font->row_bytes = (uint8_t)row_bytes;
	font->glyph_bytes = (uint16_t)glyph_bytes;
	font->glyph_count = glyph_count;

	/* Baseline/underline from our VTEX extension, else a guess. */
	const uint8_t *ext = data + PSF2_HEADER_MIN;
	if (header_size >= PSF2_HEADER_MIN + VTEX_SIZE &&
	    memcmp(ext, "VTEX", 4) == 0 && ext[4] == 1 && ext[5] < height &&
	    ext[6] < height) {
		font->baseline = ext[5];
		font->underline_row = ext[6];
	} else {
		/* Most console fonts leave ~1/8 of the cell for descenders. */
		font->baseline = (uint8_t)(height - 1 - height / 8);
		font->underline_row = (uint8_t)(height - 1 - height / 16);
	}

	size_t bitmap_len = (size_t)glyph_count * glyph_bytes;
	font->glyphs = malloc(bitmap_len);
	if (font->glyphs == nullptr)
		goto nomem;
	memcpy(font->glyphs, data + header_size, bitmap_len);

	const uint8_t *table = data + header_size + bitmap_len;
	const uint8_t *end = data + len;
	size_t pair_count;

	if (flags & PSF2_FLAG_UNICODE) {
		pair_count = walk_unicode_table(table, end, glyph_count,
		    nullptr);
		pairs = malloc((pair_count ? pair_count : 1) * sizeof(*pairs));
		if (pairs == nullptr)
			goto nomem;
		walk_unicode_table(table, end, glyph_count, pairs);
	} else {
		/* No table: glyph N is codepoint N (Latin-1 for 256 glyphs). */
		pair_count = glyph_count;
		pairs = malloc(pair_count * sizeof(*pairs));
		if (pairs == nullptr)
			goto nomem;
		for (uint32_t glyph = 0; glyph < glyph_count; glyph++)
			pairs[glyph] = (uint64_t)glyph << 16 | glyph;
	}
	qsort(pairs, pair_count, sizeof(*pairs), compare_u64);

	font->map_codepoints = malloc((pair_count ? pair_count : 1) *
	    sizeof(*font->map_codepoints));
	font->map_glyphs = malloc((pair_count ? pair_count : 1) *
	    sizeof(*font->map_glyphs));
	if (font->map_codepoints == nullptr || font->map_glyphs == nullptr)
		goto nomem;

	/* Split the packed keys, keeping the first glyph per codepoint. */
	uint32_t kept = 0;
	for (size_t i = 0; i < pair_count; i++) {
		uint32_t codepoint = (uint32_t)(pairs[i] >> 16);
		if (kept > 0 && font->map_codepoints[kept - 1] == codepoint)
			continue;
		font->map_codepoints[kept] = codepoint;
		font->map_glyphs[kept] = (uint16_t)pairs[i];
		kept++;
	}
	font->map_count = kept;
	free(pairs);
	return FONT_OK;

nomem:
	free(pairs);
	font_free(font);
	return FONT_ERR_NOMEM;
}

void
font_free(struct font *font)
{
	free(font->glyphs);
	free(font->map_codepoints);
	free(font->map_glyphs);
	memset(font, 0, sizeof(*font));
}

uint16_t
font_glyph_for(const struct font *font, uint32_t codepoint)
{
	uint32_t low = 0;
	uint32_t high = font->map_count;

	while (low < high) {
		uint32_t mid = low + (high - low) / 2;
		if (font->map_codepoints[mid] < codepoint)
			low = mid + 1;
		else
			high = mid;
	}
	if (low < font->map_count && font->map_codepoints[low] == codepoint)
		return font->map_glyphs[low];
	return FONT_GLYPH_MISSING;
}

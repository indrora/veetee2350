/*
 * test_font — PSF2 loader and codepoint lookup, run on the host.
 */
#include <string.h>

#include <unity.h>

#include "font.h"

static struct font font;

void
setUp(void)
{
	TEST_ASSERT_EQUAL(FONT_OK, font_load_psf2(&font, font_builtins[0].data,
	    font_builtins[0].len));
}

void
tearDown(void)
{
	font_free(&font);
}

static bool
glyph_has_ink(uint16_t glyph)
{
	for (unsigned row = 0; row < font.height; row++)
		if (*font_glyph_row(&font, glyph, row) != 0)
			return true;
	return false;
}

static void
test_default_font_geometry(void)
{
	/* ATI Small Wonder, 6x8, the default since devlog 025. */
	TEST_ASSERT_EQUAL_STRING("ati-smallw-6x8", font_builtins[0].name);
	TEST_ASSERT_EQUAL(6, font.width);
	TEST_ASSERT_EQUAL(8, font.height);
	TEST_ASSERT_EQUAL(1, font.row_bytes);
	/* VTEX extension present: baseline on the last row, so the
	 * underline can't go below it and shares it (as on the card). */
	TEST_ASSERT_EQUAL(7, font.baseline);
	TEST_ASSERT_EQUAL(7, font.underline_row);
}

static void
test_lookup(void)
{
	uint16_t letter_a = font_glyph_for(&font, 'A');
	uint16_t space = font_glyph_for(&font, ' ');

	TEST_ASSERT_NOT_EQUAL(FONT_GLYPH_MISSING, letter_a);
	TEST_ASSERT_TRUE(glyph_has_ink(letter_a));
	TEST_ASSERT_NOT_EQUAL(FONT_GLYPH_MISSING, space);
	TEST_ASSERT_FALSE(glyph_has_ink(space));
	TEST_ASSERT_NOT_EQUAL(font_glyph_for(&font, 'A'),
	    font_glyph_for(&font, 'B'));
	/* Synthesized box drawing and DEC scan line exist. */
	TEST_ASSERT_NOT_EQUAL(FONT_GLYPH_MISSING, font_glyph_for(&font, 0x2500));
	TEST_ASSERT_NOT_EQUAL(FONT_GLYPH_MISSING, font_glyph_for(&font, 0x23BA));
	/* Unmapped and out-of-range codepoints fall back to glyph 0. */
	TEST_ASSERT_EQUAL(FONT_GLYPH_MISSING, font_glyph_for(&font, 0x10fffd));
	TEST_ASSERT_EQUAL(FONT_GLYPH_MISSING, font_glyph_for(&font, 0x07));
}

static void
test_casual10_remap(void)
{
	/* fonts.toml remaps three swapped drawings in this font; '_' must be
	 * a single solid row, not the at-sign the raw cmap points at. */
	struct font casual;
	const struct font_builtin *entry = nullptr;

	for (unsigned i = 0; i < font_builtin_count; i++)
		if (strcmp(font_builtins[i].name, "notjam-casual-10") == 0)
			entry = &font_builtins[i];
	TEST_ASSERT_NOT_NULL(entry);
	TEST_ASSERT_EQUAL(FONT_OK, font_load_psf2(&casual, entry->data,
	    entry->len));
	uint16_t underscore = font_glyph_for(&casual, '_');
	unsigned inked_rows = 0;
	for (unsigned row = 0; row < casual.height; row++)
		if (*font_glyph_row(&casual, underscore, row) != 0)
			inked_rows++;
	TEST_ASSERT_EQUAL(1, inked_rows);
	font_free(&casual);
}

static void
test_rejects_bad_images(void)
{
	struct font bad;
	uint8_t image[64];

	memcpy(image, font_builtins[0].data, sizeof(image));
	image[0] ^= 0xff;
	TEST_ASSERT_EQUAL(FONT_ERR_FORMAT, font_load_psf2(&bad, image,
	    sizeof(image)));

	/* Valid header, but glyph data truncated. */
	TEST_ASSERT_EQUAL(FONT_ERR_FORMAT, font_load_psf2(&bad,
	    font_builtins[0].data, 64));

	/* Width 17 is beyond what the renderer supports. */
	memcpy(image, font_builtins[0].data, sizeof(image));
	image[28] = 17;
	TEST_ASSERT_EQUAL(FONT_ERR_SIZE, font_load_psf2(&bad, image,
	    sizeof(image)));
}

static void
test_all_builtins_load(void)
{
	for (unsigned i = 0; i < font_builtin_count; i++) {
		struct font each;
		TEST_ASSERT_EQUAL_MESSAGE(FONT_OK, font_load_psf2(&each,
		    font_builtins[i].data, font_builtins[i].len),
		    font_builtins[i].name);
		TEST_ASSERT_NOT_EQUAL(FONT_GLYPH_MISSING,
		    font_glyph_for(&each, 'x'));
		font_free(&each);
	}
}

int
main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_default_font_geometry);
	RUN_TEST(test_lookup);
	RUN_TEST(test_casual10_remap);
	RUN_TEST(test_rejects_bad_images);
	RUN_TEST(test_all_builtins_load);
	return UNITY_END();
}

/*
 * splash.c — paint the boot splash, one scanline at a time (splash.h).
 *
 * Runs in the render IRQ in place of render_scanline while the splash is
 * up, so it obeys the same rules (hal/video.h): RAM-resident code, no
 * blocking, no calls into flash. It does READ flash -- the image lives
 * there -- which is the one exception, and why splash.h does the sums.
 * A line outside the image is a word fill; a line through it is a copy,
 * two pixels to a 32-bit load and store (or one, doubled, at 2x).
 */
#include "splash.h"

#include "hot.h"

/*
 * Fill `count` words with `value`. Written out with an empty asm in the
 * loop so GCC can't turn it into a memset call, which lives in flash
 * (render.c's fill_words has the story, devlog 024).
 */
HOT_INLINE void
fill_words(uint32_t *out, unsigned count, uint32_t value)
{
	for (unsigned index = 0; index < count; index++) {
		out[index] = value;
		__asm__ volatile ("" ::: "memory");
	}
}

void
splash_layout(struct splash *splash, unsigned width, unsigned height)
{
	/*
	 * 1:1 up to 540 lines (256x256), doubled from 720 (512x512): it
	 * must fit, and every mode under 720 lines is under 512 tall.
	 */
	unsigned scale = height >= 720 ? 2 : 1;

	splash->width = (uint16_t)width;
	splash->height = (uint16_t)height;
	splash->scale = (uint8_t)scale;
	/* Even, so the image starts on a word: two pixels per store. */
	splash->left = (uint16_t)(((width - SPLASH_WIDTH * scale) / 2) & ~1u);
	splash->top = (uint16_t)((height - SPLASH_HEIGHT * scale) / 2);
	splash->background = (uint32_t)splash_background * 0x10001u;
}

void
HOT_FUNC(splash_scanline)(uint16_t *pixels, unsigned line, void *ctx)
{
	const struct splash *splash = ctx;
	uint32_t *out = (uint32_t *)pixels;	/* 4-byte aligned (video.h) */
	const unsigned scale = splash->scale;
	const unsigned image_lines = SPLASH_HEIGHT * scale;

	if (line < splash->top || line >= splash->top + image_lines) {
		fill_words(out, splash->width / 2, splash->background);
		return;
	}

	/* Background, left of the image. */
	unsigned left_words = splash->left / 2;
	fill_words(out, left_words, splash->background);
	out += left_words;

	/* The image's row, two pixels to a word (splash_pixels is aligned). */
	const uint32_t *source = (const uint32_t *)(splash_pixels +
	    (line - splash->top) / scale * SPLASH_WIDTH);
	if (scale == 1) {
		for (unsigned index = 0; index < SPLASH_WIDTH / 2; index++) {
			*out++ = source[index];
			__asm__ volatile ("" ::: "memory");	/* no memcpy */
		}
	} else {	/* 2: each pixel fills a word */
		for (unsigned index = 0; index < SPLASH_WIDTH / 2; index++) {
			uint32_t pair = source[index];
			*out++ = (pair & 0xffff) * 0x10001u;
			*out++ = (pair >> 16) * 0x10001u;
		}
	}

	/* Background, right of it. */
	unsigned used = splash->left + SPLASH_WIDTH * scale;
	fill_words(out, (splash->width - used) / 2, splash->background);
}

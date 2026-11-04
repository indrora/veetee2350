/*
 * splash.c — paint the boot splash, one scanline at a time (splash.h).
 *
 * Runs in the render IRQ in place of render_scanline while the splash is
 * up, so it obeys the same rules (hal/video.h): RAM-resident, no blocking,
 * no calls into flash. It is much cheaper than text: a line outside the
 * image is a word fill, and an image line is one palette lookup per
 * source pixel, written as whole 32-bit words.
 */
#include "splash.h"

#include <string.h>

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
splash_layout(struct splash *splash, unsigned width, unsigned height,
    void *ram, size_t ram_bytes)
{
	/*
	 * 1:1 up to 540 lines, doubled from 720: a small card either way
	 * (192x128, or 384x256 at 720p, under a third of the screen either way).
	 */
	unsigned scale = height >= 720 ? 2 : 1;

	splash->width = (uint16_t)width;
	splash->height = (uint16_t)height;
	splash->scale = (uint8_t)scale;
	/* Even, so the image starts on a word: two pixels per store. */
	splash->left = (uint16_t)(((width - SPLASH_WIDTH * scale) / 2) & ~1u);
	splash->top = (uint16_t)((height - SPLASH_HEIGHT * scale) / 2);
	for (unsigned index = 0; index < SPLASH_COLOURS; index++)
		splash->palette[index] = splash_palette[index];
	splash->background = (uint32_t)splash_palette[0] * 0x10001u;
	splash->pixels = splash_pixels;
	if (ram != nullptr && ram_bytes >= SPLASH_PIXEL_BYTES) {
		memcpy(ram, splash_pixels, SPLASH_PIXEL_BYTES);
		splash->pixels = ram;
	}
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

	/* The image: each source byte is two pixels, low nibble first. */
	const uint8_t *source = splash->pixels +
	    (line - splash->top) / scale * (SPLASH_WIDTH / 2);
	const uint16_t *palette = splash->palette;
	if (scale == 1) {
		for (unsigned index = 0; index < SPLASH_WIDTH / 2; index++) {
			uint8_t pair = source[index];
			*out++ = palette[pair & 15] |
			    (uint32_t)palette[pair >> 4] << 16;
		}
	} else {	/* 2: each pixel fills a word */
		for (unsigned index = 0; index < SPLASH_WIDTH / 2; index++) {
			uint8_t pair = source[index];
			*out++ = palette[pair & 15] * 0x10001u;
			*out++ = palette[pair >> 4] * 0x10001u;
		}
	}

	/* Background, right of it. */
	unsigned used = splash->left + SPLASH_WIDTH * scale;
	fill_words(out, (splash->width - used) / 2, splash->background);
}

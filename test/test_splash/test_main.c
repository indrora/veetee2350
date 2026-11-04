/*
 * The boot splash's layout and scanline (lib/render/splash.c): where the
 * card lands in each kind of mode, and that its lines come out right.
 */
#include <string.h>
#include <unity.h>

#include "splash.h"

static struct splash splash;
static uint8_t ram[SPLASH_PIXEL_BYTES] __attribute__((aligned(4)));
static uint16_t line[1280] __attribute__((aligned(4)));

void setUp(void) { memset(&splash, 0, sizeof(splash)); }
void tearDown(void) {}

/* Image pixel (x, y) as the palette colour the painter should draw. */
static uint16_t
image_colour(unsigned x, unsigned y)
{
	uint8_t pair = splash_pixels[y * (SPLASH_WIDTH / 2) + x / 2];
	return splash_palette[x & 1 ? pair >> 4 : pair & 15];
}

static void
test_layout_640x480(void)
{
	splash_layout(&splash, 640, 480, ram, sizeof(ram));
	TEST_ASSERT_EQUAL_UINT(1, splash.scale);
	TEST_ASSERT_EQUAL_UINT((640 - 192) / 2, splash.left);
	TEST_ASSERT_EQUAL_UINT((480 - 128) / 2, splash.top);
	/* Copied into the borrowed RAM, and the same bytes. */
	TEST_ASSERT_EQUAL_PTR(ram, splash.pixels);
	TEST_ASSERT_EQUAL_MEMORY(splash_pixels, ram, SPLASH_PIXEL_BYTES);
}

static void
test_layout_720p_doubles(void)
{
	splash_layout(&splash, 1280, 720, ram, sizeof(ram));
	TEST_ASSERT_EQUAL_UINT(2, splash.scale);
	TEST_ASSERT_EQUAL_UINT((1280 - 384) / 2, splash.left);
	TEST_ASSERT_EQUAL_UINT((720 - 256) / 2, splash.top);
}

static void
test_too_little_ram_reads_flash(void)
{
	splash_layout(&splash, 640, 480, ram, 16);
	TEST_ASSERT_EQUAL_PTR(splash_pixels, splash.pixels);
}

static void
test_background_line(void)
{
	splash_layout(&splash, 640, 480, ram, sizeof(ram));
	memset(line, 0xaa, sizeof(line));
	splash_scanline(line, 0, &splash);
	for (unsigned x = 0; x < 640; x++)
		TEST_ASSERT_EQUAL_HEX16(splash_palette[0], line[x]);
}

static void
test_image_line(unsigned width, unsigned height, unsigned image_row)
{
	splash_layout(&splash, width, height, ram, sizeof(ram));
	unsigned scale = splash.scale;
	memset(line, 0xaa, sizeof(line));
	splash_scanline(line, splash.top + image_row * scale, &splash);
	for (unsigned x = 0; x < width; x++) {
		uint16_t want = splash_palette[0];
		if (x >= splash.left && x < splash.left + SPLASH_WIDTH * scale)
			want = image_colour((x - splash.left) / scale, image_row);
		TEST_ASSERT_EQUAL_HEX16(want, line[x]);
	}
}

static void test_image_line_1x(void) { test_image_line(800, 480, 10); }
static void test_image_line_2x(void) { test_image_line(1280, 720, 90); }

int
main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_layout_640x480);
	RUN_TEST(test_layout_720p_doubles);
	RUN_TEST(test_too_little_ram_reads_flash);
	RUN_TEST(test_background_line);
	RUN_TEST(test_image_line_1x);
	RUN_TEST(test_image_line_2x);
	return UNITY_END();
}

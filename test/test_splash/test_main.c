/*
 * The boot splash's layout and scanline (lib/render/splash.c): where the
 * card lands in each kind of mode, and that its lines come out right.
 */
#include <string.h>
#include <unity.h>

#include "splash.h"

static struct splash splash;
static uint16_t line[1280] __attribute__((aligned(4)));

void setUp(void) { memset(&splash, 0, sizeof(splash)); }
void tearDown(void) {}

static void
test_layout_640x480(void)
{
	splash_layout(&splash, 640, 480);
	TEST_ASSERT_EQUAL_UINT(1, splash.scale);
	TEST_ASSERT_EQUAL_UINT((640 - 256) / 2, splash.left);
	TEST_ASSERT_EQUAL_UINT((480 - 256) / 2, splash.top);
}

static void
test_layout_720p_doubles(void)
{
	splash_layout(&splash, 1280, 720);
	TEST_ASSERT_EQUAL_UINT(2, splash.scale);
	TEST_ASSERT_EQUAL_UINT((1280 - 512) / 2, splash.left);
	TEST_ASSERT_EQUAL_UINT((720 - 512) / 2, splash.top);
}

/* 720x480 centres to an odd column; the image must start on a word. */
static void
test_left_is_even(void)
{
	splash_layout(&splash, 722, 480);
	TEST_ASSERT_EQUAL_UINT(0, splash.left & 1);
}

static void
test_background_line(void)
{
	splash_layout(&splash, 640, 480);
	memset(line, 0xaa, sizeof(line));
	splash_scanline(line, 0, &splash);
	for (unsigned x = 0; x < 640; x++)
		TEST_ASSERT_EQUAL_HEX16(splash_background, line[x]);
}

static void
test_image_line(unsigned width, unsigned height, unsigned image_row)
{
	splash_layout(&splash, width, height);
	unsigned scale = splash.scale;
	memset(line, 0xaa, sizeof(line));
	splash_scanline(line, splash.top + image_row * scale, &splash);
	for (unsigned x = 0; x < width; x++) {
		uint16_t want = splash_background;
		if (x >= splash.left && x < splash.left + SPLASH_WIDTH * scale)
			want = splash_pixels[image_row * SPLASH_WIDTH +
			    (x - splash.left) / scale];
		TEST_ASSERT_EQUAL_HEX16(want, line[x]);
	}
}

static void test_image_line_1x(void) { test_image_line(800, 480, 150); }
static void test_image_line_2x(void) { test_image_line(1280, 720, 170); }
static void test_last_image_line(void) { test_image_line(960, 540, 255); }

int
main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_layout_640x480);
	RUN_TEST(test_layout_720p_doubles);
	RUN_TEST(test_left_is_even);
	RUN_TEST(test_background_line);
	RUN_TEST(test_image_line_1x);
	RUN_TEST(test_image_line_2x);
	RUN_TEST(test_last_image_line);
	return UNITY_END();
}

/*
 * Streaming QOI encoder against hand-worked bytes from the spec.
 */
#include <string.h>

#include <unity.h>

#include "qoi_stream.h"

static uint8_t out[256];
static size_t out_len;

static bool
collect(const uint8_t *bytes, size_t len, void *ctx)
{
	(void)ctx;
	memcpy(out + out_len, bytes, len);
	out_len += len;
	return true;
}

void
setUp(void)
{
	out_len = 0;
}

void
tearDown(void)
{
}

static void
test_three_pixels(void)
{
	static const uint8_t want[] = {
		'q', 'o', 'i', 'f', 0, 0, 0, 3, 0, 0, 0, 1, 3, 0,
		0xc1,		/* run of 2: black equals the initial pixel */
		0x5a,		/* red: r -1 mod 256, g 0, b 0 -> DIFF */
		0, 0, 0, 0, 0, 0, 0, 1,
	};
	struct qoi_encoder qoi;

	TEST_ASSERT_TRUE(qoi_begin(&qoi, 3, 1, collect, nullptr));
	qoi_pixel(&qoi, 0, 0, 0);
	qoi_pixel(&qoi, 0, 0, 0);
	qoi_pixel(&qoi, 255, 0, 0);
	TEST_ASSERT_TRUE(qoi_end(&qoi));
	TEST_ASSERT_EQUAL(sizeof(want), out_len);
	TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, sizeof(want));
}

static void
test_ops(void)
{
	struct qoi_encoder qoi;

	TEST_ASSERT_TRUE(qoi_begin(&qoi, 5, 1, collect, nullptr));
	qoi_pixel(&qoi, 100, 50, 25);	/* RGB literal */
	qoi_pixel(&qoi, 110, 60, 33);	/* LUMA: dg 10, dr-dg 0, db-dg -2 */
	qoi_pixel(&qoi, 100, 50, 25);	/* INDEX: seen before */
	qoi_pixel(&qoi, 101, 49, 25);	/* DIFF */
	for (int i = 0; i < 70; i++)	/* runs split at 62 */
		qoi_pixel(&qoi, 101, 49, 25);
	TEST_ASSERT_TRUE(qoi_end(&qoi));
	const uint8_t *body = out + 14;
	TEST_ASSERT_EQUAL_HEX8(0xfe, body[0]);
	TEST_ASSERT_EQUAL_HEX8(100, body[1]);
	TEST_ASSERT_EQUAL_HEX8(0x80 | (10 + 32), body[4]);
	TEST_ASSERT_EQUAL_HEX8((0 + 8) << 4 | (-2 + 8), body[5]);
	unsigned slot = (100 * 3 + 50 * 5 + 25 * 7 + 255 * 11) % 64;
	TEST_ASSERT_EQUAL_HEX8(slot, body[6]);
	TEST_ASSERT_EQUAL_HEX8(0x40 | 3 << 4 | 1 << 2 | 2, body[7]);
	TEST_ASSERT_EQUAL_HEX8(0xc0 | 61, body[8]);	/* 62 */
	TEST_ASSERT_EQUAL_HEX8(0xc0 | 7, body[9]);	/* 8 */
}

/*
 * Regression: the decoder's index starts as (0,0,0,0). Opaque black must
 * not be sent as an index into a slot the decoder never filled -- it
 * comes back with alpha 0, and every hash after that disagrees.
 */
static void
test_black_not_from_empty_index(void)
{
	struct qoi_encoder qoi;

	TEST_ASSERT_TRUE(qoi_begin(&qoi, 2, 1, collect, nullptr));
	qoi_pixel(&qoi, 255, 0, 0);
	qoi_pixel(&qoi, 0, 0, 0);
	TEST_ASSERT_TRUE(qoi_end(&qoi));
	TEST_ASSERT_EQUAL_HEX8(0x5a, out[14]);	/* red: DIFF from black */
	TEST_ASSERT_EQUAL_HEX8(0x40 | 3 << 4 | 2 << 2 | 2, out[15]);
}

int
main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_three_pixels);
	RUN_TEST(test_ops);
	RUN_TEST(test_black_not_from_empty_index);
	return UNITY_END();
}

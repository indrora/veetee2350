/*
 * ttyrec: headers, telling ttyrec from raw, and the player's pacing.
 */
#include <string.h>

#include <unity.h>

#include "ttyrec.h"

static uint8_t out[512];
static size_t out_len;

static void
collect(const uint8_t *bytes, size_t len, void *ctx)
{
	(void)ctx;
	memcpy(out + out_len, bytes, len);
	out_len += len;
	out[out_len] = '\0';
}

void
setUp(void)
{
	out_len = 0;
	out[0] = '\0';
}

void
tearDown(void)
{
}

/* Append a record to `file` at *len. */
static void
add_record(uint8_t *file, size_t *len, uint32_t sec, uint32_t usec,
    const char *text)
{
	struct ttyrec_header header = {sec, usec, (uint32_t)strlen(text)};

	ttyrec_put_header(file + *len, &header);
	memcpy(file + *len + TTYREC_HEADER, text, header.len);
	*len += TTYREC_HEADER + header.len;
}

static void
test_header_round_trip(void)
{
	struct ttyrec_header in = {1700000000u, 123456u, 42u}, back;
	uint8_t bytes[TTYREC_HEADER];

	ttyrec_put_header(bytes, &in);
	TEST_ASSERT_EQUAL_HEX8(0x40, bytes[4]);	/* 123456 = 0x01e240, LE */
	TEST_ASSERT_EQUAL_HEX8(0xe2, bytes[5]);
	TEST_ASSERT_EQUAL_HEX8(42, bytes[8]);
	ttyrec_get_header(bytes, &back);
	TEST_ASSERT_EQUAL_UINT32(in.sec, back.sec);
	TEST_ASSERT_EQUAL_UINT32(in.usec, back.usec);
	TEST_ASSERT_EQUAL_UINT32(in.len, back.len);
}

static void
test_plausible(void)
{
	uint8_t file[128];
	size_t len = 0;

	add_record(file, &len, 10, 0, "hello");
	add_record(file, &len, 10, 500000, " world");
	TEST_ASSERT_TRUE(ttyrec_plausible(file, len));
	/* ANSI art and text: the "usec" field is printable bytes. */
	const char *ansi = "\x1b[0;1;34m\xdc\xdc\xdc\x1b[0m Welcome to the BBS";
	TEST_ASSERT_FALSE(ttyrec_plausible((const uint8_t *)ansi,
	    strlen(ansi)));
	TEST_ASSERT_FALSE(ttyrec_plausible((const uint8_t *)"hello, world",
	    12));
	/* Time running backwards between the first two records. */
	len = 0;
	add_record(file, &len, 10, 0, "a");
	add_record(file, &len, 9, 0, "b");
	TEST_ASSERT_FALSE(ttyrec_plausible(file, len));
	TEST_ASSERT_FALSE(ttyrec_plausible(file, 11));	/* too short */
}

/* Records come out when the play clock reaches them, not before. */
static void
test_pacing(void)
{
	struct ttyrec_player player;
	uint8_t file[128];
	size_t len = 0, used = 0;

	add_record(file, &len, 100, 0, "a");
	add_record(file, &len, 100, 300000, "b");	/* +300 ms */
	add_record(file, &len, 101, 0, "c");		/* +1000 ms */
	ttyrec_player_start(&player, true, 1000);

	used += ttyrec_player_play(&player, file + used, len - used, 1000,
	    collect, nullptr);
	TEST_ASSERT_EQUAL_STRING("a", (char *)out);	/* the first: at once */
	used += ttyrec_player_play(&player, file + used, len - used, 1200,
	    collect, nullptr);
	TEST_ASSERT_EQUAL_STRING("a", (char *)out);
	used += ttyrec_player_play(&player, file + used, len - used, 1300,
	    collect, nullptr);
	TEST_ASSERT_EQUAL_STRING("ab", (char *)out);
	/* 2x from here: "c" is 700 ms of recording away, 350 ms of ours.
	 * (Steps stay under the 250 ms a single pass may count.) */
	player.speed = TTYREC_SPEED_1X + 1;
	used += ttyrec_player_play(&player, file + used, len - used, 1500,
	    collect, nullptr);
	used += ttyrec_player_play(&player, file + used, len - used, 1600,
	    collect, nullptr);
	TEST_ASSERT_EQUAL_STRING("ab", (char *)out);
	used += ttyrec_player_play(&player, file + used, len - used, 1650,
	    collect, nullptr);
	TEST_ASSERT_EQUAL_STRING("abc", (char *)out);
	TEST_ASSERT_EQUAL(len, used);
}

/* Bytes offered a few at a time (as the card reads them) still work. */
static void
test_split_offers(void)
{
	struct ttyrec_player player;
	uint8_t file[128];
	size_t len = 0, used = 0;

	add_record(file, &len, 5, 0, "hello");
	add_record(file, &len, 5, 0, ", world");
	ttyrec_player_start(&player, true, 0);
	while (used < len) {
		size_t chunk = len - used < 5 ? len - used : 5;
		size_t took = ttyrec_player_play(&player, file + used, chunk, 0,
		    collect, nullptr);
		TEST_ASSERT_TRUE(took > 0);
		used += took;
	}
	TEST_ASSERT_EQUAL_STRING("hello, world", (char *)out);
}

/* A minute of nothing in the recording plays as TTYREC_MAX_GAP_MS. */
static void
test_long_gap(void)
{
	struct ttyrec_player player;
	uint8_t file[128];
	size_t len = 0, used = 0;
	uint32_t now = 0;

	add_record(file, &len, 0, 0, "x");
	add_record(file, &len, 60, 0, "y");
	ttyrec_player_start(&player, true, now);
	used += ttyrec_player_play(&player, file, len, now, collect, nullptr);
	while (strcmp((char *)out, "xy") != 0 && now < 70000) {
		now += 50;
		used += ttyrec_player_play(&player, file + used, len - used,
		    now, collect, nullptr);
	}
	TEST_ASSERT_EQUAL_STRING("xy", (char *)out);
	TEST_ASSERT_UINT32_WITHIN(100, TTYREC_MAX_GAP_MS, now);
}

static void
test_pause_and_max(void)
{
	struct ttyrec_player player;
	uint8_t file[128];
	size_t len = 0, used = 0;

	add_record(file, &len, 0, 0, "1");
	add_record(file, &len, 30, 0, "2");
	add_record(file, &len, 3000, 0, "3");
	ttyrec_player_start(&player, true, 0);
	player.paused = true;
	TEST_ASSERT_EQUAL(0, ttyrec_player_play(&player, file, len, 500,
	    collect, nullptr));
	player.paused = false;
	player.speed = TTYREC_SPEED_MAX;
	used = ttyrec_player_play(&player, file, len, 500, collect, nullptr);
	TEST_ASSERT_EQUAL(len, used);
	TEST_ASSERT_EQUAL_STRING("123", (char *)out);
}

/* Raw files go at 960 bytes a second of play clock at 1x. */
static void
test_raw_rate(void)
{
	struct ttyrec_player player;
	static uint8_t art[2000];
	size_t used = 0;

	memset(art, 'A', sizeof(art));
	ttyrec_player_start(&player, false, 0);
	used += ttyrec_player_play(&player, art, sizeof(art), 100, collect,
	    nullptr);
	TEST_ASSERT_EQUAL(96, used);
	out_len = 0;
	used += ttyrec_player_play(&player, art + used, sizeof(art) - used,
	    200, collect, nullptr);
	TEST_ASSERT_EQUAL(192, used);
	/* "max", then back to 1x: no backlog either way. */
	out_len = 0;
	player.speed = TTYREC_SPEED_MAX;
	used += ttyrec_player_play(&player, art + used, 500, 300, collect,
	    nullptr);
	TEST_ASSERT_EQUAL(692, used);
	player.speed = TTYREC_SPEED_1X;
	TEST_ASSERT_EQUAL(0, ttyrec_player_play(&player, art + used,
	    sizeof(art) - used, 300, collect, nullptr));
	TEST_ASSERT_EQUAL(96, ttyrec_player_play(&player, art + used,
	    sizeof(art) - used, 400, collect, nullptr));
}

/* With onlcr, bare LFs go out as CR LF; CR LF stays, even split.
 * Without it (the default), bytes go out as they are. */
static void
test_onlcr(void)
{
	struct ttyrec_player player;
	const char *text = "one\ntwo\r\nthree\n\n";
	uint8_t file[64];
	size_t len = 0;

	ttyrec_player_start(&player, false, 0);
	player.speed = TTYREC_SPEED_MAX;
	(void)ttyrec_player_play(&player, (const uint8_t *)text,
	    strlen(text), 0, collect, nullptr);
	TEST_ASSERT_EQUAL_STRING(text, (char *)out);

	out_len = 0;
	ttyrec_player_start(&player, false, 0);
	player.speed = TTYREC_SPEED_MAX;
	player.onlcr = true;
	(void)ttyrec_player_play(&player, (const uint8_t *)text, 9, 0,
	    collect, nullptr);		/* "one\ntwo\r" */
	(void)ttyrec_player_play(&player, (const uint8_t *)text + 9,
	    strlen(text) - 9, 0, collect, nullptr);
	TEST_ASSERT_EQUAL_STRING("one\r\ntwo\r\nthree\r\n\r\n", (char *)out);

	/* A recording too, when asked. */
	out_len = 0;
	add_record(file, &len, 0, 0, "a\nb");
	ttyrec_player_start(&player, true, 0);
	player.onlcr = true;
	(void)ttyrec_player_play(&player, file, len, 0, collect, nullptr);
	TEST_ASSERT_EQUAL_STRING("a\r\nb", (char *)out);
}

int
main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_header_round_trip);
	RUN_TEST(test_plausible);
	RUN_TEST(test_pacing);
	RUN_TEST(test_split_offers);
	RUN_TEST(test_long_gap);
	RUN_TEST(test_pause_and_max);
	RUN_TEST(test_raw_rate);
	RUN_TEST(test_onlcr);
	return UNITY_END();
}

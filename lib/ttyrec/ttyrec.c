/*
 * ttyrec.c — ttyrec headers and the player's pacing (see ttyrec.h).
 *
 * The play clock runs in recording time: each main-loop pass moves it
 * by the wall-clock time since the last pass, times the speed. A record
 * is handed on once the clock reaches its time (relative to the first
 * record's), all at once; a raw file is handed on at TTYREC_RAW_RATE
 * bytes per second of play clock. One wall-clock step is capped at
 * STEP_MAX_MS, so time the main loop spent elsewhere (an SD write,
 * SET-UP) doesn't come out as a burst.
 */
#include "ttyrec.h"

#include <string.h>

#define STEP_MAX_MS	250

/* Speeds in quarters of real time; the last entry is "max". */
static const uint8_t speed_quarters[TTYREC_SPEEDS] = {
	1, 2, 4, 8, 16, 32, 64, 0
};
static const char *const speed_names[TTYREC_SPEEDS] = {
	"1/4x", "1/2x", "1x", "2x", "4x", "8x", "16x", "max"
};

const char *
ttyrec_speed_name(unsigned speed)
{
	return speed < TTYREC_SPEEDS ? speed_names[speed] : "?";
}

static void
put32(uint8_t *out, uint32_t value)
{
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
	out[2] = (uint8_t)(value >> 16);
	out[3] = (uint8_t)(value >> 24);
}

static uint32_t
get32(const uint8_t *in)
{
	return (uint32_t)in[0] | (uint32_t)in[1] << 8 |
	    (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24;
}

void
ttyrec_put_header(uint8_t out[TTYREC_HEADER],
    const struct ttyrec_header *header)
{
	put32(out, header->sec);
	put32(out + 4, header->usec);
	put32(out + 8, header->len);
}

void
ttyrec_get_header(const uint8_t in[TTYREC_HEADER],
    struct ttyrec_header *header)
{
	header->sec = get32(in);
	header->usec = get32(in + 4);
	header->len = get32(in + 8);
}

static uint64_t
header_us(const struct ttyrec_header *header)
{
	return (uint64_t)header->sec * 1000000u + header->usec;
}

/*
 * Text makes implausible headers fast: any four printable bytes read as
 * a little-endian number are at least 0x20202020, far past a million
 * microseconds or a megabyte record.
 */
bool
ttyrec_plausible(const uint8_t *head, size_t len)
{
	struct ttyrec_header first, second;

	if (len < TTYREC_HEADER)
		return false;
	ttyrec_get_header(head, &first);
	if (first.usec >= 1000000u || first.len > TTYREC_MAX_RECORD)
		return false;
	size_t next = TTYREC_HEADER + (size_t)first.len;
	if (next + TTYREC_HEADER > len)
		return true;	/* only one header to judge by */
	ttyrec_get_header(head + next, &second);
	return second.usec < 1000000u && second.len <= TTYREC_MAX_RECORD &&
	    header_us(&second) >= header_us(&first);
}

void
ttyrec_player_start(struct ttyrec_player *player, bool ttyrec,
    uint32_t now_ms)
{
	memset(player, 0, sizeof(*player));
	player->ttyrec = ttyrec;
	player->speed = TTYREC_SPEED_1X;
	player->last_ms = now_ms;
}

/* Move the play clock on by the wall-clock time since the last pass. */
static void
advance(struct ttyrec_player *player, uint32_t now_ms)
{
	uint32_t elapsed = now_ms - player->last_ms;

	player->last_ms = now_ms;
	if (elapsed > STEP_MAX_MS)
		elapsed = STEP_MAX_MS;
	if (player->paused || player->speed == TTYREC_SPEED_MAX)
		return;
	player->clock_us += (uint64_t)elapsed * 1000u *
	    speed_quarters[player->speed % TTYREC_SPEEDS] / 4u;
}

/* A header is complete: work out when its record is due. */
static void
begin_record(struct ttyrec_player *player)
{
	struct ttyrec_header header;

	ttyrec_get_header(player->header, &header);
	player->header_have = 0;
	player->payload_left = header.len;

	uint64_t at = header_us(&header);
	if (!player->started) {
		player->started = true;
		player->first_us = at;
	}
	/* Time running backwards in a recording: play it straight on. */
	uint64_t due = at >= player->first_us ? at - player->first_us : 0;
	if (due < player->due_us)
		due = player->due_us;
	player->due_us = due;
	/* A long pause: bring the clock forward so it lasts MAX_GAP. */
	uint64_t gap = (uint64_t)TTYREC_MAX_GAP_MS * 1000u;
	if (due > player->clock_us + gap)
		player->clock_us = due - gap;
}

/* Hand bytes on, with each bare LF as CR LF if asked (see ttyrec.h). */
static void
hand_on(struct ttyrec_player *player, const uint8_t *data, size_t len,
    ttyrec_sink_fn sink, void *ctx)
{
	size_t start = 0;

	if (!player->onlcr) {
		sink(data, len, ctx);
		if (len > 0)
			player->last = data[len - 1];
		return;
	}
	for (size_t i = 0; i < len; i++) {
		uint8_t before = i > 0 ? data[i - 1] : player->last;
		if (data[i] != '\n' || before == '\r')
			continue;
		if (i > start)
			sink(data + start, i - start, ctx);
		sink((const uint8_t *)"\r", 1, ctx);
		start = i;	/* the LF goes out with what follows */
	}
	if (len > start)
		sink(data + start, len - start, ctx);
	if (len > 0)
		player->last = data[len - 1];
}

static size_t
play_ttyrec(struct ttyrec_player *player, const uint8_t *data, size_t len,
    ttyrec_sink_fn sink, void *ctx)
{
	const bool max = player->speed == TTYREC_SPEED_MAX;
	size_t used = 0;

	while (used < len) {
		if (player->payload_left > 0) {
			if (!max && player->due_us > player->clock_us)
				break;		/* not yet */
			size_t take = len - used;
			if (take > player->payload_left)
				take = player->payload_left;
			hand_on(player, data + used, take, sink, ctx);
			used += take;
			player->payload_left -= (uint32_t)take;
			player->played += take;
			if (max && player->clock_us < player->due_us)
				player->clock_us = player->due_us;
			continue;
		}
		size_t take = TTYREC_HEADER - player->header_have;
		if (take > len - used)
			take = len - used;
		memcpy(player->header + player->header_have, data + used,
		    take);
		player->header_have = (uint8_t)(player->header_have + take);
		used += take;
		if (player->header_have == TTYREC_HEADER)
			begin_record(player);
	}
	return used;
}

static size_t
play_raw(struct ttyrec_player *player, const uint8_t *data, size_t len,
    ttyrec_sink_fn sink, void *ctx)
{
	size_t take = len;

	if (player->speed == TTYREC_SPEED_MAX) {
		/* Keep the clock with the bytes, for slowing down again
		 * (rounded up, or the next second would come up a byte
		 * short). */
		player->clock_us = ((player->played + len) * 1000000u +
		    TTYREC_RAW_RATE - 1) / TTYREC_RAW_RATE;
	} else {
		uint64_t allowed = player->clock_us * TTYREC_RAW_RATE /
		    1000000u;
		if (allowed <= player->played)
			return 0;
		if (take > allowed - player->played)
			take = (size_t)(allowed - player->played);
	}
	hand_on(player, data, take, sink, ctx);
	player->played += take;
	return take;
}

size_t
ttyrec_player_play(struct ttyrec_player *player, const uint8_t *data,
    size_t len, uint32_t now_ms, ttyrec_sink_fn sink, void *ctx)
{
	if (now_ms != player->last_ms)
		advance(player, now_ms);
	if (player->paused || len == 0)
		return 0;
	return player->ttyrec ? play_ttyrec(player, data, len, sink, ctx) :
	    play_raw(player, data, len, sink, ctx);
}

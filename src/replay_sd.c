/*
 * replay_sd.c — play a card file into the terminal (see replay_sd.h).
 *
 * The file is read a buffer at a time and offered to lib/ttyrec's
 * player, which takes what's due; what it leaves is offered again next
 * pass. One pass spends at most POLL_BUDGET_US feeding the terminal, so
 * at "max" speed the rest of the main loop (keys, USB, the status line)
 * still runs every few milliseconds.
 */
#include "replay_sd.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "pico/time.h"
#include "settings.h"
#include "storage.h"

#define BUFFER_BYTES	2048
#define POLL_BUDGET_US	15000

static bool active;
static int handle = -1;
static char name[STORAGE_NAME_MAX];
static struct ttyrec_player player;
static uint8_t buffer[BUFFER_BYTES];
static size_t buffered, offset;

bool
replay_sd_start(const char *path, uint32_t now_ms, unsigned newlines,
    char *error, size_t error_len)
{
	replay_sd_stop();
	if (!storage_mount()) {
		snprintf(error, error_len, "%s", storage_error());
		return false;
	}
	handle = storage_open(path, STORAGE_READ);
	if (handle < 0) {
		snprintf(error, error_len, "%s", storage_error());
		return false;
	}
	long got = storage_read(handle, buffer, sizeof(buffer));
	if (got < 0) {
		snprintf(error, error_len, "%s", storage_error());
		replay_sd_stop();
		return false;
	}
	buffered = (size_t)got;
	offset = 0;
	const char *slash = strrchr(path, '/');
	snprintf(name, sizeof(name), "%s", slash != nullptr ? slash + 1 : path);
	bool ttyrec = ttyrec_plausible(buffer, buffered);
	ttyrec_player_start(&player, ttyrec, now_ms);
	player.onlcr = newlines == NEWLINES_ALL ||
	    (newlines == NEWLINES_RAW && !ttyrec);
	active = true;
	return true;
}

void
replay_sd_stop(void)
{
	if (handle >= 0)
		(void)storage_close(handle);
	handle = -1;
	active = false;
}

bool
replay_sd_active(void)
{
	return active;
}

bool
replay_sd_poll(uint32_t now_ms, ttyrec_sink_fn sink, void *ctx)
{
	if (!active)
		return false;
	uint32_t start = time_us_32();
	while (time_us_32() - start < POLL_BUDGET_US) {
		if (offset == buffered) {
			long got = storage_read(handle, buffer,
			    sizeof(buffer));
			if (got <= 0) {
				replay_sd_stop();	/* the end, or a read error */
				return false;
			}
			buffered = (size_t)got;
			offset = 0;
		}
		size_t used = ttyrec_player_play(&player, buffer + offset,
		    buffered - offset, now_ms, sink, ctx);
		if (used == 0)
			break;		/* nothing due yet */
		offset += used;
		board_watchdog_feed();
	}
	return true;
}

bool
replay_sd_key(const struct key_event *key)
{
	char c = key->code == KEY_CHAR ? (char)key->byte : '\0';

	if (key->code == KEY_ESCAPE || c == 'q' || c == 'Q') {
		replay_sd_stop();
		return false;
	}
	if (c == ' ')
		player.paused = !player.paused;
	else if (c == 'n' || c == 'N')
		player.onlcr = !player.onlcr;
	else if ((key->code == KEY_UP || c == '+' || c == '=') &&
	    player.speed + 1 < TTYREC_SPEEDS)
		player.speed++;
	else if ((key->code == KEY_DOWN || c == '-') && player.speed > 0)
		player.speed--;
	return true;
}

void
replay_sd_describe(char *out, size_t len)
{
	snprintf(out, len, "Play %s %s%s", name, player.paused ? "paused" :
	    ttyrec_speed_name(player.speed), player.onlcr ? " +CR" : "");
}

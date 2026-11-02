/*
 * replay_sd.h — play a file from the SD card into the terminal: a
 * ttyrec recording with its timing (capture_sd.c makes them; so do
 * ttyrec and asciinema's exporters), or any other file as raw bytes at
 * 9600 baud (ANSI art, logs). Pacing is lib/ttyrec's.
 */
#ifndef REPLAY_SD_H
#define REPLAY_SD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "keys.h"
#include "ttyrec.h"

/*
 * Open `path` and start playing; `newlines` is settings.h's enum
 * replay_newlines (whether a bare LF goes out as CR LF). False, with
 * `error`, if it can't.
 */
bool	replay_sd_start(const char *path, uint32_t now_ms, unsigned newlines,
	    char *error, size_t error_len);
void	replay_sd_stop(void);
bool	replay_sd_active(void);

/*
 * Main loop: hand what's due to `sink`, for at most a few milliseconds
 * (at "max" speed there is always more). Returns false once the file is
 * done (or unreadable), and replay has stopped.
 */
bool	replay_sd_poll(uint32_t now_ms, ttyrec_sink_fn sink, void *ctx);

/*
 * A key while replaying: Space pauses, Up/Down or +/- change speed,
 * n switches LF -> CR LF on or off (for a file that staircases, or one
 * that shouldn't have had it), Esc or q stops. Every key is the
 * replay's (none reach the host). Returns false when it stopped.
 */
bool	replay_sd_key(const struct key_event *key);

/* "Play 0003.rec 2x", "Play x.ans paused +CR", for the status line. */
void	replay_sd_describe(char *out, size_t len);

#endif /* REPLAY_SD_H */

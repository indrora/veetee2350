/*
 * ttyrec.h — the ttyrec recording format, and a player that paces it.
 *
 * Layering: pure module (no hardware, no files; host-testable). The SD
 * card side lives in src/capture_sd.c (writing) and src/replay_sd.c
 * (reading); this only knows the format and the timing.
 *
 * ttyrec (Satoru Takabayashi's, and what ttyplay, ipbt and asciinema's
 * importer read) is a run of records, each a 12-byte header and the
 * bytes the terminal was sent:
 *
 *   uint32 sec, uint32 usec, uint32 len   (all little-endian)
 *   len bytes
 *
 * The time is when the bytes arrived. Players only use differences, so
 * our times are "since boot" (there's no clock to give a date).
 *
 * The player takes file bytes as they're read and hands on the ones
 * that are due, pacing ttyrec records by their times and raw files
 * (ANSI art, logs) at a byte rate, as if down a 9600-baud line. With
 * `onlcr`, a bare LF goes out as CR LF, as a Unix tty's onlcr does when
 * a file is cat'ed: text files and some .ans files end lines with LF
 * alone and staircase without it. It's the caller's choice (SET-UP's
 * "Replay newlines"): LF alone can be meant, as cursor-down. Long
 * pauses in a recording are cut short (TTYREC_MAX_GAP_MS), as ipbt
 * does: nobody wants to watch someone think for a minute.
 */
#ifndef TTYREC_H
#define TTYREC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TTYREC_HEADER		12
#define TTYREC_MAX_RECORD	(1u << 20)	/* longer: not a ttyrec */
#define TTYREC_MAX_GAP_MS	2000		/* pauses longer play as this */
#define TTYREC_RAW_RATE		960		/* raw bytes/s at 1x: 9600 bd */

struct ttyrec_header {
	uint32_t	 sec;
	uint32_t	 usec;
	uint32_t	 len;
};

void	ttyrec_put_header(uint8_t out[TTYREC_HEADER],
	    const struct ttyrec_header *header);
void	ttyrec_get_header(const uint8_t in[TTYREC_HEADER],
	    struct ttyrec_header *header);

/*
 * Does a file starting with `head` (its first `len` bytes, ideally 64+)
 * look like ttyrec? The first header must have a sane length, and if
 * the second header is in `head` too, a sane length and a time no
 * earlier than the first. Anything else plays as raw bytes.
 */
bool	ttyrec_plausible(const uint8_t *head, size_t len);

/* Playback speeds, slowest first; TTYREC_SPEED_MAX feeds as fast as it can. */
#define TTYREC_SPEEDS		8
#define TTYREC_SPEED_1X		2	/* index of 1x */
#define TTYREC_SPEED_MAX	(TTYREC_SPEEDS - 1)
const char *ttyrec_speed_name(unsigned speed);	/* "1/4x" ... "max" */

typedef void (*ttyrec_sink_fn)(const uint8_t *bytes, size_t len, void *ctx);

struct ttyrec_player {
	bool		 ttyrec;	/* else raw bytes at a byte rate */
	bool		 paused;
	bool		 onlcr;		/* bare LF -> CR LF (set by the caller) */
	uint8_t		 speed;		/* index, see ttyrec_speed_name */
	bool		 started;	/* first record's time seen */
	uint8_t		 header[TTYREC_HEADER];
	uint8_t		 header_have;
	uint32_t	 payload_left;	/* of the current record */
	uint64_t	 first_us;	/* the first record's time */
	uint64_t	 due_us;	/* current record, on the play clock */
	uint64_t	 clock_us;	/* play position, recording time */
	uint32_t	 last_ms;	/* wall clock at the last call */
	uint64_t	 played;	/* payload bytes handed on */
	uint8_t		 last;		/* the last byte handed on, for onlcr */
};

void	ttyrec_player_start(struct ttyrec_player *player, bool ttyrec,
	    uint32_t now_ms);

/*
 * Offer `len` file bytes. Hands what's due to `sink` and returns how
 * many of the offered bytes it used (headers included); the rest are
 * offered again next time. Returns 0 while nothing is due, or paused.
 * Call with the same `now_ms` for every offer in one pass of the main
 * loop: the play clock moves once per new `now_ms`.
 */
size_t	ttyrec_player_play(struct ttyrec_player *player, const uint8_t *data,
	    size_t len, uint32_t now_ms, ttyrec_sink_fn sink, void *ctx);

#endif /* TTYREC_H */

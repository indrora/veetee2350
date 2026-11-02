/*
 * capture_sd.c — host output to /capture/NNNN.rec, as ttyrec.
 *
 * Like the printer (printer_sd.c): switching capture on picks a fresh
 * number at the first bytes, and everything until it's switched off
 * goes to that file. The file is closed after a second without output
 * and reopened (appending) on the next bytes, so pulling the card
 * between bursts loses nothing.
 *
 * Bytes collect in a RAM buffer, already framed as ttyrec records, and
 * reach the card when it fills or output goes quiet: SD writes block the
 * main loop for milliseconds, and a record per USB packet would be a
 * write per packet. Bytes arriving within MERGE_MS of a record's start
 * join that record; ttyrec's timing is per record, and 10 ms is finer
 * than anyone can see.
 *
 * Times are milliseconds since boot (there's no clock); players only
 * use the differences.
 */
#include "capture_sd.h"

#include <stdio.h>
#include <string.h>

#include "card.h"
#include "storage.h"
#include "ttyrec.h"

#define CAPTURE_DIR	"/capture"
#define BUFFER_BYTES	4096
#define MERGE_MS	10
#define FLUSH_QUIET_MS	200	/* write out after this long without bytes */
#define FLUSH_HELD_MS	1000	/* ... or once bytes have waited this long */
#define IDLE_CLOSE_MS	1000

static bool enabled;
static char path[24];		/* "" until the first bytes pick one */
static char error[48];
static int handle = -1;

static uint8_t buffer[BUFFER_BYTES];
static size_t buffered;
static bool record_open;	/* the last record in buffer takes more */
static size_t record_at;	/* its header's offset */
static uint32_t record_ms;	/* its time */
static uint32_t first_held_ms;	/* oldest unwritten bytes' time */
static uint32_t last_bytes_ms;

static void
stop(const char *why)
{
	snprintf(error, sizeof(error), "%s", why);
	if (handle >= 0)
		(void)storage_close(handle);
	handle = -1;
	enabled = false;
	buffered = 0;
	record_open = false;
}

/* Write the buffer out. False (and capturing stops) if it can't be. */
static bool
flush(void)
{
	if (buffered == 0)
		return true;
	if (handle < 0) {
		if (path[0] == '\0') {
			static const char *const exts[] = {"rec", nullptr};
			char why[48];
			if (!card_next_name(CAPTURE_DIR, exts, "rec", path,
			    sizeof(path), why, sizeof(why))) {
				path[0] = '\0';
				stop(why);
				return false;
			}
		}
		handle = storage_open(path, STORAGE_APPEND);
		if (handle < 0) {
			stop(storage_error());
			return false;
		}
	}
	if (storage_write(handle, buffer, buffered) != (long)buffered) {
		stop("card full or pulled");
		return false;
	}
	buffered = 0;
	record_open = false;
	return true;
}

/* Lengthen the open record's header by `len` bytes. */
static void
grow_record(size_t len)
{
	struct ttyrec_header header;

	ttyrec_get_header(buffer + record_at, &header);
	header.len += (uint32_t)len;
	ttyrec_put_header(buffer + record_at, &header);
}

void
capture_sd_feed(const uint8_t *bytes, size_t len, uint32_t now_ms)
{
	if (!enabled)
		return;
	if (buffered == 0)
		first_held_ms = now_ms;
	last_bytes_ms = now_ms;
	while (len > 0) {
		if (!record_open || now_ms - record_ms > MERGE_MS) {
			if (BUFFER_BYTES - buffered < TTYREC_HEADER + 1 &&
			    !flush())
				return;
			struct ttyrec_header header = {
				.sec = now_ms / 1000u,
				.usec = now_ms % 1000u * 1000u,
				.len = 0,
			};
			record_at = buffered;
			record_ms = now_ms;
			record_open = true;
			ttyrec_put_header(buffer + buffered, &header);
			buffered += TTYREC_HEADER;
		}
		size_t take = BUFFER_BYTES - buffered;
		if (take > len)
			take = len;
		memcpy(buffer + buffered, bytes, take);
		grow_record(take);
		buffered += take;
		bytes += take;
		len -= take;
		if (buffered == BUFFER_BYTES && !flush())
			return;
	}
}

void
capture_sd_poll(uint32_t now_ms)
{
	if (buffered > 0 && (now_ms - last_bytes_ms > FLUSH_QUIET_MS ||
	    now_ms - first_held_ms > FLUSH_HELD_MS))
		(void)flush();
	if (handle >= 0 && buffered == 0 &&
	    now_ms - last_bytes_ms > IDLE_CLOSE_MS) {
		if (!storage_close(handle))
			stop("card pulled");
		handle = -1;
	}
}

void
capture_sd_enable(bool on)
{
	if (on == enabled)
		return;
	if (!on) {
		(void)flush();
		if (handle >= 0)
			(void)storage_close(handle);
		handle = -1;
	}
	enabled = on;
	path[0] = '\0';		/* next time on: next file */
	error[0] = '\0';
	buffered = 0;
	record_open = false;
}

bool
capture_sd_enabled(void)
{
	return enabled;
}

const char *
capture_sd_name(void)
{
	const char *slash = strrchr(path, '/');

	return slash != nullptr ? slash + 1 : "";
}

const char *
capture_sd_error(void)
{
	return error;
}

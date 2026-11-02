/*
 * printer_sd.c — the VT printer port, printing to the SD card.
 *
 * lib/term turns Media Copy into bytes (lib/term/printer.c); this is
 * where they land: /print/NNNN.txt. Each time the printer is switched
 * on (SET-UP, or boot with it on) it gets a fresh number on its first
 * job, like loading new paper; everything until it's switched off
 * appends to that file.
 *
 * The file is opened on demand and closed at the end of each job (print
 * screen, print line, a controller-mode session), so pulling the card
 * between jobs loses nothing. Auto print has no "end of job": it's
 * closed after a second without output instead (printer_sd_poll).
 */
#include "printer_sd.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "pico/time.h"
#include "storage.h"

#define PRINT_DIR	"/print"
#define IDLE_CLOSE_MS	1000

static bool enabled;
static char path[24];		/* empty until the first job picks one */
static int handle = -1;
static uint32_t last_write_ms;

static uint32_t
now_ms(void)
{
	return to_ms_since_boot(get_absolute_time());
}

static bool
note_number(const char *name, uint32_t size, bool is_dir,
    const uint8_t *head, size_t head_len, void *ctx)
{
	unsigned *highest = ctx;
	unsigned number = 0;

	(void)size;
	(void)head;
	(void)head_len;
	board_watchdog_feed();
	if (is_dir || strlen(name) != 8 || strcmp(name + 4, ".txt") != 0)
		return true;
	for (int i = 0; i < 4; i++) {
		if (name[i] < '0' || name[i] > '9')
			return true;
		number = number * 10 + (unsigned)(name[i] - '0');
	}
	if (number > *highest)
		*highest = number;
	return true;
}

static bool
open_paper(void)
{
	if (handle >= 0)
		return true;
	if (!storage_mount())
		return false;
	if (path[0] == '\0') {
		unsigned highest = 0;
		if (!storage_mkdir(PRINT_DIR))
			return false;
		(void)storage_list(PRINT_DIR, 0, note_number, &highest);
		if (highest >= 9999)
			return false;
		snprintf(path, sizeof(path), PRINT_DIR "/%04u.txt",
		    highest + 1);
	}
	handle = storage_open(path, STORAGE_APPEND);
	return handle >= 0;
}

static void
close_paper(void)
{
	if (handle >= 0)
		(void)storage_close(handle);
	handle = -1;
}

static bool
sd_write(const uint8_t *bytes, size_t len, void *ctx)
{
	(void)ctx;
	if (!enabled || !open_paper())
		return false;
	last_write_ms = now_ms();
	if (storage_write(handle, bytes, len) == (long)len)
		return true;
	close_paper();		/* card full or pulled: try afresh next time */
	return false;
}

static void
sd_job_done(void *ctx)
{
	(void)ctx;
	close_paper();
}

static enum term_printer_status
sd_status(void *ctx)
{
	(void)ctx;
	if (!enabled)
		return TERM_PRINTER_NONE;
	return storage_mount() ? TERM_PRINTER_READY : TERM_PRINTER_NOT_READY;
}

static const struct term_printer sd_printer = {
	.write = sd_write,
	.job_done = sd_job_done,
	.status = sd_status,
};

void
printer_sd_enable(struct term *term, bool on)
{
	if (on == enabled)
		return;
	close_paper();
	enabled = on;
	path[0] = '\0';		/* switched on: next job, next file */
	term_set_printer(term, on ? &sd_printer : nullptr);
}

void
printer_sd_poll(void)
{
	if (handle >= 0 && now_ms() - last_write_ms > IDLE_CLOSE_MS)
		close_paper();
}

const char *
printer_sd_path(void)
{
	return path;
}

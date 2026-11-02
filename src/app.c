/*
 * app.c — application glue. Goal 1 (in progress): a working terminal.
 *
 * Bytes from the host arrive on the line port (hal/usbio.h) and go into
 * the terminal emulator (lib/term: libvterm -> cell grid), which the video
 * pipeline draws. Replies (DA, DSR, ...) and keystrokes go back out on the
 * line port. Keystrokes arrive on the keyboard port.
 */
#include "app.h"

#include <stdint.h>

#include "usbio.h"
#include "font.h"
#include "render.h"
#include "term.h"
#include "video.h"

#include "pico.h"
#include "pico/time.h"

static_assert(RENDER_WIDTH == VIDEO_WIDTH && RENDER_HEIGHT == VIDEO_HEIGHT);

static struct render render;
static struct term term;

static void
send_to_host(const char *bytes, size_t len, void *ctx)
{
	(void)ctx;
	usbio_write(USBIO_LINE, bytes, len);
}

void
app_init(void)
{
	if (!term_init(&term, &font_builtins[0], &render, send_to_host,
	    nullptr))
		panic("term: cannot start (font or memory)");
	video_init(render_scanline, &render);
	render.live = true;	/* from now on, render_pause must wait */

	static const char banner[] =
	    "\x1b[1;33mVeeTee 2350\x1b[0m ready (80x30, notjam-clean-16)\r\n";
	term_feed(&term, (const uint8_t *)banner, sizeof(banner) - 1);
}

#ifdef VEETEE_SERIAL_STATS
/*
 * Debug builds only (-DVEETEE_SERIAL_STATS): print scanout statistics to
 * the host once a second. They go out on the same serial port as the
 * terminal's replies, so don't run an interactive session at the same
 * time; tools use them to measure the render/encode budget under load.
 */
static void
report_stats(void)
{
	static absolute_time_t next_report;
	struct video_stats stats;

	if (!time_reached(next_report))
		return;
	next_report = make_timeout_time_ms(1000);
	video_get_stats(&stats);
	usbio_printf("\r\n[stats] frames=%lu encode avg=%lu max=%lu "
	    "render avg=%lu max=%lu starved=%lu budget=%u\r\n",
	    (unsigned long)stats.frames, (unsigned long)stats.avg_encode_cycles,
	    (unsigned long)stats.max_encode_cycles,
	    (unsigned long)stats.avg_render_cycles,
	    (unsigned long)stats.max_render_cycles,
	    (unsigned long)stats.starved, VIDEO_LINE_BUDGET_CYCLES);
}
#endif

void
app_poll(void)
{
	char buffer[256];
	size_t count = usbio_read(USBIO_LINE, buffer, sizeof(buffer));

	if (count > 0)
		term_feed(&term, (const uint8_t *)buffer, count);

	/* Keys typed on the terminal go to the host over the line. */
	count = usbio_read(USBIO_KEYBOARD, buffer, sizeof(buffer));
	if (count > 0)
		term_send(&term, buffer, count);
#ifdef VEETEE_SERIAL_STATS
	report_stats();
#endif
}

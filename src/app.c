/*
 * app.c — application glue. Goal 1 (in progress): a working terminal.
 *
 * Bytes from the host arrive on the USB CDC serial port and go straight
 * into the terminal emulator (lib/term: libvterm -> cell grid); core 1
 * renders the grid to DVI. The terminal's replies (DA, DSR, ...) go back
 * out on the same port. The serial port is the "line" here: the host runs
 * a shell or program attached to it.
 */
#include "app.h"

#include <stdint.h>

#include "console.h"
#include "font.h"
#include "render.h"
#include "term.h"
#include "video.h"

#include "pico.h"
#include "pico/time.h"

static_assert(RENDER_WIDTH == VIDEO_WIDTH && RENDER_HEIGHT == VIDEO_HEIGHT);

static struct font font;
static struct render render;
static struct term term;

static void
send_to_host(const char *bytes, size_t len, void *ctx)
{
	(void)ctx;
	console_write(bytes, len);
}

void
app_init(void)
{
	const struct font_builtin *builtin = &font_builtins[0];

	if (font_load_psf2(&font, builtin->data, builtin->len) != FONT_OK)
		panic("font: cannot load %s", builtin->name);
	if (!term_init(&term, &font, &render, send_to_host, nullptr))
		panic("term: out of memory");
	video_init(render_scanline, &render);

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
	console_printf("\r\n[stats] frames=%lu encode avg=%lu max=%lu "
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
	size_t count = console_read(buffer, sizeof(buffer));

	if (count > 0)
		term_feed(&term, (const uint8_t *)buffer, count);
#ifdef VEETEE_SERIAL_STATS
	report_stats();
#endif
}

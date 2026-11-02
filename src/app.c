/*
 * app.c — application glue: USB ports, terminal, SET-UP screen.
 *
 * Bytes from the host arrive on the line port (hal/usbio.h) and go into
 * the terminal emulator (lib/term: libvterm -> cell grid), which the video
 * pipeline draws. Replies (DA, DSR, ...) go back out on the line port.
 *
 * Keys arrive on the keyboard port and pass through a decoder (lib/input):
 * normally they're forwarded to the host unchanged (with LNM and local
 * echo applied), except F3, which opens SET-UP like a VT220's Set-Up key.
 * The BOOTSEL button does the same. While SET-UP is open, keys drive the
 * menu instead, and its own grid replaces the terminal's on screen; the
 * terminal keeps processing host output underneath.
 */
#include "app.h"

#include <stdint.h>
#include <string.h>

#include "board.h"
#include "flashcfg.h"
#include "font.h"
#include "keys.h"
#include "render.h"
#include "settings.h"
#include "setup.h"
#include "term.h"
#include "usbio.h"
#include "video.h"

#include "pico.h"
#include "pico/time.h"

static_assert(RENDER_WIDTH == VIDEO_WIDTH && RENDER_HEIGHT == VIDEO_HEIGHT);
static_assert(sizeof(struct settings) <= FLASHCFG_MAX);

#define BOOTSEL_POLL_MS		50
#define VISUAL_BELL_MS		100

static struct render render;
static struct term term;
static struct settings settings;	/* in effect now */
static struct keys keys;

static bool in_setup;
static struct setup setup;
static struct grid setup_grid;

static uint32_t bells_seen;
static absolute_time_t bell_until;
static bool bell_flashing;

static void
send_to_host(const char *bytes, size_t len, void *ctx)
{
	(void)ctx;
	usbio_write(USBIO_LINE, bytes, len);
}

static uint32_t
now_ms(void)
{
	return to_ms_since_boot(get_absolute_time());
}

static const struct font_builtin *
settings_font(const struct settings *from)
{
	const struct font_builtin *builtin = font_builtin_find(from->font);

	return builtin != nullptr ? builtin : &font_builtins[0];
}

/* Make the terminal match `settings` (resets it if geometry changed). */
static void
apply_settings(void)
{
	struct term_options options = {
		.max_rows = settings.rows,
		.cursor_shape = settings.cursor_shape,
		.cursor_blink = settings.cursor_blink,
		.autowrap = settings.autowrap,
		.newline_mode = settings.newline_mode,
	};

	settings_scheme_rgb(settings.scheme, options.fg_rgb, options.bg_rgb);
	memcpy(options.answerback, settings.answerback,
	    sizeof(options.answerback));
	if (!term_configure(&term, settings_font(&settings), &options))
		panic("term: cannot apply settings (memory)");
}

static void
load_settings(void)
{
	flashcfg_read(&settings, sizeof(settings));
	if (!settings_valid(&settings))
		settings_defaults(&settings);
}

/* --- SET-UP ----------------------------------------------------------- */

static void
enter_setup(void)
{
	struct grid *screen = term.screen;
	struct cell blank = grid_make_cell(term.space_glyph, 0xffff, 0, 0);

	if (!grid_init(&setup_grid, screen->cols, screen->rows, blank))
		return;		/* no memory: stay in the terminal */
	setup_grid.cursor_visible = false;
	setup_grid.border2 = grid_dup565(grid_rgb565(16, 40, 120));
	setup_begin(&setup, &settings, &setup_grid, &term.font);
	render_set_grid(&render, &setup_grid);
	in_setup = true;
}

static void
leave_setup(void)
{
	in_setup = false;
	render_set_grid(&render, term.screen);
	settings = setup.work;
	settings_seal(&settings);
	/* render_pause (inside a font change) can't race the grid free:
	 * the renderer was pointed back at the terminal first. */
	apply_settings();
	render_pause(&render);	/* make sure no line still reads setup_grid */
	render_init(&render, &term.font, term.screen);
	grid_free(&setup_grid);
}

static void
setup_handle(const struct key_event *key)
{
	struct settings stored;

	switch (setup_key(&setup, key)) {
	case SETUP_EXIT:
		leave_setup();
		break;
	case SETUP_SAVE:
		settings_seal(&setup.work);
		setup_message(&setup, flashcfg_write(&setup.work,
		    sizeof(setup.work)) ? "Saved. Applied on exit." :
		    "Save FAILED.");
		break;
	case SETUP_RECALL:
		flashcfg_read(&stored, sizeof(stored));
		if (settings_valid(&stored)) {
			setup.work = stored;
			setup_message(&setup, "Recalled saved settings.");
		} else {
			setup_message(&setup, "Nothing saved yet.");
		}
		break;
	case SETUP_DEFAULTS:
		settings_defaults(&setup.work);
		setup_message(&setup, "Factory defaults loaded (not saved).");
		break;
	case SETUP_RESET_TERMINAL:
		term_reset(&term);
		setup_message(&setup, "Terminal reset.");
		break;
	case SETUP_NONE:
		break;
	}
}

/* --- keyboard path ---------------------------------------------------- */

static void
forward_key(const struct key_event *key)
{
	/* LNM: Enter sends CR LF. Only a bare CR is translated. */
	if (settings.newline_mode && key->code == KEY_ENTER &&
	    key->raw_len == 1 && key->raw[0] == '\r') {
		term_send(&term, "\r\n", 2);
		if (settings.local_echo)
			term_feed(&term, (const uint8_t *)"\r\n", 2);
		return;
	}
	term_send(&term, (const char *)key->raw, key->raw_len);
	if (settings.local_echo)
		term_feed(&term, key->raw, key->raw_len);
}

static void
handle_key(const struct key_event *key)
{
	if (in_setup)
		setup_handle(key);
	else if (key->code == KEY_F3)
		enter_setup();
	else
		forward_key(key);
}

static void
poll_keyboard(void)
{
	char buffer[64];
	struct key_event key;
	size_t count = usbio_read(USBIO_KEYBOARD, buffer, sizeof(buffer));

	for (size_t i = 0; i < count; i++)
		if (keys_feed(&keys, (uint8_t)buffer[i], now_ms(), &key))
			handle_key(&key);
	if (keys_poll(&keys, now_ms(), &key))
		handle_key(&key);
}

static void
poll_bootsel(void)
{
	static absolute_time_t next_poll;
	static bool was_pressed;

	if (!time_reached(next_poll) || !video_flash_safe())
		return;
	next_poll = make_timeout_time_ms(BOOTSEL_POLL_MS);
	bool pressed = board_bootsel_pressed();
	if (pressed && !was_pressed) {
		if (in_setup)
			leave_setup();
		else
			enter_setup();
	}
	was_pressed = pressed;
}

/* Visual bell: invert the screen briefly on BEL. */
static void
poll_bell(void)
{
	if (term.bells != bells_seen) {
		bells_seen = term.bells;
		if (settings.visual_bell && !in_setup && !bell_flashing) {
			term.screen->reverse_video = !term.reverse_video;
			bell_flashing = true;
			bell_until = make_timeout_time_ms(VISUAL_BELL_MS);
		}
	}
	if (bell_flashing && time_reached(bell_until)) {
		term.screen->reverse_video = term.reverse_video;
		bell_flashing = false;
	}
}

#ifdef VEETEE_SERIAL_STATS
/*
 * Debug builds only (-DVEETEE_SERIAL_STATS): print scanout statistics to
 * the host once a second, on the line port.
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
app_init(void)
{
	load_settings();
	keys_init(&keys);
	if (!term_init(&term, settings_font(&settings), &render, send_to_host,
	    nullptr))
		panic("term: cannot start (font or memory)");
	apply_settings();
	video_init(render_scanline, &render);
	render.live = true;	/* from now on, render_pause must wait */

	static const char banner[] =
	    "\x1b[1mVeeTee 2350\x1b[0m ready. F3 or BOOTSEL: SET-UP\r\n";
	term_feed(&term, (const uint8_t *)banner, sizeof(banner) - 1);
}

void
app_poll(void)
{
	char buffer[256];
	size_t count = usbio_read(USBIO_LINE, buffer, sizeof(buffer));

	if (count > 0)
		term_feed(&term, (const uint8_t *)buffer, count);
	poll_keyboard();
	poll_bootsel();
	poll_bell();
#ifdef VEETEE_SERIAL_STATS
	report_stats();
#endif
}

/*
 * app.c — application glue: USB ports, terminal, SET-UP screen.
 *
 * Bytes from the host arrive on the line port (hal/usbio.h) and go into
 * the terminal emulator (lib/term: libvterm -> cell grid), which the video
 * pipeline draws. Replies (DA, DSR, ...) go back out on the line port.
 *
 * Keys come from a USB keyboard on the board's own port (lib/hal/
 * usbhost.h, lib/input/hidkbd.h), or in debug builds from a script on the
 * host through the automation HID (poll_automation). They're sent to the
 * host as the terminal's modes say (term_key_bytes), except hotkeys
 * (settings.hotkeys): Ctrl+F3 opens the Tools menu (lib/setup/menu.h),
 * F12 takes a screenshot. A tap on the BOOTSEL button opens SET-UP;
 * holding it 5 s resets every setting to factory defaults
 * (factory_reset). The Tools menu is an overlay over the live screen.
 * While SET-UP is open, keys drive the menu instead, and its own grid
 * replaces the terminal's on screen; the terminal keeps processing host
 * output underneath.
 */
#include "app.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "automation.h"
#include "automation_hid.h"
#include "board.h"
#include "build.h"
#include "button.h"
#include "capture_sd.h"
#include "card.h"
#include "flashcfg.h"
#include "font.h"
#include "hidkbd.h"
#include "keys.h"
#include "render.h"
#include "menu.h"
#include "picker.h"
#include "printer_sd.h"
#include "replay_sd.h"
#include "screenshot.h"
#include "serial.h"
#include "settings.h"
#include "storage.h"
#include "setup.h"
#include "splash.h"
#include "term.h"
#include "usbhost.h"
#include "usbio.h"
#include "video.h"

#include "pico.h"
#include "pico/time.h"
#include "hardware/irq.h"
#include "hardware/structs/watchdog.h"

static_assert(sizeof(struct settings) <= FLASHCFG_MAX);

#define BOOTSEL_POLL_MS		50
#define BOOTSEL_HOLD_MS		1000	/* down this long: a hold, not a tap */
#define BOOTSEL_RESET_MS	5000	/* held this long: factory reset */
#define BOOT_HEALTHY_MS		60000	/* then the crash count clears */
#define CRASHES_MAX		2	/* in a row: boot with factory defaults */
#define VISUAL_BELL_MS		100
#define SPLASH_MS		3000	/* the boot splash, from first picture */

static struct render render;
static struct term term;
static struct settings settings;	/* in effect now */
static unsigned running_resolution;	/* video mode chosen at boot */
static struct button bootsel;		/* tap: SET-UP; hold: factory reset */

static bool in_setup;
static struct setup setup;
static struct grid setup_grid;
/*
 * SET-UP's screen: at most 80x30, in RAM of its own. It used to be one
 * terminal screen's worth from the heap, which after a save that rebuilt
 * the terminal (new font, size, colours) could be there in total but
 * not in one piece: SET-UP then wouldn't open again until a restart
 * (devlog 028). SET-UP is the way out of bad settings, so it mustn't
 * depend on the heap: 19 KB, always there. On a bigger screen it's a
 * centred 80x30 panel, as on a VT220, and its list scrolls.
 */
#define SETUP_COLS	80
#define SETUP_ROWS	30
static struct cell setup_cells[SETUP_COLS * SETUP_ROWS];

/* Configuration Error screen: problems collected while applying. */
static char problems[512];
static size_t problems_len;
static bool in_config_error;

/*
 * The boot splash (lib/render/splash.h): a small test card for the first
 * SPLASH_MS, drawn instead of the terminal, because some capture cards
 * lock onto a busy picture far more readily than onto a fresh boot's
 * nearly-black screen (devlog 032). The terminal runs underneath all the
 * while -- banner, host output and all -- and simply appears when the
 * splash ends, so nothing waits on it.
 */
static struct splash splash;
static bool splash_up;
static uint32_t splash_until;

static uint32_t bells_seen;
static absolute_time_t bell_until;
static bool bell_flashing;

/*
 * The communication line: the USB CDC port or the header's UART
 * (settings.line_port, applied by line_apply). Everything that talks to
 * the host goes through these two, so switching ports is one flag.
 */
static bool line_on_uart;

static size_t
line_read(char *buf, size_t cap)
{
	return line_on_uart ? serial_read((uint8_t *)buf, cap) :
	    usbio_read(USBIO_LINE, buf, cap);
}

static void
line_write(const char *buf, size_t len)
{
	if (line_on_uart)
		serial_write((const uint8_t *)buf, len);
	else
		usbio_write(USBIO_LINE, buf, len);
}

/* Start the port the settings name, with their format; stop the other. */
static void
line_apply(void)
{
	if (settings.line_port != LINE_UART) {
		serial_end();
		line_on_uart = false;
		return;
	}
	struct serial_format format = {
		.baud = settings_bauds[settings.line_baud % SETTINGS_BAUDS],
		.data_bits = settings.line_data_bits,
		.parity = settings.line_parity,
		.stop_bits = settings.line_stop_bits,
		.rts_cts = settings.line_flow == FLOW_RTS_CTS,
	};
	serial_begin(&format);
	line_on_uart = true;
}

static void
send_to_host(const char *bytes, size_t len, void *ctx)
{
	(void)ctx;
	/* A replayed file's queries (DA, DSR) get answers the host never
	 * asked for: drop them. */
	if (replay_sd_active())
		return;
	line_write(bytes, len);
}

static uint32_t
now_ms(void)
{
	return to_ms_since_boot(get_absolute_time());
}

/* --- Configuration Error ---------------------------------------------- */

/* Note a problem ("/fonts/x.psf", "not a PSF2 font") for the screen. */
static void
config_problem(const char *what, const char *why)
{
	int len = snprintf(problems + problems_len,
	    sizeof(problems) - problems_len, "  \x1b[1m%s\x1b[22m\r\n    %s\r\n",
	    what, why);

	if (len > 0 && problems_len + (size_t)len < sizeof(problems))
		problems_len += (size_t)len;
}

/*
 * If anything went wrong, take over the screen: white on red, a
 * double-height "Configuration Error", each problem and its reason.
 * Host output is held (not read) until a key or BOOTSEL dismisses it,
 * so nothing scribbles over the message.
 */
static void
show_config_problems(void)
{
	static const char head[] =
	    "\x1b[?25l\x1b[0;1;97;41m\x1b[2J\x1b[H\r\n"
	    "\x1b#3  Configuration Error\r\n"
	    "\x1b#4  Configuration Error\r\n\x1b[22m\r\n";
	static const char tail[] =
	    "\r\n  Using the defaults for these instead. Saved settings are\r\n"
	    "  unchanged until you save in SET-UP.\r\n\r\n"
	    "  \x1b[1mPress any key to continue.\x1b[22m";

	if (problems_len == 0)
		return;
	term_feed(&term, (const uint8_t *)head, sizeof(head) - 1);
	term_feed(&term, (const uint8_t *)problems, problems_len);
	term_feed(&term, (const uint8_t *)tail, sizeof(tail) - 1);
	problems_len = 0;
	in_config_error = true;
}

static void
dismiss_config_error(void)
{
	static const char clear[] = "\x1b[0m\x1b[2J\x1b[H\x1b[?25h";

	in_config_error = false;
	term_feed(&term, (const uint8_t *)clear, sizeof(clear) - 1);
}

/* --- settings --------------------------------------------------------- */

/*
 * The font `settings` asks for. A card font ("sd:name") is read from
 * /fonts; if that fails, the problem is noted, the setting reverts to
 * the default for this session, and the default font is used.
 */
static const struct font_builtin *
settings_font(void)
{
	const struct font_builtin *builtin = font_builtin_find(settings.font);
	size_t prefix = strlen(SETTINGS_CARD_PREFIX);
	char error[64], what[96];

	if (builtin != nullptr)
		return builtin;
	if (strncmp(settings.font, SETTINGS_CARD_PREFIX, prefix) == 0) {
		builtin = card_font(settings.font + prefix, term.font_source,
		    error, sizeof(error));
		if (builtin != nullptr)
			return builtin;
		snprintf(what, sizeof(what), "Font /fonts/%.32s.psf",
		    settings.font + prefix);
		config_problem(what, error);
	} else {
		config_problem("Font", "unknown built-in font name");
	}
	snprintf(settings.font, sizeof(settings.font), "%s",
	    font_builtins[0].name);
	return &font_builtins[0];
}

/* Colours from the scheme setting; a card scheme falls back the same way. */
static void
settings_colours(struct term_options *options)
{
	struct base16_terminal colours;
	char error[64], what[96];

	if (settings.scheme_file[0] != '\0') {
		if (card_scheme(settings.scheme_file, &colours, error,
		    sizeof(error))) {
			memcpy(options->fg_rgb, colours.fg, 3);
			memcpy(options->bg_rgb, colours.bg, 3);
			memcpy(options->ansi, colours.ansi, sizeof(options->ansi));
			options->ansi_set = true;
			return;
		}
		snprintf(what, sizeof(what), "Colour scheme /schemes/%s.yaml",
		    settings.scheme_file);
		config_problem(what, error);
		settings.scheme_file[0] = '\0';
		settings.scheme = SCHEME_XTERM;
	}
	settings_scheme_rgb(settings.scheme, options->fg_rgb, options->bg_rgb);
	options->ansi_set = false;
}

/* settings.h's status modes are term's, by number. */
static_assert((int)STATUS_HIDDEN == (int)TERM_STATUS_HIDDEN &&
    (int)STATUS_BOTTOM == (int)TERM_STATUS_BOTTOM &&
    (int)STATUS_TOP == (int)TERM_STATUS_TOP &&
    (int)STATUS_PEEK_BOTTOM == (int)TERM_STATUS_PEEK_BOTTOM &&
    (int)STATUS_PEEK_TOP == (int)TERM_STATUS_PEEK_TOP &&
    (int)STATUS_MODE_COUNT == (int)TERM_STATUS_MODES,
    "status line modes differ");

/* The status line's standing text: what's switched on or running. */
static void
show_state(void)
{
	char text[STATUS_TEXT_MAX] = "", part[STATUS_TEXT_MAX];
	size_t len = 0;

	if (replay_sd_active()) {
		replay_sd_describe(part, sizeof(part));
		len += (size_t)snprintf(text + len, sizeof(text) - len, "%s  ",
		    part);
	}
	if (settings.capture && len < sizeof(text))
		len += (size_t)snprintf(text + len, sizeof(text) - len,
		    "Rec %s  ", capture_sd_name()[0] != '\0' ?
		    capture_sd_name() : "on");
	if (settings.printer && len < sizeof(text))
		len += (size_t)snprintf(text + len, sizeof(text) - len,
		    "Printer  ");
	if (len > sizeof(text) - 1)
		len = sizeof(text) - 1;
	while (len > 0 && text[len - 1] == ' ')
		text[--len] = '\0';
	term_status_text(&term, text);
}

/* Host bytes (and their local echo) to the screen, and to a capture. */
static void
show_bytes(const uint8_t *bytes, size_t len)
{
	term_feed(&term, bytes, len);
	capture_sd_feed(bytes, len, now_ms());
}

static size_t
line_available(void)
{
	return line_on_uart ? serial_available() :
	    usbio_available(USBIO_LINE);
}

/*
 * Host output to the screen without tearing (devlog 031). The render
 * IRQ reads the grid a few lines ahead of the picture; output fed while
 * it does lands in the middle of a frame, the top of the screen showing
 * one moment and the rest the next. So output is fed only while the grid
 * is idle (video.h, video_in_gap): the vertical blanking interval,
 * about 1.2-1.7 ms a frame, in chunks small enough that one straddling its end
 * (the next line read) barely shows.
 *
 * The line keeps arriving meanwhile: the UART ring (8 KiB) and the CDC
 * port (whose host simply waits) hold it. When that backlog passes
 * FEED_BACKLOG -- a flood faster than the gaps can take, a `cat` of a
 * large file over USB -- output is fed mid-frame too, a pass at a time,
 * trading tearing for throughput.
 */
#define FEED_CHUNK	64
#define FEED_BACKLOG	2048
#define FEED_BUSY_MAX	256	/* bytes a pass, mid-frame */

/* Running totals for the stats build's [feed] line. */
static uint32_t feed_idle_bytes, feed_busy_bytes, feed_frames;
static uint32_t feed_passes, feed_idle_passes;

static void
feed_line(void)
{
	char chunk[FEED_CHUNK];
	size_t busy = 0;
	bool backlog = line_available() > FEED_BACKLOG;
	static uint32_t fed_frame;

	feed_passes++;
	if (video_in_gap())
		feed_idle_passes++;

	while (video_in_gap() || (backlog && busy < FEED_BUSY_MAX)) {
		bool idle = video_in_gap();
		size_t count = line_read(chunk, sizeof(chunk));

		if (count == 0)
			break;
		show_bytes((const uint8_t *)chunk, count);
		if (idle) {
			feed_idle_bytes += count;
			if (fed_frame != render.frame) {
				fed_frame = render.frame;
				feed_frames++;
			}
		} else {
			feed_busy_bytes += count;
			busy += count;
		}
	}
}

/* Make the terminal match `settings` (resets it if geometry changed). */
static void
apply_settings(void)
{
	const struct settings_size *size =
	    &settings_sizes[settings.term_size % SIZE_COUNT];
	struct term_options options = {
		.max_rows = size->rows,
		.max_cols = size->cols,
		.cursor_shape = settings.cursor_shape,
		.cursor_blink = settings.cursor_blink,
		.autowrap = settings.autowrap,
		.newline_mode = settings.newline_mode,
	};

	options.emulation = settings.emulation;
	options.colour_depth = settings.colour_depth;
	options.utf8 = settings.utf8;
	options.alt_screen = settings.alt_screen;
	options.sixel = settings.sixel != 0;
	options.sixel_bpp = settings.sixel != 0 ? settings.sixel : 8;
	settings_colours(&options);
	memcpy(options.answerback, settings.answerback,
	    sizeof(options.answerback));
	options.status_mode = settings.status_line % STATUS_MODE_COUNT;
	options.peek_seconds = settings_peek_seconds[settings.peek_time %
	    SETTINGS_PEEK_TIMES];
	if (!term_configure(&term, settings_font(), &options))
		panic("term: cannot apply settings (memory)");
	printer_sd_enable(&term, settings.printer != 0);
	capture_sd_enable(settings.capture != 0);
	line_apply();
	show_state();
}

/* Where this boot's settings came from, for the boot banner. */
static char settings_origin[40];

static void
load_settings(void)
{
	struct settings stored;

	flashcfg_read(&stored, sizeof(stored));
	if (settings_valid(&stored)) {
		settings = stored;
		snprintf(settings_origin, sizeof(settings_origin), "saved");
	} else if (settings_upgrade(&settings, &stored)) {
		snprintf(settings_origin, sizeof(settings_origin),
		    "upgraded from v%u", stored.version);
	} else {
		settings_defaults(&settings);
		/* Say what was there, to tell "never saved" from "lost". */
		if (stored.magic == SETTINGS_MAGIC)
			snprintf(settings_origin, sizeof(settings_origin),
			    "defaults (v%u record unreadable)",
			    stored.version);
		else
			snprintf(settings_origin, sizeof(settings_origin),
			    "defaults (none saved)");
	}
	/* An upgraded record isn't written back: SET-UP's Save does that. */
}

/*
 * Most heap Sixel tiles may take: 256 KB is ~2000 8x16 cells, 85% of an
 * 80x30 screen. The pool gets less when less is free (720p grids are
 * big); see lib/term/graphics.h.
 */
#define GRAPHICS_BUDGET	(256u * 1024u)

/* --- SET-UP ----------------------------------------------------------- */

/* Probe the card and put what we found in SET-UP's title row. */
static void
show_storage_status(void)
{
	struct storage_info info;
	char text[40];

	if (storage_mount() && storage_info(&info)) {
		unsigned tenths = (unsigned)(info.bytes / 100000000u);
		snprintf(text, sizeof(text), "SD: %u.%u GB %s", tenths / 10,
		    tenths % 10, info.fat_type == 64 ? "exFAT" :
		    info.fat_type == 32 ? "FAT32" : "FAT");
	} else {
		snprintf(text, sizeof(text), "SD: %s", storage_error());
	}
	setup_storage(&setup, text);
}

/* card_scan progress -> SET-UP's message line, redrawn at once. */
static void
scan_progress(const char *text, void *ctx)
{
	(void)ctx;
	setup_message(&setup, text);
}

/*
 * Put the terminal back on the screen. render.live goes true only now:
 * render_pause waits for the renderer's frame counter to move, and it
 * doesn't while the splash draws, so until here a pause must not wait.
 */
static void
end_splash(void)
{
	if (!splash_up)
		return;
	splash_up = false;
	video_set_scanline(render_scanline, &render);
	render.live = true;
	/*
	 * The splash's pixels were in SET-UP's screen (setup_cells): wipe
	 * them. video_set_scanline holds off the render IRQ, so the line it
	 * might have been drawing finished before the swap; lines after it
	 * are the terminal's.
	 */
	memset(setup_cells, 0, sizeof(setup_cells));
}

static void
enter_setup(void)
{
	end_splash();	/* SET-UP is never hidden behind it */

	struct grid *screen = term.screen;
	struct cell blank = grid_make_cell(term.space_glyph, 0xffff, 0, 0);
	/* No bigger than the terminal's grid: that's known to fit. */
	unsigned cols = screen->cols < SETUP_COLS ? screen->cols : SETUP_COLS;
	unsigned rows = screen->rows < SETUP_ROWS ? screen->rows : SETUP_ROWS;

	if (!grid_init_in(&setup_grid, cols, rows, setup_cells, blank)) {
		/* Can't happen (sizes are capped), but never silently. */
		term_status_message(&term, "SET-UP won't open", now_ms());
		return;
	}
	setup_grid.cursor_visible = false;
	setup_grid.border2 = grid_dup565(grid_rgb565(16, 40, 120));
	setup_begin(&setup, &settings, &setup_grid, &term.font,
	    running_resolution);
	/*
	 * Shown first, so the card scan's progress is visible. Laid out
	 * afresh, not just swapped in (render_set_grid): SET-UP's grid can
	 * be smaller than the terminal's (80x30 at most), so it needs its
	 * own centring and row map. Swapped in alone it sat top-left, with
	 * its title row repeated down the rest of the screen.
	 */
	render_pause(&render);
	render_init(&render, &term.font, &setup_grid);
	in_setup = true;
	show_storage_status();
	struct setup_catalogue catalogue;
	(void)card_scan(&catalogue, scan_progress, nullptr);
	setup_catalogue(&setup, &catalogue);
}

/*
 * Close SET-UP. `apply`: put its settings in effect (nothing unsaved is
 * left by then: setup_exit asks first). Otherwise it's "discard": the
 * settings stay exactly as they were, as if SET-UP never opened.
 */
static void
leave_setup(bool apply)
{
	in_setup = false;
	/*
	 * Back to the terminal's grid and its own layout in one go: drawn
	 * with SET-UP's (smaller, centred) layout, a wider terminal grid
	 * would run past the end of the scanline.
	 */
	render_pause(&render);
	render_init(&render, &term.font, term.screen);
	if (apply) {
		settings = setup.work;
		settings_seal(&settings);
	}

	/*
	 * A new resolution means a new sysclk, VREG and DVI timing, which are
	 * only set at boot (video_init), so restart. It's saved by now: SET-UP
	 * only lets a new mode out through a save (F4, Y, or S then F3), so
	 * the restart comes back in it. Leaving never saves by itself; it
	 * used to, for a new mode, until that caught the user out.
	 */
	if (settings.resolution != running_resolution)
		board_reboot();

	/*
	 * Paused again while settings apply: a font or size change rebuilds
	 * the terminal's grids under the renderer, and render_init resumes
	 * on whatever they become. (setup_grid's cells are its own, in
	 * setup_cells, so there's nothing of SET-UP's to free.)
	 */
	render_pause(&render);
	card_release();		/* SET-UP's lists: memory for a card font */
	apply_settings();
	render_init(&render, &term.font, term.screen);
	show_config_problems();
}

/* Carry out what SET-UP asked for (a key, or a BOOTSEL tap). */
static void
setup_act(enum setup_action action)
{
	struct settings stored;

	switch (action) {
	case SETUP_EXIT:
		leave_setup(true);
		break;
	case SETUP_DISCARD:
		leave_setup(false);
		break;
	case SETUP_SAVE_EXIT:
		settings_seal(&setup.work);
		if (!flashcfg_write(&setup.work, sizeof(setup.work))) {
			setup_message(&setup, "Save FAILED.");
			break;
		}
		leave_setup(true);
		break;
	case SETUP_SAVE:
		settings_seal(&setup.work);
		if (flashcfg_write(&setup.work, sizeof(setup.work))) {
			setup_saved(&setup);
			setup_message(&setup, "Saved. Applied on exit.");
		} else {
			setup_message(&setup, "Save FAILED.");
		}
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

static void
setup_handle(const struct key_event *key)
{
	setup_act(setup_key(&setup, key));
}

/* --- keyboard path ---------------------------------------------------- */

static void
forward_key(const struct key_event *key)
{
	/*
	 * A USB keyboard's key (no raw bytes: hidkbd.h): the terminal is
	 * the one that encodes it, by its modes, LNM included.
	 */
	if (key->raw_len == 0) {
		uint8_t bytes[KEYS_RAW_MAX];
		size_t len = term_key_bytes(&term, key, bytes, sizeof(bytes));
		term_send(&term, (const char *)bytes, len);
		if (settings.local_echo)
			show_bytes(bytes, len);
		return;
	}
	/* LNM: Enter sends CR LF. Only a bare CR is translated. */
	if (settings.newline_mode && key->code == KEY_ENTER &&
	    key->raw_len == 1 && key->raw[0] == '\r') {
		term_send(&term, "\r\n", 2);
		if (settings.local_echo)
			show_bytes((const uint8_t *)"\r\n", 2);
		return;
	}
	term_send(&term, (const char *)key->raw, key->raw_len);
	if (settings.local_echo)
		show_bytes(key->raw, key->raw_len);
}

/*
 * F12: save the screen to the card. Success flashes the screen (or, in
 * SET-UP, names the file on the message line); failure gets the
 * Configuration Error treatment, since a silent failure would look like
 * success.
 */
static void
take_screenshot(void)
{
	char path[32], error[64];
	uint32_t start = now_ms();

	if (!screenshot_save(&render, settings.screenshot_format, path,
	    sizeof(path), error, sizeof(error))) {
		if (in_setup) {
			char text[96];
			snprintf(text, sizeof(text), "Screenshot failed: %s",
			    error);
			setup_message(&setup, text);
			return;
		}
		config_problem("Screenshot", error);
		show_config_problems();
		return;
	}
	if (in_setup) {
		char text[64];
		snprintf(text, sizeof(text), "Saved %s (%lu ms)", path,
		    (unsigned long)(now_ms() - start));
		setup_message(&setup, text);
	} else {
		char text[48];
		snprintf(text, sizeof(text), "Saved %s", path);
		term_status_message(&term, text, now_ms());
		if (!bell_flashing) {
			term.screen->reverse_video = !term.reverse_video;
			bell_flashing = true;
			bell_until = make_timeout_time_ms(VISUAL_BELL_MS);
		}
	}
}

/* --- the Tools menu and hotkeys (lib/setup/menu.h) -------------------- */

/* The Tools menu and the file picker are never open together. */
static union {
	struct menu	 menu;
	struct picker	 picker;
} ui;
static bool in_menu, in_picker;
static char picker_dir[PICKER_NAME] = "/capture";

static void
close_menu(void)
{
	render.overlay[RENDER_OVERLAY_MENU] = nullptr;
	in_menu = false;
	in_picker = false;
	render_plan(&render);	/* gone now, not next frame: screenshots */
}

static uint16_t
theme_rgb565(const uint8_t rgb[3])
{
	return grid_rgb565(rgb[0], rgb[1], rgb[2]);
}

/* --- capture and replay (capture_sd.h, replay_sd.h) ------------------- */

static void
toggle_capture(void)
{
	char text[48];

	if (settings.capture && capture_sd_name()[0] != '\0') {
		snprintf(text, sizeof(text), "Saved /capture/%s",
		    capture_sd_name());
		term_status_message(&term, text, now_ms());
	}
	settings.capture = !settings.capture;
	capture_sd_enable(settings.capture);
	show_state();
}

/* Capturing stopped by itself (card pulled or full): say why. */
static void
check_capture(void)
{
	char text[64];

	if (!settings.capture || capture_sd_enabled())
		return;
	settings.capture = false;
	snprintf(text, sizeof(text), "Capture stopped: %s",
	    capture_sd_error());
	term_status_message(&term, text, now_ms());
	show_state();
}

static bool
list_entry(const char *name, uint32_t size, bool is_dir,
    const uint8_t *head, size_t head_len, void *ctx)
{
	(void)head;
	(void)head_len;
	board_watchdog_feed();
	/* Dot names are hidden, as in ls: on a card that's been in a Mac,
	 * .Trashes, .Spotlight-V100 and a "._" twin of every file. */
	if (name[0] == '.')
		return true;
	return picker_add(ctx, name, size, is_dir);
}

/* Show `dir` in the picker (the root if it isn't there). */
static void
open_picker(const char *dir)
{
	char text[64];

	if (!storage_mount()) {
		snprintf(text, sizeof(text), "Replay: %s", storage_error());
		term_status_message(&term, text, now_ms());
		return;
	}
	if (!storage_exists(dir))
		dir = "/";
	snprintf(picker_dir, sizeof(picker_dir), "%s", dir);
	picker_begin(&ui.picker, picker_dir,
	    "Enter: play / open   Esc: close");
	(void)storage_list(picker_dir, 0, list_entry, &ui.picker);
	picker_show(&ui.picker, &term.font, theme_rgb565(term.options.fg_rgb),
	    theme_rgb565(term.options.bg_rgb), term.screen->cols,
	    term.screen->rows);
	render.overlay[RENDER_OVERLAY_MENU] = &ui.picker.overlay;
	in_picker = true;
	render_plan(&render);
}

static void
start_replay(const char *path)
{
	static const char clean[] = "\x1b[!p\x1b[H\x1b[2J";
	char error[48], text[96];

	if (!replay_sd_start(path, now_ms(), settings.replay_newlines,
	    error, sizeof(error))) {
		snprintf(text, sizeof(text), "Replay: %s", error);
		term_status_message(&term, text, now_ms());
		return;
	}
	/* A clean slate to play onto: soft reset, home, clear. */
	term_feed(&term, (const uint8_t *)clean, sizeof(clean) - 1);
	show_state();
}

static void
picker_key_pressed(const struct key_event *key)
{
	char path[PICKER_NAME * 2];
	int picked = picker_key(&ui.picker, key);

	if (picked == PICKER_STAY)
		return;
	if (picked == PICKER_CLOSE) {
		close_menu();
		return;
	}
	picker_path(&ui.picker, picked, path, sizeof(path));
	if (picker_entry(&ui.picker, picked)->dir) {
		open_picker(path);	/* into it, or up */
		return;
	}
	close_menu();
	start_replay(path);
}

static void
replay_sink(const uint8_t *bytes, size_t len, void *ctx)
{
	(void)ctx;
	term_feed(&term, bytes, len);
}

static void
open_menu(void)
{
	bool available[TOOL_COUNT] = {false};
	const char *state[TOOL_COUNT] = {nullptr};

	available[TOOL_SETUP] = true;
	available[TOOL_SCREENSHOT] = true;
	available[TOOL_PRINT_SCREEN] = settings.printer;
	available[TOOL_PRINTER] = true;
	available[TOOL_CAPTURE] = true;
	available[TOOL_REPLAY] = true;
	available[TOOL_CLEAR] = true;
	available[TOOL_SOFT_RESET] = true;
	available[TOOL_HARD_RESET] = true;
	/* File transfers join when they're built. */
	state[TOOL_PRINTER] = settings.printer ? "on" : "off";
	state[TOOL_CAPTURE] = settings.capture ? "on" : "off";
	state[TOOL_REPLAY] = replay_sd_active() ? "playing" : nullptr;
	menu_open(&ui.menu, &term.font, theme_rgb565(term.options.fg_rgb),
	    theme_rgb565(term.options.bg_rgb), term.screen->cols,
	    term.screen->rows, available, state, settings.hotkeys);
	render.overlay[RENDER_OVERLAY_MENU] = &ui.menu.overlay;
	in_menu = true;
	render_plan(&render);
}

/*
 * Run a tool, from the menu or its hotkey. Local only: none of these
 * send anything to the host. Settings changed here (the printer) last
 * for the session; SET-UP's Save makes them stick.
 */
static void
run_tool(enum tool tool)
{
	static const char clear[] = "\x1b[H\x1b[2J";
	static const char soft_reset[] = "\x1b[!p";

	switch (tool) {
	case TOOL_MENU:
		open_menu();
		break;
	case TOOL_SETUP:
		enter_setup();
		break;
	case TOOL_SCREENSHOT:
		take_screenshot();
		break;
	case TOOL_PRINT_SCREEN:
		printer_print_screen(&term);
		break;
	case TOOL_PRINTER:
		settings.printer = !settings.printer;
		printer_sd_enable(&term, settings.printer);
		show_state();
		break;
	case TOOL_CAPTURE:
		toggle_capture();
		break;
	case TOOL_REPLAY:
		if (replay_sd_active()) {
			replay_sd_stop();
			term_status_message(&term, "Replay stopped", now_ms());
			show_state();
		} else {
			open_picker(picker_dir);
		}
		break;
	case TOOL_CLEAR:
		term_feed(&term, (const uint8_t *)clear, sizeof(clear) - 1);
		break;
	case TOOL_SOFT_RESET:
		term_feed(&term, (const uint8_t *)soft_reset,
		    sizeof(soft_reset) - 1);
		break;
	case TOOL_HARD_RESET:
		term_reset(&term);
		break;
	default:
		break;		/* not built yet */
	}
}

/* The tool bound to `key`, or -1. */
static int
hotkey_tool(const struct key_event *key)
{
	for (unsigned tool = 0; tool < TOOL_COUNT; tool++)
		if (hotkey_matches(&settings.hotkeys[tool], key))
			return (int)tool;
	return -1;
}

static void
handle_key(const struct key_event *key)
{
	if (VEETEE_DEBUG_BUILD) {
		/* Where every key goes: the first thing to know when a key
		 * "does nothing" (devlog 027). */
		char name[24];
		keys_name(key->code, key->byte, key->mods, name, sizeof(name));
		usbio_printf("[key] %s -> %s\r\n", name, in_config_error ?
		    "config error" : in_setup ? "set-up" : in_menu ? "menu" :
		    in_picker ? "picker" : hotkey_tool(key) >= 0 ? "hotkey" :
		    replay_sd_active() ? "replay" : "host");
	}
	if (in_config_error) {
		dismiss_config_error();
	} else if (in_setup) {
		setup_handle(key);
	} else if (in_menu) {
		int picked = menu_key(&ui.menu, key);
		if (picked == MENU_STAY)
			return;
		close_menu();		/* before the tool: not in screenshots */
		if (picked != MENU_CLOSE)
			run_tool((enum tool)picked);
	} else if (in_picker) {
		picker_key_pressed(key);
	} else {
		int tool = hotkey_tool(key);
		if (tool >= 0) {
			run_tool((enum tool)tool);
		} else if (replay_sd_active()) {
			/* Keys are the replay's while it runs. */
			if (!replay_sd_key(key))
				term_status_message(&term, "Replay stopped",
				    now_ms());
			show_state();
		} else {
			forward_key(key);
		}
	}
}

#if VEETEE_DEBUG_BUILD
/*
 * The automation HID (debug builds; include/automation_hid.h): a script
 * on the host typing, pressing keys and BOOTSEL. Its keys take the same
 * road as a person's (handle_key: hotkeys, menus, SET-UP, the host), and
 * its BOOTSEL joins the real one in bootsel_down().
 */
static struct automation automation;

static void
poll_automation(void)
{
	uint8_t report[AUTOMATION_REPORT_SIZE];
	struct key_event events[AUTOMATION_REPORT_SIZE];

	while (usbio_automation_read(report)) {
		unsigned count = automation_report(&automation, report,
		    sizeof(report), now_ms(), events, AUTOMATION_REPORT_SIZE);
		for (unsigned i = 0; i < count; i++)
			handle_key(&events[i]);
	}
	unsigned count = automation_poll(&automation, now_ms(), events);
	for (unsigned i = 0; i < count; i++)
		handle_key(&events[i]);
	usbio_poll();			/* debug text queued for the host */
}
#endif

/*
 * Is BOOTSEL down: the real button, or (debug builds) the automation
 * HID's. `fresh` reads the HID's reports first, but only for BOOTSEL
 * (keys in them are dropped): for factory_reset's wait, where the main
 * loop isn't running and nothing may act on a key.
 */
static bool
bootsel_down(bool fresh)
{
	bool down = board_bootsel_pressed();

#if VEETEE_DEBUG_BUILD
	uint8_t report[AUTOMATION_REPORT_SIZE];
	struct key_event dropped[AUTOMATION_REPORT_SIZE];

	while (fresh && usbio_automation_read(report))
		(void)automation_report(&automation, report, sizeof(report),
		    now_ms(), dropped, AUTOMATION_REPORT_SIZE);
	down = down || automation.bootsel;
#else
	(void)fresh;
#endif
	return down;
}

/* --- the USB keyboard (lib/hal/usbhost.h, lib/input/hidkbd.h) -------- */

static struct hidkbd usb_keyboard;

/* A report (from usbhost_poll): its new keys, as from the CDC port. */
static void
usb_keyboard_report(const uint8_t *report, size_t len, void *ctx)
{
	struct key_event events[HIDKBD_KEYS];
	uint8_t leds = hidkbd_leds(&usb_keyboard);

	(void)ctx;
	if (VEETEE_DEBUG_BUILD) {
		usbio_printf("[hid] keyboard");
		for (size_t i = 0; i < len; i++)
			usbio_printf(" %02x", report[i]);
		usbio_printf("\r\n");
	}
	unsigned count = hidkbd_report(&usb_keyboard, report, len, now_ms(),
	    events, HIDKBD_KEYS);
	for (unsigned i = 0; i < count; i++)
		handle_key(&events[i]);
	if (hidkbd_leds(&usb_keyboard) != leds)
		usbhost_set_leds(hidkbd_leds(&usb_keyboard));
}

static void
usb_keyboard_event(const char *text, void *ctx)
{
	(void)ctx;
	term_status_message(&term, text, now_ms());
}

static void
poll_usb_keyboard(void)
{
	struct key_event key;

	usbhost_poll();
	if (hidkbd_poll(&usb_keyboard, now_ms(), &key))
		handle_key(&key);
}

/*
 * BOOTSEL held for BOOTSEL_RESET_MS: every setting back to factory
 * defaults, saved, and a restart into them. The way out of a setting
 * you can't undo without a keyboard, or a video mode the monitor can't
 * show (it works blind). "Every setting" is the whole settings record:
 * mode, font, colours, hotkeys, lights, printer, capture. Files on the
 * SD card aren't touched, and the restart is a deliberate one, so it
 * doesn't count towards the crash fallback (board_boot_begin).
 */
static void
factory_reset(void)
{
	struct settings defaults;

	settings_defaults(&defaults);
	settings_seal(&defaults);
	bool saved = flashcfg_write(&defaults, sizeof(defaults));
	if (in_setup)
		setup_message(&setup, saved ?
		    "Factory defaults saved. Let go of BOOTSEL to restart." :
		    "Factory reset FAILED to save. Let go of BOOTSEL.");
	/*
	 * Wait for the button to be let go. The bootrom reads BOOTSEL at
	 * reset, and finding it held, would come up as a USB drive instead
	 * of the VeeTee. Video carries on meanwhile (it's interrupts and
	 * core 1); only the main loop waits, so feed the watchdog.
	 */
	while (bootsel_down(true)) {
		board_watchdog_feed();
		sleep_ms(BOOTSEL_POLL_MS);
	}
	if (saved)
		board_reboot();
}

static void
poll_bootsel(void)
{
	static absolute_time_t next_poll;
	static uint32_t shown_seconds;	/* countdown on screen, 0: none */
	uint32_t left_ms;

	if (!time_reached(next_poll) || !video_flash_safe())
		return;
	next_poll = make_timeout_time_ms(BOOTSEL_POLL_MS);
	switch (button_sample(&bootsel, bootsel_down(false), now_ms(),
	    &left_ms)) {
	case BUTTON_TAP:
		if (in_config_error)
			dismiss_config_error();
		else if (in_setup)
			setup_act(setup_exit(&setup));	/* asks, if changed */
		else
			enter_setup();
		break;
	case BUTTON_HOLDING: {
		/*
		 * Show the countdown in SET-UP's message line, opening
		 * SET-UP for it if need be. If that fails (no memory) the
		 * reset still happens, just without a countdown.
		 */
		if (!in_setup) {
			if (in_config_error)
				dismiss_config_error();
			enter_setup();
			shown_seconds = 0;
		}
		uint32_t seconds = (left_ms + 999) / 1000;
		if (in_setup && seconds != shown_seconds) {
			char text[64];
			snprintf(text, sizeof(text), "Keep holding BOOTSEL: "
			    "factory reset in %lu s", (unsigned long)seconds);
			setup_message(&setup, text);
			shown_seconds = seconds;
		}
		break;
	}
	case BUTTON_CANCELLED:
		if (in_setup)
			setup_message(&setup, "Factory reset cancelled.");
		shown_seconds = 0;
		break;
	case BUTTON_FIRED:
		factory_reset();
		shown_seconds = 0;	/* only if the save failed */
		break;
	case BUTTON_NONE:
		break;
	}
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
	uint32_t fault_pc, fault_lr;
	if (board_last_fault(&fault_pc, &fault_lr))
		usbio_printf("\r\n[fault] last boot: pc=0x%08lx lr=0x%08lx "
		    "r0=0x%08lx r2=0x%08lx r5=0x%08lx",
		    (unsigned long)fault_pc, (unsigned long)fault_lr,
		    (unsigned long)watchdog_hw->scratch[5],
		    (unsigned long)watchdog_hw->scratch[6],
		    (unsigned long)watchdog_hw->scratch[7]);
	usbio_printf("\r\n[mem] term cells=%p/%p sixel cells=%u",
	    (void *)term.primary.cells, (void *)term.alternate.cells,
	    term.graphics.tiles);
	usbio_printf("\r\n[stats] frames=%lu encode avg=%lu max=%lu "
	    "render avg=%lu max=%lu starved=%lu yields=%lu budget=%u\r\n",
	    (unsigned long)stats.frames, (unsigned long)stats.avg_encode_cycles,
	    (unsigned long)stats.max_encode_cycles,
	    (unsigned long)stats.avg_render_cycles,
	    (unsigned long)stats.max_render_cycles,
	    (unsigned long)stats.starved, (unsigned long)stats.yields,
	    video_line_budget());
	usbio_printf("[usb] keyboard port %s\r\n", usbhost_refresh());
	/* Host output this second: fed in the blanking gap (frames that
	 * had some), fed mid-frame because of a backlog (devlog 031). */
	static uint32_t seen_idle, seen_busy, seen_frames, seen_passes,
	    seen_idle_passes;
	usbio_printf("[blank] longest pause before line %lu, %lu cycles\r\n",
	    (unsigned long)stats.gap_line, (unsigned long)stats.gap_cycles);
	usbio_printf("[feed] gap=%lu bytes in %lu frames, mid-frame=%lu bytes, "
	    "waiting=%u; loop passes=%lu, in the gap=%lu\r\n",
	    (unsigned long)(feed_idle_bytes - seen_idle),
	    (unsigned long)(feed_frames - seen_frames),
	    (unsigned long)(feed_busy_bytes - seen_busy),
	    (unsigned)line_available(),
	    (unsigned long)(feed_passes - seen_passes),
	    (unsigned long)(feed_idle_passes - seen_idle_passes));
	seen_idle = feed_idle_bytes;
	seen_busy = feed_busy_bytes;
	seen_frames = feed_frames;
	seen_passes = feed_passes;
	seen_idle_passes = feed_idle_passes;
#ifdef VEETEE_RENDER_PROFILE
	/* Overlay composition (Tools menu, user lights), per composed line. */
	/* Running totals (render.h): this second's share is the change. */
	static uint32_t seen_lines, seen_cycles, seen_loop_lines,
	    seen_loop_cycles;
	uint32_t lines = render.profile_compose_lines - seen_lines;
	uint32_t cycles = render.profile_compose_cycles - seen_cycles;
	uint32_t loop_lines = render.profile_loop_lines - seen_loop_lines;
	uint32_t loop_cycles = render.profile_loop_cycles - seen_loop_cycles;
	seen_lines = render.profile_compose_lines;
	seen_cycles = render.profile_compose_cycles;
	seen_loop_lines = render.profile_loop_lines;
	seen_loop_cycles = render.profile_loop_cycles;
	usbio_printf("[profile] overlay lines=%lu compose avg=%lu cycles; "
	    "column loop avg=%lu cycles over %lu lines\r\n",
	    (unsigned long)lines, (unsigned long)(lines ? cycles / lines : 0),
	    (unsigned long)(loop_lines ? loop_cycles / loop_lines : 0),
	    (unsigned long)loop_lines);
#endif
#ifdef VEETEE_RENDER_EXPERIMENTS
	/* render.c's loop variants, average cycles a run. */
	static uint32_t seen_runs, seen_exp[8];
	uint32_t runs = render.experiment_runs - seen_runs;
	seen_runs = render.experiment_runs;
	usbio_printf("[experiments] runs=%lu", (unsigned long)runs);
	for (unsigned i = 0; i < 6; i++) {
		uint32_t total = render.experiment_cycles[i] - seen_exp[i];
		seen_exp[i] = render.experiment_cycles[i];
		usbio_printf(" %u:%lu", i,
		    (unsigned long)(runs ? total / runs : 0));
	}
	usbio_printf("\r\n");
#endif
}
#endif

void
app_init(void)
{
	load_settings();
#if VEETEE_DEBUG_BUILD
	automation_init(&automation);
#endif
	button_init(&bootsel, BOOTSEL_HOLD_MS, BOOTSEL_RESET_MS);
	/*
	 * If the last boots ended in watchdog resets (a crash or hang, e.g.
	 * an overclocked mode this chip can't hold), run this session on
	 * factory defaults. The saved settings stay in flash untouched, so
	 * nothing is lost to a transient fault; saving from SET-UP makes the
	 * defaults stick.
	 */
	unsigned crashes = board_boot_begin();
	bool fell_back = crashes >= CRASHES_MAX;
	if (fell_back)
		settings_defaults(&settings);
	running_resolution = settings.resolution;
	const struct video_mode *mode = &video_modes[running_resolution];
	render_set_screen(&render, mode->width, mode->height);
	/*
	 * Video first, SD card second. Start on a built-in font, bring the
	 * picture up, and only then apply the saved settings, which may name
	 * fonts and schemes on the card (a second rebuild at boot, cheap).
	 * A missing or slow card then can't delay or interfere with the
	 * first picture, and its Configuration Error has a screen to go on.
	 */
	const struct font_builtin *boot_font = font_builtin_find(settings.font);
	if (!term_init(&term, boot_font != nullptr ? boot_font :
	    &font_builtins[0], &render, send_to_host, nullptr))
		panic("term: cannot start (font or memory)");
	/*
	 * The first picture is the splash; render.live stays false until it
	 * ends (end_splash), since the renderer isn't drawing until then.
	 */
	splash_layout(&splash, mode->width, mode->height, setup_cells,
	    sizeof(setup_cells));	/* SET-UP's screen: free until opened */
	splash_up = true;
	video_init(running_resolution, splash_scanline, &splash);
	splash_until = now_ms() + SPLASH_MS;
	/* After video_init: its TMDS buffers come off the heap first. */
	term_set_graphics_budget(&term, GRAPHICS_BUDGET, board_free_heap);
	term_set_keepalive(&term, board_watchdog_feed);
	/*
	 * After video_init, whose DMA channels and PIO 0 it must not take.
	 * Once it runs, its frame timer is the one interrupt the renderer
	 * yields to (usbhost.h).
	 */
	hidkbd_init(&usb_keyboard);
	if (usbhost_init(usb_keyboard_report, usb_keyboard_event, nullptr))
		video_set_priority(PICO_HIGHEST_IRQ_PRIORITY + 0x40);
	/* The LEDs each keyboard gets as it attaches (Num Lock starts on). */
	usbhost_set_leds(hidkbd_leds(&usb_keyboard));
	apply_settings();	/* may read the card */

	static const char banner[] =
	    "\x1b[1mVeeTee 2350\x1b[0m ready. Ctrl+F3: Tools. BOOTSEL: "
	    "SET-UP (hold 5 s: factory reset)\r\n";
	term_feed(&term, (const uint8_t *)banner, sizeof(banner) - 1);
	char detail[320];
	char line[32];
	settings_line_summary(&settings, line, sizeof(line));
	int len = snprintf(detail, sizeof(detail), "%s, %s, %ux%u, "
	    "sixel %u cells\r\nsettings: %s; line: %s; USB keyboard port: "
	    "%s\r\nlast reset: %s%s\r\n",
	    mode->name, term.font_source->name, term.screen->cols,
	    term.screen->rows, term.graphics.tiles, settings_origin, line,
	    usbhost_describe(), board_reset_reason(),
	    strcmp(board_reset_reason(), "brown-out") == 0 ?
	    "  (the supply dipped: a USB device plugged in?)" : "");
	term_feed(&term, (const uint8_t *)detail, (size_t)len);
	if (VEETEE_DEBUG_BUILD) {
		static const char debug[] = "\x1b[1mdebug build:\x1b[22m"
		    VEETEE_DEBUG_FEATURES "\r\n";
		term_feed(&term, (const uint8_t *)debug, sizeof(debug) - 1);
	}
#ifdef VEETEE_FLASH_PROBE
	extern struct board_flash_probe video_flash_probe_result;
	const struct board_flash_probe *probe = &video_flash_probe_result;
	len = snprintf(detail, sizeof(detail), "flash probe @%lu MHz: div %u "
	    "rxdelay ok mask 0x%02x, div %u mask 0x%02x\r\n",
	    (unsigned long)(probe->sys_hz / 1000000), probe->divider,
	    probe->pass[0], probe->divider + 1, probe->pass[1]);
	term_feed(&term, (const uint8_t *)detail, (size_t)len);
#endif
	uint32_t fault_pc, fault_lr;
	if (board_last_fault(&fault_pc, &fault_lr)) {
		len = snprintf(detail, sizeof(detail), "\x1b[1;31mLast boot "
		    "crashed: HardFault at pc=0x%08lx lr=0x%08lx\x1b[0m\r\n",
		    (unsigned long)fault_pc, (unsigned long)fault_lr);
		term_feed(&term, (const uint8_t *)detail, (size_t)len);
		line_write(detail, (size_t)len);
	}
	if (fell_back)
		config_problem("Startup", "the last boots crashed or hung "
		    "repeatedly: running on factory defaults");
	show_config_problems();
}

/*
 * The profiler bar (SET-UP, render.h): once a frame, last frame's numbers
 * from hal/video's stats into the renderer. Overlay composing is only
 * timed in VEETEE_RENDER_PROFILE builds; elsewhere its share is 0 and
 * the whole average shows as rendering.
 */
static void
update_meter(void)
{
	static uint32_t last_frame, last_compose;
	struct render_meter *meter = &render.meter;
	struct video_stats stats;

	meter->on = settings.profiler_bar == PROFILER_ON ||
	    (settings.profiler_bar == PROFILER_DEBUG && VEETEE_DEBUG_BUILD);
	if (!meter->on || render.frame == last_frame)
		return;
	last_frame = render.frame;
	video_get_stats(&stats);

	/* Compose cycles this frame, spread over the frame's lines. */
	uint32_t compose_total = render.profile_compose_cycles;
	uint32_t compose = (compose_total - last_compose) / render.height;
	last_compose = compose_total;
	if (compose > stats.avg_render_cycles)
		compose = stats.avg_render_cycles;

	meter->budget = (uint16_t)video_line_budget();
	meter->bar[0] = (struct render_meter_bar){
		.max = (uint16_t)stats.max_render_cycles,
		.count = 2,
		.segment = {
			{(uint16_t)compose, grid_rgb565(240, 200, 0)},
			{(uint16_t)(stats.avg_render_cycles - compose),
			    grid_rgb565(40, 200, 80)},
		},
	};
	meter->bar[1] = (struct render_meter_bar){
		.max = (uint16_t)stats.max_encode_cycles,
		.count = 1,
		.segment = {
			{(uint16_t)stats.avg_encode_cycles,
			    grid_rgb565(60, 120, 255)},
		},
	};
}

void
app_poll(void)
{
	static bool boot_marked_ok;

	board_watchdog_feed();
	video_poll();
	update_meter();
	/* The status line and lights, once a frame (lib/term/status.h). */
	static uint32_t status_frame;
	if (render.frame != status_frame) {
		status_frame = render.frame;
		show_state();	/* the capture's file name appears late */
		term_status_poll(&term, now_ms(), !in_setup);
		/* The frame plan (render.h): after the lights overlay above. */
		render_plan(&render);
	}
#ifdef VEETEE_FLASH_PROBE
	static absolute_time_t next_probe_report;
	if (time_reached(next_probe_report)) {
		extern struct board_flash_probe video_flash_probe_result;
		extern uint32_t board_boot_qmi_timing;
		const struct board_flash_probe *p = &video_flash_probe_result;
		next_probe_report = make_timeout_time_ms(1000);
		usbio_printf("[probe] %lu MHz boot_timing=0x%08lx div%u mask=0x%02x "
		    "div%u mask=0x%02x took %lu us\r\n",
		    (unsigned long)(p->sys_hz / 1000000),
		    (unsigned long)board_boot_qmi_timing, p->divider, p->pass[0],
		    p->divider + 1, p->pass[1], (unsigned long)p->probe_us);
	}
#endif
	if (splash_up && (int32_t)(now_ms() - splash_until) >= 0)
		end_splash();
	if (!boot_marked_ok && now_ms() > BOOT_HEALTHY_MS) {
		board_boot_ok();
		boot_marked_ok = true;
	}
	/* Host output waits while the Configuration Error is showing, and
	 * while a replay has the screen (it stays in the USB buffers). */
	if (!in_config_error && !replay_sd_active())
		feed_line();
	/* On the UART, what a program sends the idle CDC port goes
	 * nowhere: drain it rather than let it back up into the host. */
	if (line_on_uart) {
		char discard[64];
		while (usbio_read(USBIO_LINE, discard, sizeof(discard)) > 0)
			;
	}

	if (replay_sd_active() && !in_config_error &&
	    !replay_sd_poll(now_ms(), replay_sink, nullptr)) {
		term_status_message(&term, "Replay finished", now_ms());
		show_state();
	}
	capture_sd_poll(now_ms());
	check_capture();
#if VEETEE_DEBUG_BUILD
	poll_automation();
#endif
	poll_usb_keyboard();
	poll_bootsel();
	poll_bell();
	printer_sd_poll();
#ifdef VEETEE_SERIAL_STATS
	report_stats();
#endif
}

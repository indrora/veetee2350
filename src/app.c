/*
 * app.c — application glue: USB ports, terminal, SET-UP screen.
 *
 * Bytes from the host arrive on the line port (hal/usbio.h) and go into
 * the terminal emulator (lib/term: libvterm -> cell grid), which the video
 * pipeline draws. Replies (DA, DSR, ...) go back out on the line port.
 *
 * Keys arrive on the keyboard port and pass through a decoder (lib/input):
 * normally they're forwarded to the host unchanged (with LNM and local
 * echo applied), except hotkeys (settings.hotkeys): Ctrl+F3 opens the
 * Tools menu (lib/setup/menu.h), F12 takes a screenshot. The BOOTSEL
 * button opens SET-UP. The Tools menu is an overlay over the live screen. While SET-UP is open, keys drive the
 * menu instead, and its own grid replaces the terminal's on screen; the
 * terminal keeps processing host output underneath.
 */
#include "app.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "card.h"
#include "flashcfg.h"
#include "font.h"
#include "keys.h"
#include "render.h"
#include "menu.h"
#include "printer_sd.h"
#include "screenshot.h"
#include "settings.h"
#include "storage.h"
#include "setup.h"
#include "term.h"
#include "usbio.h"
#include "video.h"

#include "pico.h"
#include "pico/time.h"
#include "hardware/structs/watchdog.h"

static_assert(sizeof(struct settings) <= FLASHCFG_MAX);

#define BOOTSEL_POLL_MS		50
#define BOOT_HEALTHY_MS		60000	/* then the crash count clears */
#define CRASHES_MAX		2	/* in a row: boot with factory defaults */
#define VISUAL_BELL_MS		100

static struct render render;
static struct term term;
static struct settings settings;	/* in effect now */
static unsigned running_resolution;	/* video mode chosen at boot */
static struct keys keys;

static bool in_setup;
static struct setup setup;
static struct grid setup_grid;

/* Configuration Error screen: problems collected while applying. */
static char problems[512];
static size_t problems_len;
static bool in_config_error;

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
	if (!term_configure(&term, settings_font(), &options))
		panic("term: cannot apply settings (memory)");
	printer_sd_enable(&term, settings.printer != 0);
}

static void
load_settings(void)
{
	flashcfg_read(&settings, sizeof(settings));
	if (!settings_valid(&settings))
		settings_defaults(&settings);
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

static void
enter_setup(void)
{
	struct grid *screen = term.screen;
	struct cell blank = grid_make_cell(term.space_glyph, 0xffff, 0, 0);

	if (!grid_init(&setup_grid, screen->cols, screen->rows, blank))
		return;		/* no memory: stay in the terminal */
	setup_grid.cursor_visible = false;
	setup_grid.border2 = grid_dup565(grid_rgb565(16, 40, 120));
	setup_begin(&setup, &settings, &setup_grid, &term.font,
	    running_resolution);
	/* Shown first, so the card scan's progress is visible. */
	render_set_grid(&render, &setup_grid);
	in_setup = true;
	show_storage_status();
	struct setup_catalogue catalogue;
	(void)card_scan(&catalogue, scan_progress, nullptr);
	setup_catalogue(&setup, &catalogue);
}

static void
leave_setup(void)
{
	in_setup = false;
	render_set_grid(&render, term.screen);
	settings = setup.work;
	settings_seal(&settings);

	/*
	 * A new resolution means a new sysclk, VREG and DVI timing, which are
	 * only set at boot (video_init), so save it and restart. Saving is
	 * implied: without it the restart would come back in the old mode.
	 */
	if (settings.resolution != running_resolution) {
		if (!flashcfg_write(&settings, sizeof(settings)))
			panic("settings: save before restart failed");
		board_reboot();
	}

	/*
	 * Free SET-UP's grid before applying: a font change rebuilds the
	 * terminal's grids and the Sixel pool, and should have the memory.
	 * The renderer was pointed back at the terminal above; the pause
	 * makes sure no scanline still reads setup_grid.
	 */
	render_pause(&render);
	grid_free(&setup_grid);
	card_release();		/* SET-UP's lists: memory for a card font */
	apply_settings();
	render_init(&render, &term.font, term.screen);
	show_config_problems();
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
	} else if (!bell_flashing) {
		term.screen->reverse_video = !term.reverse_video;
		bell_flashing = true;
		bell_until = make_timeout_time_ms(VISUAL_BELL_MS);
	}
}

/* --- the Tools menu and hotkeys (lib/setup/menu.h) -------------------- */

static struct menu menu;
static bool in_menu;

static void
close_menu(void)
{
	render.overlay[RENDER_OVERLAY_MENU] = nullptr;
	in_menu = false;
}

static void
open_menu(void)
{
	bool available[TOOL_COUNT] = {false};
	const char *state[TOOL_COUNT] = {nullptr};
	const uint8_t *fg = term.options.fg_rgb, *bg = term.options.bg_rgb;

	available[TOOL_SETUP] = true;
	available[TOOL_SCREENSHOT] = true;
	available[TOOL_PRINT_SCREEN] = settings.printer;
	available[TOOL_PRINTER] = true;
	available[TOOL_CLEAR] = true;
	available[TOOL_SOFT_RESET] = true;
	available[TOOL_HARD_RESET] = true;
	/* Capture, replay and transfers join as they're built. */
	state[TOOL_PRINTER] = settings.printer ? "on" : "off";
	menu_open(&menu, &term.font, grid_rgb565(fg[0], fg[1], fg[2]),
	    grid_rgb565(bg[0], bg[1], bg[2]), term.screen->cols,
	    term.screen->rows, available, state, settings.hotkeys);
	render.overlay[RENDER_OVERLAY_MENU] = &menu.overlay;
	in_menu = true;
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
	if (in_config_error) {
		dismiss_config_error();
	} else if (in_setup) {
		setup_handle(key);
	} else if (in_menu) {
		int picked = menu_key(&menu, key);
		if (picked == MENU_STAY)
			return;
		close_menu();		/* before the tool: not in screenshots */
		if (picked != MENU_CLOSE)
			run_tool((enum tool)picked);
	} else {
		int tool = hotkey_tool(key);
		if (tool >= 0)
			run_tool((enum tool)tool);
		else
			forward_key(key);
	}
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
		if (in_config_error)
			dismiss_config_error();
		else if (in_setup)
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
#ifdef VEETEE_RENDER_PROFILE
	/* Overlay composition (Tools menu, user lights), per composed line. */
	uint32_t lines = render.profile_compose_lines;
	uint32_t cycles = render.profile_compose_cycles;
	render.profile_compose_lines = 0;
	render.profile_compose_cycles = 0;
	usbio_printf("[profile] overlay lines=%lu compose avg=%lu cycles\r\n",
	    (unsigned long)lines, (unsigned long)(lines ? cycles / lines : 0));
#endif
}
#endif

void
app_init(void)
{
	load_settings();
	keys_init(&keys);
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
	video_init(running_resolution, render_scanline, &render);
	render.live = true;	/* from now on, render_pause must wait */
	/* After video_init: its TMDS buffers come off the heap first. */
	term_set_graphics_budget(&term, GRAPHICS_BUDGET, board_free_heap);
	term_set_keepalive(&term, board_watchdog_feed);
	apply_settings();	/* may read the card */

	static const char banner[] =
	    "\x1b[1mVeeTee 2350\x1b[0m ready. Ctrl+F3: Tools. BOOTSEL: "
	    "SET-UP\r\n";
	term_feed(&term, (const uint8_t *)banner, sizeof(banner) - 1);
	char detail[96];
	int len = snprintf(detail, sizeof(detail), "%s, %s, %ux%u, "
	    "sixel %u cells\r\n", mode->name, term.font_source->name,
	    term.screen->cols, term.screen->rows, term.graphics.tiles);
	term_feed(&term, (const uint8_t *)detail, (size_t)len);
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
		usbio_write(USBIO_LINE, detail, (size_t)len);
	}
	if (fell_back)
		config_problem("Startup", "the last boots crashed or hung "
		    "repeatedly: running on factory defaults");
	show_config_problems();
}

void
app_poll(void)
{
	static bool boot_marked_ok;
	char buffer[256];

	board_watchdog_feed();
	video_poll();
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
	if (!boot_marked_ok && now_ms() > BOOT_HEALTHY_MS) {
		board_boot_ok();
		boot_marked_ok = true;
	}
	/* Host output waits while the Configuration Error is showing. */
	size_t count = in_config_error ? 0 :
	    usbio_read(USBIO_LINE, buffer, sizeof(buffer));

	if (count > 0)
		term_feed(&term, (const uint8_t *)buffer, count);
	poll_keyboard();
	poll_bootsel();
	poll_bell();
	printer_sd_poll();
#ifdef VEETEE_SERIAL_STATS
	report_stats();
#endif
}

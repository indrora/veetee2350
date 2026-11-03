/*
 * settings.c — defaults, validation and colour schemes.
 */
#include "settings.h"

#include <stddef.h>
#include <string.h>

#include "grid.h"
#include "keys.h"
#include "term.h"
#include "video_modes.h"

static const struct {
	const char	*name;
	uint8_t		 fg[3];
	uint8_t		 bg[3];
} schemes[SCHEME_COUNT] = {
	[SCHEME_XTERM] = {"xterm grey", {229, 229, 229}, {0, 0, 0}},
	/* Phosphor colours approximate P1/P3/P4 CRTs on a dark face. */
	[SCHEME_GREEN] = {"green phosphor", {51, 255, 102}, {0, 20, 8}},
	[SCHEME_AMBER] = {"amber phosphor", {255, 176, 0}, {20, 10, 0}},
	[SCHEME_WHITE] = {"white phosphor", {255, 255, 255}, {0, 0, 0}},
	[SCHEME_PAPER] = {"paper", {16, 16, 16}, {240, 236, 224}},
};

const struct settings_size settings_sizes[SIZE_COUNT] = {
	[SIZE_FULL] = {0, 0},
	[SIZE_80x24] = {80, 24},
	[SIZE_80x25] = {80, 25},
	[SIZE_80x43] = {80, 43},
	[SIZE_80x50] = {80, 50},
	[SIZE_132x24] = {132, 24},
	[SIZE_132x43] = {132, 43},
};

const uint8_t settings_peek_seconds[SETTINGS_PEEK_TIMES] = {3, 5, 7, 10};

void
settings_defaults(struct settings *settings)
{
	memset(settings, 0, sizeof(*settings));
	/* 60 Hz, not 72: the ShadowCast capture card won't take VESA-only
	 * 640x480 @ 72, and factory defaults (or the crash fallback) must
	 * come up on a picture (devlog 025). */
	settings->resolution = VIDEO_MODE_640x480;
	strcpy(settings->font, "ati-smallw-6x8");	/* devlog 025 */
	settings->term_size = SIZE_FULL;
	settings->scheme = SCHEME_XTERM;
	settings->cursor_shape = CURSOR_BLOCK;
	settings->cursor_blink = 1;
	settings->autowrap = 1;
	settings->newline_mode = 0;
	settings->local_echo = 0;
	settings->visual_bell = 1;
	strcpy(settings->answerback, "VeeTee 2350");
	settings->emulation = EMU_XTERM;
	settings->colour_depth = term_emulations[EMU_XTERM].depth;
	settings->utf8 = term_emulations[EMU_XTERM].utf8;
	settings->alt_screen = term_emulations[EMU_XTERM].alt_screen;
	settings->sixel = term_emulations[EMU_XTERM].sixel;
	settings->status_line = STATUS_HIDDEN;
	settings->peek_time = 1;		/* 5 seconds */
	/* Ctrl+F3 opens the Tools menu (plain F3, SET-UP on a VT220, goes
	 * to the host now); F12 keeps taking screenshots. The rest are
	 * unbound (KEY_NONE is 0). */
	settings->hotkeys[TOOL_MENU] = (struct hotkey){KEY_F3, 0, KEY_MOD_CTRL};
	settings->hotkeys[TOOL_SCREENSHOT] = (struct hotkey){KEY_F12, 0, 0};
	/* Debug builds show what they measure (build.h). */
	settings->profiler_bar = PROFILER_DEBUG;
	settings_seal(settings);
}

/*
 * Bitwise CRC-32 (IEEE 802.3, reflected, poly 0xedb88320). Slow but tiny;
 * it runs over ~80 bytes at boot and on save.
 */
static uint32_t
crc32(const void *data, size_t len)
{
	const uint8_t *bytes = data;
	uint32_t crc = 0xffffffffu;

	for (size_t i = 0; i < len; i++) {
		crc ^= bytes[i];
		for (int bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
	}
	return ~crc;
}

void
settings_seal(struct settings *settings)
{
	settings->magic = SETTINGS_MAGIC;
	settings->version = SETTINGS_VERSION;
	settings->size = sizeof(*settings);
	/* Keep strings terminated whatever was edited into them. */
	settings->font[SETTINGS_FONT_MAX - 1] = '\0';
	settings->scheme_file[SETTINGS_SCHEME_MAX - 1] = '\0';
	settings->answerback[SETTINGS_ANSWERBACK_MAX - 1] = '\0';
	settings->crc = crc32(settings, offsetof(struct settings, crc));
}

/* Zero a text field past its NUL, so equal text is equal bytes. */
static void
tidy_text(char *text, size_t size)
{
	size_t len = strnlen(text, size);

	memset(text + len, 0, size - len);
}

bool
settings_same(const struct settings *a, const struct settings *b)
{
	/*
	 * Compared as bytes, after sealing tidied copies: the record is
	 * stored as raw bytes, so that's exactly "would a save change
	 * anything". Padding can't differ: every record starts as a
	 * zeroed settings_defaults() or a copy of one (flash holds what
	 * was saved from one), and fields are only ever assigned.
	 */
	struct settings left = *a, right = *b;

	tidy_text(left.font, sizeof(left.font));
	tidy_text(right.font, sizeof(right.font));
	tidy_text(left.scheme_file, sizeof(left.scheme_file));
	tidy_text(right.scheme_file, sizeof(right.scheme_file));
	tidy_text(left.answerback, sizeof(left.answerback));
	tidy_text(right.answerback, sizeof(right.answerback));
	settings_seal(&left);
	settings_seal(&right);
	return memcmp(&left, &right, sizeof(left)) == 0;
}

bool
settings_valid(const struct settings *settings)
{
	return settings->magic == SETTINGS_MAGIC &&
	    settings->version == SETTINGS_VERSION &&
	    settings->size == sizeof(*settings) &&
	    settings->scheme < SCHEME_COUNT &&
	    settings->resolution < VIDEO_MODE_COUNT &&
	    settings->term_size < SIZE_COUNT &&
	    settings->emulation < EMU_COUNT &&
	    settings->colour_depth < DEPTH_COUNT &&
	    settings->screenshot_format < SHOT_COUNT &&
	    (settings->sixel == 0 || settings->sixel == 1 ||
	    settings->sixel == 2 || settings->sixel == 4 ||
	    settings->sixel == 8) &&
	    settings->crc == crc32(settings, offsetof(struct settings, crc));
}

const char *
settings_scheme_name(unsigned scheme)
{
	return scheme < SCHEME_COUNT ? schemes[scheme].name : "?";
}

void
settings_scheme_rgb(unsigned scheme, uint8_t fg[3], uint8_t bg[3])
{
	if (scheme >= SCHEME_COUNT)
		scheme = SCHEME_XTERM;
	memcpy(fg, schemes[scheme].fg, 3);
	memcpy(bg, schemes[scheme].bg, 3);
}

/*
 * Where each old version's fields ended (its `crc` followed, after any
 * padding). Copying only up to here, not up to the old crc, matters:
 * the old padding bytes are new fields today.
 */
static size_t
fields_end(unsigned version)
{
	switch (version) {
	case 9:
		return offsetof(struct settings, printer) + 1;
	case 10:
		return offsetof(struct settings, hotkeys) +
		    sizeof(((struct settings *)0)->hotkeys);
	case 11:
		return offsetof(struct settings, profiler_bar) + 1;
	default:
		return 0;
	}
}

bool
settings_upgrade(struct settings *out, const void *raw)
{
	const struct settings *old = raw;
	const uint8_t *bytes = raw;
	uint32_t crc;

	if (old->magic != SETTINGS_MAGIC ||
	    old->version < SETTINGS_UPGRADE_FROM ||
	    old->version >= SETTINGS_VERSION ||
	    old->size < sizeof(uint32_t) || old->size > sizeof(*old) ||
	    old->size % sizeof(uint32_t) != 0 ||
	    fields_end(old->version) == 0 ||
	    fields_end(old->version) > old->size - sizeof(crc))
		return false;
	/* The old record's crc is its last word, over everything before. */
	memcpy(&crc, bytes + old->size - sizeof(crc), sizeof(crc));
	if (crc != crc32(raw, old->size - sizeof(crc)))
		return false;

	settings_defaults(out);
	memcpy(out, raw, fields_end(old->version));
	settings_seal(out);		/* this version's magic, size, crc */
	return settings_valid(out);
}

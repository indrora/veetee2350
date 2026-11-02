/*
 * settings.c — defaults, validation and colour schemes.
 */
#include "settings.h"

#include <stddef.h>
#include <string.h>

#include "grid.h"
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

void
settings_defaults(struct settings *settings)
{
	memset(settings, 0, sizeof(*settings));
	settings->resolution = VIDEO_MODE_640x480;
	strcpy(settings->font, "notjam-clean-16");
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
	settings->answerback[SETTINGS_ANSWERBACK_MAX - 1] = '\0';
	settings->crc = crc32(settings, offsetof(struct settings, crc));
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

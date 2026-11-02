/*
 * settings.h — the terminal's user settings (what SET-UP edits and saves).
 *
 * Layering: pure module. The record is a plain struct written to flash
 * as-is (hal/flashcfg), so its layout is versioned: bump SETTINGS_VERSION
 * whenever fields change, and stale records fall back to defaults.
 * A CRC-32 over everything before `crc` rejects erased (0xff) or torn
 * flash.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#define SETTINGS_MAGIC		0x45455456u	/* "VTEE" little-endian */
#define SETTINGS_VERSION	2	/* 2: added resolution */
#define SETTINGS_FONT_MAX	24
#define SETTINGS_ANSWERBACK_MAX	32

enum colour_scheme {
	SCHEME_XTERM,		/* light grey on black (xterm default) */
	SCHEME_GREEN,		/* P1 green phosphor */
	SCHEME_AMBER,		/* P3 amber phosphor */
	SCHEME_WHITE,		/* P4 white phosphor */
	SCHEME_PAPER,		/* black on off-white */
	SCHEME_COUNT
};

/* Rows choices for SETTINGS rows: 0 means "as many as the font fits". */
#define SETTINGS_ROWS_AUTO	0

struct settings {
	uint32_t	magic;
	uint16_t	version;
	uint16_t	size;		/* sizeof(struct settings) when sealed */
	uint8_t		resolution;	/* enum video_mode_id (video_modes.h) */
	char		font[SETTINGS_FONT_MAX];	/* built-in font name */
	uint8_t		rows;		/* SETTINGS_ROWS_AUTO, 24 or 25 */
	uint8_t		scheme;		/* enum colour_scheme */
	uint8_t		cursor_shape;	/* enum cursor_shape (grid.h) */
	uint8_t		cursor_blink;
	uint8_t		autowrap;	/* DECAWM */
	uint8_t		newline_mode;	/* LNM: Enter sends CR LF, LF does CR */
	uint8_t		local_echo;
	uint8_t		visual_bell;
	char		answerback[SETTINGS_ANSWERBACK_MAX];	/* ENQ reply */
	uint32_t	crc;
};

void	settings_defaults(struct settings *settings);
/* Fill magic/version/size and the CRC; call before saving. */
void	settings_seal(struct settings *settings);
/* True if `settings` is a sealed record of this version. */
bool	settings_valid(const struct settings *settings);

const char *settings_scheme_name(unsigned scheme);
/* Default foreground/background of a scheme, as 8-bit RGB triples. */
void	settings_scheme_rgb(unsigned scheme, uint8_t fg[3], uint8_t bg[3]);

#endif /* SETTINGS_H */

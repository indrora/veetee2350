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
#define SETTINGS_VERSION	4	/* 2 resolution, 3 size, 4 emulation */
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

/*
 * Terminal size choices. SIZE_FULL uses every cell the font and resolution
 * give; the fixed sizes are letterboxed, and clamped if they don't fit.
 */
enum terminal_size {
	SIZE_FULL,
	SIZE_80x24,
	SIZE_80x25,
	SIZE_80x43,
	SIZE_80x50,
	SIZE_132x24,
	SIZE_132x43,
	SIZE_COUNT
};

struct settings_size {
	uint16_t	cols;		/* 0 = as many as fit */
	uint16_t	rows;
};

extern const struct settings_size settings_sizes[SIZE_COUNT];

struct settings {
	uint32_t	magic;
	uint16_t	version;
	uint16_t	size;		/* sizeof(struct settings) when sealed */
	uint8_t		resolution;	/* enum video_mode_id (video_modes.h) */
	char		font[SETTINGS_FONT_MAX];	/* built-in font name */
	uint8_t		term_size;	/* enum terminal_size */
	uint8_t		scheme;		/* enum colour_scheme */
	uint8_t		cursor_shape;	/* enum cursor_shape (grid.h) */
	uint8_t		cursor_blink;
	uint8_t		autowrap;	/* DECAWM */
	uint8_t		newline_mode;	/* LNM: Enter sends CR LF, LF does CR */
	uint8_t		local_echo;
	uint8_t		visual_bell;
	char		answerback[SETTINGS_ANSWERBACK_MAX];	/* ENQ reply */
	uint8_t		emulation;	/* enum term_emulation (term.h) */
	uint8_t		colour_depth;	/* enum colour_depth (term.h) */
	uint8_t		utf8;
	uint8_t		alt_screen;
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

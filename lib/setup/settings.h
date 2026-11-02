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
#define SETTINGS_VERSION	10	/* 2 resolution, 3 size, 4 emulation, 5 sixel,
				   6 card fonts and schemes, 7 screenshots,
				   8 printer, 9 sixel depth, 10 hotkeys,
				   status line, capture */
#define SETTINGS_FONT_MAX	32	/* built-in name, or "sd:" + card file */
#define SETTINGS_SCHEME_MAX	32	/* card scheme file name, no ".yaml" */
#define SETTINGS_CARD_PREFIX	"sd:"
#define SETTINGS_ANSWERBACK_MAX	32

/*
 * Things a hotkey (or the Tools menu) can do. The order is the Tools
 * menu's; the values are stored in settings, so only ever append.
 */
enum tool {
	TOOL_MENU,		/* open the Tools menu itself */
	TOOL_SETUP,
	TOOL_SCREENSHOT,
	TOOL_PRINT_SCREEN,
	TOOL_PRINTER,		/* printer on/off */
	TOOL_CAPTURE,		/* capture to the card on/off */
	TOOL_REPLAY,
	TOOL_SEND,
	TOOL_RECEIVE,
	TOOL_CLEAR,		/* clear the screen, home the cursor */
	TOOL_SOFT_RESET,	/* DECSTR */
	TOOL_HARD_RESET,	/* RIS */
	TOOL_COUNT
};

/*
 * A key that runs a tool: enum key_code (keys.h, KEY_NONE = unbound),
 * the byte for KEY_CHAR keys (Ctrl+] is KEY_CHAR 0x1d), and enum key_mod
 * bits.
 */
struct hotkey {
	uint8_t		code;
	uint8_t		byte;
	uint8_t		mods;
};

/* Where the status line goes; peeking ones show only after a change. */
enum status_line_mode {
	STATUS_HIDDEN,
	STATUS_BOTTOM,
	STATUS_TOP,
	STATUS_PEEK_BOTTOM,
	STATUS_PEEK_TOP,
	STATUS_MODE_COUNT
};

/* How long a peeking status line stays up: 3, 5, 7 or 10 seconds. */
#define SETTINGS_PEEK_TIMES	4
extern const uint8_t settings_peek_seconds[SETTINGS_PEEK_TIMES];

/* F12 saves the screen to /screens on the card in this format. */
enum screenshot_format {
	SHOT_BMP,		/* 16-bit RGB565, opens anywhere */
	SHOT_QOI,		/* lossless, a fraction of the size */
	SHOT_COUNT
};

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
	char		font[SETTINGS_FONT_MAX];	/* built-in or "sd:name" */
	uint8_t		term_size;	/* enum terminal_size */
	uint8_t		scheme;		/* enum colour_scheme */
	/* A base16 scheme from the card's /schemes, overriding `scheme`
	 * when not empty. */
	char		scheme_file[SETTINGS_SCHEME_MAX];
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
	uint8_t		sixel;		/* 0 off, else bits per pixel: 8/4/2/1 */
	uint8_t		screenshot_format;	/* enum screenshot_format */
	uint8_t		printer;	/* Media Copy to /print on the card */
	uint8_t		status_line;	/* enum status_line_mode */
	uint8_t		peek_time;	/* index into settings_peek_seconds */
	uint8_t		capture;	/* host output to /capture on the card */
	struct hotkey	hotkeys[TOOL_COUNT];
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

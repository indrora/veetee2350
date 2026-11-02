/*
 * keys.h — decode a keyboard byte stream into key events.
 *
 * Layering: pure module (host-testable).
 *
 * The keyboard port delivers what a terminal keyboard would send: plain
 * bytes, plus escape sequences for cursor and function keys (ECMA-48 CSI
 * "ESC [ ... final" and SS3 "ESC O x"). The device needs to recognise a few
 * of them (hotkeys such as Ctrl+F3 for the Tools menu; arrows and Enter
 * inside SET-UP) and pass the rest to the host unchanged, so every event
 * carries its raw bytes.
 *
 * A lone ESC is ambiguous until more bytes arrive or nothing does: keys_poll
 * reports it as KEY_ESCAPE after KEYS_ESC_TIMEOUT_MS of silence, the same
 * trick curses uses.
 */
#ifndef KEYS_H
#define KEYS_H

#include <stdbool.h>
#include <stdint.h>

#define KEYS_RAW_MAX		16
#define KEYS_ESC_TIMEOUT_MS	40

enum key_code {
	KEY_NONE,
	KEY_CHAR,	/* printable or other single byte (see .byte) */
	KEY_ENTER,
	KEY_TAB,
	KEY_BACKSPACE,
	KEY_ESCAPE,
	KEY_UP,
	KEY_DOWN,
	KEY_RIGHT,
	KEY_LEFT,
	KEY_HOME,
	KEY_END,
	KEY_INSERT,
	KEY_DELETE,
	KEY_PAGE_UP,
	KEY_PAGE_DOWN,
	KEY_F1,
	KEY_F2,
	KEY_F3,
	KEY_F4,
	KEY_F5,
	KEY_F6,
	KEY_F7,
	KEY_F8,
	KEY_F9,
	KEY_F10,
	KEY_F11,
	KEY_F12,
	KEY_UNKNOWN,	/* well-formed but unrecognised sequence */
	/*
	 * Appended (saved hotkeys store these numbers): keys only a USB
	 * keyboard reports as themselves (hidkbd.h). The keypad, for
	 * application keypad mode (DECKPAM); Print Screen and Pause, which
	 * send nothing to the host and so make good hotkeys.
	 */
	KEY_KP_0,
	KEY_KP_1,
	KEY_KP_2,
	KEY_KP_3,
	KEY_KP_4,
	KEY_KP_5,
	KEY_KP_6,
	KEY_KP_7,
	KEY_KP_8,
	KEY_KP_9,
	KEY_KP_DOT,
	KEY_KP_ENTER,
	KEY_KP_PLUS,
	KEY_KP_MINUS,
	KEY_KP_MULTIPLY,
	KEY_KP_DIVIDE,
	KEY_KP_EQUAL,
	KEY_PRINT_SCREEN,
	KEY_PAUSE,
	KEY_CODES	/* count */
};

/*
 * Modifier bits, as xterm encodes them: its parameter is 1 + these
 * ("CSI 1 ; 5 R" is Ctrl+F3). The USB HID keyboard will fill them in
 * directly.
 */
enum key_mod {
	KEY_MOD_SHIFT	= 1,
	KEY_MOD_ALT	= 2,
	KEY_MOD_CTRL	= 4,
	KEY_MOD_META	= 8,
};

struct key_event {
	enum key_code	code;
	uint8_t		byte;		/* for KEY_CHAR */
	uint8_t		mods;		/* enum key_mod bits */
	uint8_t		raw_len;
	uint8_t		raw[KEYS_RAW_MAX];
};

struct keys {
	uint8_t		buf[KEYS_RAW_MAX];
	uint8_t		len;
	uint32_t	started_ms;	/* when buf[0] (ESC) arrived */
};

void	keys_init(struct keys *keys);

/* Feed one byte. Returns true and fills *event when a key is complete. */
bool	keys_feed(struct keys *keys, uint8_t byte, uint32_t now_ms,
	    struct key_event *event);

/*
 * A key's name for menus: "Ctrl+F3", "Alt+x", "Ctrl+]", "(none)" for
 * KEY_NONE. `byte` matters for KEY_CHAR only. Always NUL-terminates.
 */
void	keys_name(enum key_code code, uint8_t byte, uint8_t mods, char *out,
	    unsigned size);

/* Call regularly: completes a pending lone ESC (or a stalled sequence). */
bool	keys_poll(struct keys *keys, uint32_t now_ms, struct key_event *event);

#endif /* KEYS_H */

/*
 * keys.c — keyboard escape-sequence decoder.
 *
 * Recognised sequences are what xterm/VT220 keyboards send:
 *   ESC [ A..D   / ESC O A..D     arrows (normal / application mode)
 *   ESC [ H, F   / ESC O H, F     home, end
 *   ESC O P..S                    F1..F4 (VT100 PF1..PF4)
 *   ESC [ n ~                     1 home 2 ins 3 del 4 end 5 pgup 6 pgdn
 *                                 7 home 8 end, 11..15 F1..F5,
 *                                 17..21 F6..F10, 23..24 F11..F12
 *   ESC O M                       keypad Enter
 * with xterm's modifier parameter: "ESC [ 1 ; m X", "ESC [ n ; m ~" and
 * the older "ESC O m X", where m - 1 is the key_mod bits (2 Shift, 3 Alt,
 * 5 Ctrl, ...). ESC followed by a printable byte is Alt plus that key.
 */
#include "keys.h"

#include <stdio.h>
#include <string.h>

void
keys_init(struct keys *keys)
{
	memset(keys, 0, sizeof(*keys));
}

static void
take_raw(struct keys *keys, struct key_event *event, enum key_code code,
    uint8_t mods)
{
	event->code = code;
	event->byte = keys->buf[0];
	event->mods = mods;
	event->raw_len = keys->len;
	memcpy(event->raw, keys->buf, keys->len);
	keys->len = 0;
}

static enum key_code
tilde_key(unsigned number)
{
	static const enum key_code table[] = {
		[1] = KEY_HOME, [2] = KEY_INSERT, [3] = KEY_DELETE,
		[4] = KEY_END, [5] = KEY_PAGE_UP, [6] = KEY_PAGE_DOWN,
		[7] = KEY_HOME, [8] = KEY_END,
		[11] = KEY_F1, [12] = KEY_F2, [13] = KEY_F3, [14] = KEY_F4,
		[15] = KEY_F5, [17] = KEY_F6, [18] = KEY_F7, [19] = KEY_F8,
		[20] = KEY_F9, [21] = KEY_F10, [23] = KEY_F11, [24] = KEY_F12,
	};

	if (number < sizeof(table) / sizeof(table[0]) && table[number] != 0)
		return table[number];
	return KEY_UNKNOWN;
}

static enum key_code
letter_key(uint8_t final)
{
	switch (final) {
	case 'A': return KEY_UP;
	case 'B': return KEY_DOWN;
	case 'C': return KEY_RIGHT;
	case 'D': return KEY_LEFT;
	case 'H': return KEY_HOME;
	case 'F': return KEY_END;
	case 'P': return KEY_F1;
	case 'Q': return KEY_F2;
	case 'R': return KEY_F3;
	case 'S': return KEY_F4;
	case 'M': return KEY_ENTER;	/* SS3 M: keypad Enter */
	default: return KEY_UNKNOWN;
	}
}

/* xterm's modifier parameter (1 + key_mod bits) as bits; 0 if absent. */
static uint8_t
modifier_bits(unsigned parameter)
{
	return parameter >= 2 && parameter <= 16 ? (uint8_t)(parameter - 1) : 0;
}

/*
 * Decode a complete CSI sequence in keys->buf (ESC [ params final): the
 * key from the first parameter (or the final byte), modifiers from the
 * second.
 */
static enum key_code
decode_csi(const struct keys *keys, uint8_t *mods)
{
	uint8_t final = keys->buf[keys->len - 1];
	unsigned params[2] = {0, 0}, count = 0;

	for (unsigned i = 2; i < keys->len - 1u; i++) {
		uint8_t byte = keys->buf[i];
		if (byte == ';') {
			if (++count == 2)
				break;
		} else if (byte >= '0' && byte <= '9') {
			params[count] = params[count] * 10 + (byte - '0');
		} else {
			return KEY_UNKNOWN;
		}
	}
	*mods = modifier_bits(params[1]);
	return final == '~' ? tilde_key(params[0]) : letter_key(final);
}

static enum key_code
single_byte_key(uint8_t byte)
{
	switch (byte) {
	case '\r':
	case '\n':
		return KEY_ENTER;
	case '\t':
		return KEY_TAB;
	case 0x7f:
	case 0x08:
		return KEY_BACKSPACE;
	default:
		return KEY_CHAR;
	}
}

bool
keys_feed(struct keys *keys, uint8_t byte, uint32_t now_ms,
    struct key_event *event)
{
	if (keys->len == 0) {
		if (byte == 0x1b) {
			keys->buf[keys->len++] = byte;
			keys->started_ms = now_ms;
			return false;
		}
		keys->buf[keys->len++] = byte;
		take_raw(keys, event, single_byte_key(byte), 0);
		return true;
	}

	keys->buf[keys->len++] = byte;
	if (keys->len == 2) {
		if (byte == '[' || byte == 'O')
			return false;
		/* ESC + a printable byte is Alt plus that key (xterm's
		 * metaSendsEscape); ESC + anything else passes on unknown. */
		if (byte >= 0x20 && byte < 0x7f) {
			take_raw(keys, event, KEY_CHAR, KEY_MOD_ALT);
			event->byte = byte;
		} else {
			take_raw(keys, event, KEY_UNKNOWN, 0);
		}
		return true;
	}

	if (keys->buf[1] == 'O') {
		/* SS3: one final byte, perhaps after a modifier digit (the
		 * older xterm form "ESC O 5 R"). */
		if (byte >= '0' && byte <= '9' && keys->len == 3)
			return false;
		uint8_t mods = keys->len == 4 ?
		    modifier_bits((unsigned)(keys->buf[2] - '0')) : 0;
		take_raw(keys, event, letter_key(byte), mods);
		return true;
	}
	/* CSI: parameter/intermediate bytes until a final byte. */
	if (byte >= 0x40 && byte <= 0x7e) {
		uint8_t mods = 0;
		enum key_code code = decode_csi(keys, &mods);
		take_raw(keys, event, code, mods);
		return true;
	}
	if (keys->len == KEYS_RAW_MAX) {
		take_raw(keys, event, KEY_UNKNOWN, 0);
		return true;
	}
	return false;
}

bool
keys_poll(struct keys *keys, uint32_t now_ms, struct key_event *event)
{
	if (keys->len == 0 || now_ms - keys->started_ms < KEYS_ESC_TIMEOUT_MS)
		return false;
	take_raw(keys, event, keys->len == 1 ? KEY_ESCAPE : KEY_UNKNOWN, 0);
	return true;
}

void
keys_name(enum key_code code, uint8_t byte, uint8_t mods, char *out,
    unsigned size)
{
	static const char *const names[] = {
		[KEY_NONE] = "(none)", [KEY_ENTER] = "Enter", [KEY_TAB] = "Tab",
		[KEY_BACKSPACE] = "Backspace", [KEY_ESCAPE] = "Esc",
		[KEY_UP] = "Up", [KEY_DOWN] = "Down", [KEY_RIGHT] = "Right",
		[KEY_LEFT] = "Left", [KEY_HOME] = "Home", [KEY_END] = "End",
		[KEY_INSERT] = "Insert", [KEY_DELETE] = "Delete",
		[KEY_PAGE_UP] = "PgUp", [KEY_PAGE_DOWN] = "PgDn",
		[KEY_F1] = "F1", [KEY_F2] = "F2", [KEY_F3] = "F3",
		[KEY_F4] = "F4", [KEY_F5] = "F5", [KEY_F6] = "F6",
		[KEY_F7] = "F7", [KEY_F8] = "F8", [KEY_F9] = "F9",
		[KEY_F10] = "F10", [KEY_F11] = "F11", [KEY_F12] = "F12",
		[KEY_UNKNOWN] = "?",
	};
	char key[8];
	const char *name = key;

	if (code == KEY_CHAR) {
		/* Control bytes read as Ctrl+letter, the way they're typed. */
		if (byte < 0x20) {
			mods |= KEY_MOD_CTRL;
			byte = (uint8_t)(byte + 0x40);
		}
		if (byte == ' ')
			name = "Space";
		else
			snprintf(key, sizeof(key), "%c",
			    byte < 0x7f ? byte : '?');
	} else if ((unsigned)code < sizeof(names) / sizeof(names[0]) &&
	    names[code] != nullptr) {
		name = names[code];
	} else {
		name = "?";
	}
	snprintf(out, size, "%s%s%s%s%s",
	    mods & KEY_MOD_CTRL ? "Ctrl+" : "",
	    mods & KEY_MOD_ALT ? "Alt+" : "",
	    mods & KEY_MOD_SHIFT ? "Shift+" : "",
	    mods & KEY_MOD_META ? "Meta+" : "", name);
}

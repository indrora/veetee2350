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
 * Modifier parameters (";2" etc.) are accepted and ignored.
 */
#include "keys.h"

#include <string.h>

void
keys_init(struct keys *keys)
{
	memset(keys, 0, sizeof(*keys));
}

static void
take_raw(struct keys *keys, struct key_event *event, enum key_code code)
{
	event->code = code;
	event->byte = keys->buf[0];
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

/* Decode a complete CSI sequence in keys->buf (ESC [ params final). */
static enum key_code
decode_csi(const struct keys *keys)
{
	uint8_t final = keys->buf[keys->len - 1];
	unsigned number = 0;

	if (final != '~')
		return letter_key(final);
	/* First parameter only; anything after ';' is a modifier. */
	for (unsigned i = 2; i < keys->len - 1u && keys->buf[i] != ';'; i++) {
		if (keys->buf[i] < '0' || keys->buf[i] > '9')
			return KEY_UNKNOWN;
		number = number * 10 + (keys->buf[i] - '0');
	}
	return tilde_key(number);
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
		take_raw(keys, event, single_byte_key(byte));
		return true;
	}

	keys->buf[keys->len++] = byte;
	if (keys->len == 2) {
		if (byte == '[' || byte == 'O')
			return false;
		/* ESC + anything else (Alt-key, ESC ESC...): pass it on. */
		take_raw(keys, event, KEY_UNKNOWN);
		return true;
	}

	if (keys->buf[1] == 'O') {	/* SS3: exactly one byte follows */
		take_raw(keys, event, letter_key(byte));
		return true;
	}
	/* CSI: parameter/intermediate bytes until a final byte. */
	if (byte >= 0x40 && byte <= 0x7e) {
		enum key_code code = decode_csi(keys);
		take_raw(keys, event, code);
		return true;
	}
	if (keys->len == KEYS_RAW_MAX) {
		take_raw(keys, event, KEY_UNKNOWN);
		return true;
	}
	return false;
}

bool
keys_poll(struct keys *keys, uint32_t now_ms, struct key_event *event)
{
	if (keys->len == 0 || now_ms - keys->started_ms < KEYS_ESC_TIMEOUT_MS)
		return false;
	take_raw(keys, event, keys->len == 1 ? KEY_ESCAPE : KEY_UNKNOWN);
	return true;
}

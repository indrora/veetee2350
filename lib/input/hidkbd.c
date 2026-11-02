/*
 * hidkbd.c — USB keyboard reports to key events (see hidkbd.h).
 *
 * Usage numbers are from the USB HID Usage Tables, keyboard page (0x07).
 *
 * The events match what keys.c makes of the same key typed in an xterm
 * on the Mac, so a hotkey bound from one fires from the other: a
 * printable key is KEY_CHAR with its character (Shift folded in, as it
 * is in a byte); Ctrl with a letter or one of @ [ \ ] ^ _ is the control
 * byte (0x01-0x1f), Ctrl not kept as a modifier; Alt (and GUI, as Meta)
 * stay as modifiers; other keys keep every modifier, as xterm's
 * "CSI 1 ; m" does.
 */
#include "hidkbd.h"

#include <string.h>

/* Modifier byte: left Ctrl/Shift/Alt/GUI in bits 0-3, right in 4-7. */
#define HID_CTRL	0x11
#define HID_SHIFT	0x22
#define HID_ALT		0x44
#define HID_GUI		0x88

#define USAGE_ERROR_ROLLOVER	0x01	/* too many keys: report means nothing */
#define USAGE_A			0x04
#define USAGE_CAPS_LOCK		0x39
#define USAGE_SCROLL_LOCK	0x47
#define USAGE_NUM_LOCK		0x53

/* Usages 0x04-0x38 on a US keyboard: unshifted, then shifted. */
static const char plain[] =
    "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\\\;'`,./";
static const char shifted[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}||:\"~<>?";

void
hidkbd_init(struct hidkbd *kbd)
{
	memset(kbd, 0, sizeof(*kbd));
	kbd->num_lock = true;	/* as most PCs boot */
}

uint8_t
hidkbd_leds(const struct hidkbd *kbd)
{
	return (uint8_t)((kbd->num_lock ? HIDKBD_LED_NUM : 0) |
	    (kbd->caps_lock ? HIDKBD_LED_CAPS : 0) |
	    (kbd->scroll_lock ? HIDKBD_LED_SCROLL : 0));
}

static uint8_t
key_mods(uint8_t modifiers)
{
	return (uint8_t)((modifiers & HID_SHIFT ? KEY_MOD_SHIFT : 0) |
	    (modifiers & HID_ALT ? KEY_MOD_ALT : 0) |
	    (modifiers & HID_CTRL ? KEY_MOD_CTRL : 0) |
	    (modifiers & HID_GUI ? KEY_MOD_META : 0));
}

/* The control byte Ctrl makes of `c`, or -1 (xterm's choices). */
static int
control_byte(char c)
{
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 1;
	if (c >= '@' && c <= '_')
		return c - '@';		/* @ A-Z [ \ ] ^ _ */
	switch (c) {
	case ' ':
	case '2':
		return 0x00;
	case '3':
		return 0x1b;
	case '4':
		return 0x1c;
	case '5':
		return 0x1d;
	case '6':
		return 0x1e;
	case '7':
	case '/':
		return 0x1f;
	case '8':
	case '?':
		return 0x7f;
	default:
		return -1;
	}
}

/* Keys that aren't characters (usage -> key code), 0 for none. */
static enum key_code
special_key(uint8_t usage, bool num_lock)
{
	static const uint8_t nav[] = {	/* 0x49-0x52 */
		KEY_INSERT, KEY_HOME, KEY_PAGE_UP, KEY_DELETE, KEY_END,
		KEY_PAGE_DOWN, KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
	};
	/* Keypad 1-9, 0, . with Num Lock off: the navigation printed on
	 * them (5 has none). */
	static const uint8_t keypad_nav[] = {	/* 0x59-0x63 */
		KEY_END, KEY_DOWN, KEY_PAGE_DOWN, KEY_LEFT, KEY_NONE,
		KEY_RIGHT, KEY_HOME, KEY_UP, KEY_PAGE_UP, KEY_INSERT,
		KEY_DELETE,
	};
	static const uint8_t keypad[] = {	/* 0x59-0x63 */
		KEY_KP_1, KEY_KP_2, KEY_KP_3, KEY_KP_4, KEY_KP_5, KEY_KP_6,
		KEY_KP_7, KEY_KP_8, KEY_KP_9, KEY_KP_0, KEY_KP_DOT,
	};

	if (usage >= 0x3a && usage <= 0x45)
		return (enum key_code)(KEY_F1 + (usage - 0x3a));
	if (usage >= 0x49 && usage <= 0x52)
		return (enum key_code)nav[usage - 0x49];
	if (usage >= 0x59 && usage <= 0x63)
		return (enum key_code)(num_lock ? keypad[usage - 0x59] :
		    keypad_nav[usage - 0x59]);
	switch (usage) {
	case 0x46:
		return KEY_PRINT_SCREEN;
	case 0x48:
		return KEY_PAUSE;
	case 0x54:
		return KEY_KP_DIVIDE;
	case 0x55:
		return KEY_KP_MULTIPLY;
	case 0x56:
		return KEY_KP_MINUS;
	case 0x57:
		return KEY_KP_PLUS;
	case 0x58:
		return KEY_KP_ENTER;
	case 0x64:		/* the ISO key beside left Shift */
		return KEY_CHAR;
	case 0x67:
		return KEY_KP_EQUAL;
	default:
		return KEY_NONE;
	}
}

/* What pressing `usage` with these modifiers is; false for nothing. */
static bool
usage_event(const struct hidkbd *kbd, uint8_t usage, struct key_event *out)
{
	const bool shift = kbd->modifiers & HID_SHIFT;
	uint8_t mods = key_mods(kbd->modifiers);

	memset(out, 0, sizeof(*out));
	if (usage == 0x64)
		usage = 0x31;	/* ISO \ |, where a US board has it */
	if (usage >= USAGE_A && usage <= 0x38) {
		unsigned index = usage - USAGE_A;
		char c = shift ? shifted[index] : plain[index];

		switch (c) {	/* the non-printing keys in the tables */
		case '\n':
			out->code = KEY_ENTER;
			out->mods = mods;
			return true;
		case '\x1b':
			out->code = KEY_ESCAPE;
			out->mods = mods;
			return true;
		case '\b':
			out->code = KEY_BACKSPACE;
			out->mods = mods;
			return true;
		case '\t':
			out->code = KEY_TAB;
			out->mods = mods;
			return true;
		default:
			break;
		}
		/* Caps Lock flips letters only, against Shift. */
		if (kbd->caps_lock && index < 26)
			c = shift ? plain[index] : shifted[index];
		out->code = KEY_CHAR;
		out->byte = (uint8_t)c;
		out->mods = (uint8_t)(mods & ~KEY_MOD_SHIFT);
		if (mods & KEY_MOD_CTRL) {
			int control = control_byte(c);
			if (control >= 0) {
				out->byte = (uint8_t)control;
				out->mods &= (uint8_t)~KEY_MOD_CTRL;
			}
		}
		return true;
	}
	enum key_code code = special_key(usage, kbd->num_lock);
	if (code == KEY_NONE || code == KEY_CHAR)
		return false;
	out->code = code;
	out->mods = mods;
	return true;
}

static bool
was_held(const struct hidkbd *kbd, uint8_t usage)
{
	for (unsigned i = 0; i < HIDKBD_KEYS; i++)
		if (kbd->held[i] == usage)
			return true;
	return false;
}

static bool
lock_key(struct hidkbd *kbd, uint8_t usage)
{
	switch (usage) {
	case USAGE_CAPS_LOCK:
		kbd->caps_lock = !kbd->caps_lock;
		return true;
	case USAGE_NUM_LOCK:
		kbd->num_lock = !kbd->num_lock;
		return true;
	case USAGE_SCROLL_LOCK:
		kbd->scroll_lock = !kbd->scroll_lock;
		return true;
	default:
		return false;
	}
}

unsigned
hidkbd_report(struct hidkbd *kbd, const uint8_t *report, size_t len,
    uint32_t now_ms, struct key_event *out, unsigned max)
{
	uint8_t now_held[HIDKBD_KEYS] = {0};
	unsigned count = 0;

	if (len < 1)
		return 0;
	for (size_t i = 2; i < len && i < 2 + HIDKBD_KEYS; i++)
		now_held[i - 2] = report[i];
	/* Too many keys at once: the keyboard can't say which. Ignore it. */
	if (now_held[0] == USAGE_ERROR_ROLLOVER)
		return 0;

	kbd->modifiers = report[0];
	for (unsigned i = 0; i < HIDKBD_KEYS; i++) {
		uint8_t usage = now_held[i];
		if (usage == 0 || was_held(kbd, usage))
			continue;
		if (lock_key(kbd, usage))
			continue;
		struct key_event event;
		if (!usage_event(kbd, usage, &event))
			continue;
		if (count < max)
			out[count++] = event;
		/* The newest key is the one that repeats. */
		kbd->repeat_usage = usage;
		kbd->repeat_at = now_ms + HIDKBD_REPEAT_DELAY_MS;
	}
	memcpy(kbd->held, now_held, sizeof(kbd->held));
	if (kbd->repeat_usage != 0 && !was_held(kbd, kbd->repeat_usage))
		kbd->repeat_usage = 0;		/* let go */
	return count;
}

bool
hidkbd_poll(struct hidkbd *kbd, uint32_t now_ms, struct key_event *out)
{
	if (kbd->repeat_usage == 0 || (int32_t)(now_ms - kbd->repeat_at) < 0)
		return false;
	kbd->repeat_at += HIDKBD_REPEAT_MS;
	/* Far behind (the main loop was busy): don't catch up in a burst. */
	if ((int32_t)(now_ms - kbd->repeat_at) > 0)
		kbd->repeat_at = now_ms + HIDKBD_REPEAT_MS;
	/* With the modifiers held now (Shift pressed mid-repeat counts). */
	return usage_event(kbd, kbd->repeat_usage, out);
}

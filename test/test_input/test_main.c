/*
 * test_input — keyboard escape-sequence decoding.
 */
#include <string.h>

#include <unity.h>

#include "hidkbd.h"
#include "keys.h"

static struct keys keys;

void
setUp(void)
{
	keys_init(&keys);
}

void
tearDown(void)
{
}

/* Feed a string at time `ms`; return the last completed key code. */
static enum key_code
feed(const char *bytes, uint32_t ms, struct key_event *event)
{
	enum key_code code = KEY_NONE;

	for (; *bytes != '\0'; bytes++)
		if (keys_feed(&keys, (uint8_t)*bytes, ms, event))
			code = event->code;
	return code;
}

static void
test_plain_bytes(void)
{
	struct key_event event;

	TEST_ASSERT_EQUAL(KEY_CHAR, feed("a", 0, &event));
	TEST_ASSERT_EQUAL('a', event.byte);
	TEST_ASSERT_EQUAL(KEY_ENTER, feed("\r", 0, &event));
	TEST_ASSERT_EQUAL(KEY_BACKSPACE, feed("\x7f", 0, &event));
	TEST_ASSERT_EQUAL(KEY_TAB, feed("\t", 0, &event));
}

static void
test_cursor_and_function_keys(void)
{
	struct key_event event;

	TEST_ASSERT_EQUAL(KEY_UP, feed("\x1b[A", 0, &event));
	TEST_ASSERT_EQUAL(KEY_LEFT, feed("\x1bOD", 0, &event));	/* DECCKM */
	TEST_ASSERT_EQUAL(KEY_F3, feed("\x1bOR", 0, &event));	/* xterm F3 */
	TEST_ASSERT_EQUAL(KEY_F3, feed("\x1b[13~", 0, &event));	/* VT220 F3 */
	TEST_ASSERT_EQUAL(KEY_F12, feed("\x1b[24~", 0, &event));
	TEST_ASSERT_EQUAL(KEY_PAGE_DOWN, feed("\x1b[6~", 0, &event));
	TEST_ASSERT_EQUAL(KEY_RIGHT, feed("\x1b[1;5C", 0, &event)); /* ctrl */
	TEST_ASSERT_EQUAL(6, event.raw_len);
	TEST_ASSERT_EQUAL_MEMORY("\x1b[1;5C", event.raw, 6);
}

static void
test_lone_escape_needs_timeout(void)
{
	struct key_event event;

	TEST_ASSERT_EQUAL(KEY_NONE, feed("\x1b", 100, &event));
	TEST_ASSERT_FALSE(keys_poll(&keys, 100 + KEYS_ESC_TIMEOUT_MS - 1,
	    &event));
	TEST_ASSERT_TRUE(keys_poll(&keys, 100 + KEYS_ESC_TIMEOUT_MS, &event));
	TEST_ASSERT_EQUAL(KEY_ESCAPE, event.code);
}

static void
test_unknown_sequences_pass_through(void)
{
	struct key_event event;

	TEST_ASSERT_EQUAL(KEY_UNKNOWN, feed("\x1b\x01", 0, &event));
	TEST_ASSERT_EQUAL_MEMORY("\x1b\x01", event.raw, 2);
	TEST_ASSERT_EQUAL(KEY_UNKNOWN, feed("\x1b[99~", 0, &event));
	TEST_ASSERT_EQUAL(5, event.raw_len);
}

static void
test_modifiers(void)
{
	struct key_event event;

	/* Alt-x: ESC x, raw kept for the host. */
	TEST_ASSERT_EQUAL(KEY_CHAR, feed("\x1bx", 0, &event));
	TEST_ASSERT_EQUAL('x', event.byte);
	TEST_ASSERT_EQUAL(KEY_MOD_ALT, event.mods);
	TEST_ASSERT_EQUAL_MEMORY("\x1bx", event.raw, 2);
	/* Ctrl+F3, the three ways xterm has sent it. */
	TEST_ASSERT_EQUAL(KEY_F3, feed("\x1b[1;5R", 0, &event));
	TEST_ASSERT_EQUAL(KEY_MOD_CTRL, event.mods);
	TEST_ASSERT_EQUAL(KEY_F3, feed("\x1b[13;5~", 0, &event));
	TEST_ASSERT_EQUAL(KEY_MOD_CTRL, event.mods);
	TEST_ASSERT_EQUAL(KEY_F3, feed("\x1bO5R", 0, &event));
	TEST_ASSERT_EQUAL(KEY_MOD_CTRL, event.mods);
	TEST_ASSERT_EQUAL(4, event.raw_len);
	/* Shift+Ctrl+Up; plain F3 has none. */
	TEST_ASSERT_EQUAL(KEY_UP, feed("\x1b[1;6A", 0, &event));
	TEST_ASSERT_EQUAL(KEY_MOD_SHIFT | KEY_MOD_CTRL, event.mods);
	TEST_ASSERT_EQUAL(KEY_F3, feed("\x1bOR", 0, &event));
	TEST_ASSERT_EQUAL(0, event.mods);
}

static void
test_key_names(void)
{
	char name[24];

	keys_name(KEY_F3, 0, KEY_MOD_CTRL, name, sizeof(name));
	TEST_ASSERT_EQUAL_STRING("Ctrl+F3", name);
	keys_name(KEY_CHAR, 0x1d, 0, name, sizeof(name));
	TEST_ASSERT_EQUAL_STRING("Ctrl+]", name);
	keys_name(KEY_CHAR, 'x', KEY_MOD_ALT, name, sizeof(name));
	TEST_ASSERT_EQUAL_STRING("Alt+x", name);
	keys_name(KEY_NONE, 0, 0, name, sizeof(name));
	TEST_ASSERT_EQUAL_STRING("(none)", name);
}

/* --- USB keyboard reports (hidkbd.c) ------------------------------------ */

static struct hidkbd kbd;
static struct key_event hid_events[8];

/* Send a boot report: modifiers and up to six usages. */
static unsigned
hid(uint8_t modifiers, uint8_t a, uint8_t b, uint32_t now)
{
	uint8_t report[8] = {modifiers, 0, a, b, 0, 0, 0, 0};

	return hidkbd_report(&kbd, report, sizeof(report), now, hid_events, 8);
}

static void
test_hid_characters(void)
{
	hidkbd_init(&kbd);
	TEST_ASSERT_EQUAL(1, hid(0, 0x04, 0, 0));		/* a */
	TEST_ASSERT_EQUAL(KEY_CHAR, hid_events[0].code);
	TEST_ASSERT_EQUAL('a', hid_events[0].byte);
	TEST_ASSERT_EQUAL(0, hid(0, 0x04, 0, 10));		/* still held */
	TEST_ASSERT_EQUAL(1, hid(0x02, 0x04, 0x1f, 20));	/* + 2, Shift */
	TEST_ASSERT_EQUAL('@', hid_events[0].byte);
	TEST_ASSERT_EQUAL(0, hid_events[0].mods);	/* Shift is in the char */
	hid(0, 0, 0, 30);
	/* Caps Lock: letters flip, digits don't; LED follows. */
	TEST_ASSERT_EQUAL(0, hid(0, 0x39, 0, 40));
	TEST_ASSERT_EQUAL_HEX8(HIDKBD_LED_NUM | HIDKBD_LED_CAPS,
	    hidkbd_leds(&kbd));
	hid(0, 0, 0, 50);
	hid(0, 0x05, 0, 60);
	TEST_ASSERT_EQUAL('B', hid_events[0].byte);
	hid(0x20, 0x05, 0x1e, 70);	/* right Shift: b back, 1 -> ! */
	TEST_ASSERT_EQUAL('!', hid_events[0].byte);
	hid(0, 0, 0, 80);
	hid(0x20, 0x05, 0, 90);
	TEST_ASSERT_EQUAL('b', hid_events[0].byte);
}

/* Ctrl+letter is its control byte, as an xterm on the Mac sends it. */
static void
test_hid_control_and_alt(void)
{
	hidkbd_init(&kbd);
	hid(0x01, 0x06, 0, 0);				/* Ctrl+C */
	TEST_ASSERT_EQUAL(KEY_CHAR, hid_events[0].code);
	TEST_ASSERT_EQUAL(0x03, hid_events[0].byte);
	TEST_ASSERT_EQUAL(0, hid_events[0].mods);
	hid(0, 0, 0, 10);
	hid(0x10, 0x30, 0, 20);				/* right Ctrl+] */
	TEST_ASSERT_EQUAL(0x1d, hid_events[0].byte);
	hid(0, 0, 0, 30);
	hid(0x04, 0x1b, 0, 40);				/* Alt+x */
	TEST_ASSERT_EQUAL('x', hid_events[0].byte);
	TEST_ASSERT_EQUAL(KEY_MOD_ALT, hid_events[0].mods);
	hid(0, 0, 0, 50);
	/* Ctrl+F3: the Tools menu's hotkey, as keys.c decodes it. */
	hid(0x01, 0x3c, 0, 60);
	TEST_ASSERT_EQUAL(KEY_F3, hid_events[0].code);
	TEST_ASSERT_EQUAL(KEY_MOD_CTRL, hid_events[0].mods);
	hid(0, 0, 0, 70);
	hid(0x02, 0x2b, 0, 80);				/* Shift+Tab */
	TEST_ASSERT_EQUAL(KEY_TAB, hid_events[0].code);
	TEST_ASSERT_EQUAL(KEY_MOD_SHIFT, hid_events[0].mods);
}

/* The same report again (PIO-USB after an unplug), and rollover. */
static void
test_hid_repeats_and_rollover(void)
{
	uint8_t rollover[8] = {0, 0, 1, 1, 1, 1, 1, 1};

	hidkbd_init(&kbd);
	TEST_ASSERT_EQUAL(1, hid(0, 0x04, 0, 0));
	for (int i = 0; i < 50; i++)
		TEST_ASSERT_EQUAL(0, hid(0, 0x04, 0, 1));
	TEST_ASSERT_EQUAL(0, hidkbd_report(&kbd, rollover, 8, 2, hid_events,
	    8));
	TEST_ASSERT_EQUAL(1, hid(0, 0x04, 0x05, 3));	/* only the new one */
	TEST_ASSERT_EQUAL('b', hid_events[0].byte);
}

static void
test_hid_keypad(void)
{
	hidkbd_init(&kbd);
	hid(0, 0x5f, 0, 0);		/* KP7, Num Lock on */
	TEST_ASSERT_EQUAL(KEY_KP_7, hid_events[0].code);
	hid(0, 0, 0, 10);
	hid(0, 0x53, 0, 20);		/* Num Lock off */
	hid(0, 0, 0, 30);
	hid(0, 0x5f, 0, 40);		/* KP7 is Home */
	TEST_ASSERT_EQUAL(KEY_HOME, hid_events[0].code);
	hid(0, 0, 0, 50);
	TEST_ASSERT_EQUAL(0, hid(0, 0x5d, 0, 60));	/* KP5: nothing */
	hid(0, 0x58, 0, 70);		/* KP Enter, either way */
	TEST_ASSERT_EQUAL(KEY_KP_ENTER, hid_events[0].code);
	TEST_ASSERT_EQUAL_HEX8(0, hidkbd_leds(&kbd));
}

/* The last key held repeats after 500 ms, then every 33; let go stops. */
static void
test_hid_auto_repeat(void)
{
	struct key_event event;

	hidkbd_init(&kbd);
	hid(0, 0x04, 0, 1000);
	TEST_ASSERT_FALSE(hidkbd_poll(&kbd, 1499, &event));
	TEST_ASSERT_TRUE(hidkbd_poll(&kbd, 1500, &event));
	TEST_ASSERT_EQUAL('a', event.byte);
	TEST_ASSERT_FALSE(hidkbd_poll(&kbd, 1532, &event));
	TEST_ASSERT_TRUE(hidkbd_poll(&kbd, 1533, &event));
	/* Shift pressed mid-repeat: the repeat follows it. */
	hid(0x02, 0x04, 0, 1540);
	TEST_ASSERT_TRUE(hidkbd_poll(&kbd, 1566, &event));
	TEST_ASSERT_EQUAL('A', event.byte);
	/* A second key takes over; letting it go stops the repeat. */
	hid(0, 0x04, 0x05, 1570);
	TEST_ASSERT_FALSE(hidkbd_poll(&kbd, 1600, &event));
	TEST_ASSERT_TRUE(hidkbd_poll(&kbd, 2070, &event));
	TEST_ASSERT_EQUAL('b', event.byte);
	hid(0, 0x04, 0, 2080);
	TEST_ASSERT_FALSE(hidkbd_poll(&kbd, 3000, &event));
	/* Lock keys never repeat. */
	hid(0, 0, 0, 3010);
	hid(0, 0x39, 0, 3020);
	TEST_ASSERT_FALSE(hidkbd_poll(&kbd, 4000, &event));
}

int
main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_plain_bytes);
	RUN_TEST(test_cursor_and_function_keys);
	RUN_TEST(test_lone_escape_needs_timeout);
	RUN_TEST(test_unknown_sequences_pass_through);
	RUN_TEST(test_modifiers);
	RUN_TEST(test_key_names);
	RUN_TEST(test_hid_characters);
	RUN_TEST(test_hid_control_and_alt);
	RUN_TEST(test_hid_repeats_and_rollover);
	RUN_TEST(test_hid_keypad);
	RUN_TEST(test_hid_auto_repeat);
	return UNITY_END();
}

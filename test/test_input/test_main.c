/*
 * test_input — keyboard escape-sequence decoding.
 */
#include <string.h>

#include <unity.h>

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
	return UNITY_END();
}

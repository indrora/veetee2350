/*
 * base16.c — read base16 scheme files. See base16.h for the format.
 *
 * Line by line: strip leading blanks, then look for "baseXX:" or a
 * "name:"/"scheme:" key. Values may be quoted ("..." or '...'), may start
 * with '#', and may have a trailing comment. Repeated keys: last wins.
 */
#include "base16.h"

#include <string.h>

static int
hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* [start, end) trimmed of blanks and one layer of quotes. */
static void
trim_value(const char **start, const char **end)
{
	const char *from = *start, *to = *end;

	while (from < to && (*from == ' ' || *from == '\t'))
		from++;
	/* "#" starts a comment only after a blank: "#ff0000" is a value. */
	for (const char *at = from + 1; at < to; at++) {
		if (*at == '#' && (at[-1] == ' ' || at[-1] == '\t')) {
			to = at;
			break;
		}
	}
	while (to > from && (to[-1] == ' ' || to[-1] == '\t' ||
	    to[-1] == '\r'))
		to--;
	if (to - from >= 2 && (*from == '"' || *from == '\'') &&
	    to[-1] == *from) {
		from++;
		to--;
	}
	*start = from;
	*end = to;
}

static bool
parse_colour(const char *from, const char *to, uint8_t rgb[3])
{
	if (from < to && *from == '#')
		from++;
	if (to - from != 6)
		return false;
	for (int i = 0; i < 3; i++) {
		int high = hex_digit(from[2 * i]), low = hex_digit(from[2 * i + 1]);
		if (high < 0 || low < 0)
			return false;
		rgb[i] = (uint8_t)(high << 4 | low);
	}
	return true;
}

/* Does [line, end) start with `key` followed by ':'? -> after the ':'. */
static const char *
match_key(const char *line, const char *end, const char *key)
{
	size_t len = strlen(key);

	if ((size_t)(end - line) <= len || memcmp(line, key, len) != 0 ||
	    line[len] != ':')
		return nullptr;
	return line + len + 1;
}

bool
base16_parse(const char *text, size_t len, struct base16 *scheme,
    const char **error)
{
	static const char *const missing[16] = {
		"base00 missing", "base01 missing", "base02 missing",
		"base03 missing", "base04 missing", "base05 missing",
		"base06 missing", "base07 missing", "base08 missing",
		"base09 missing", "base0A missing", "base0B missing",
		"base0C missing", "base0D missing", "base0E missing",
		"base0F missing",
	};
	static const char *const bad[16] = {
		"base00: not a colour", "base01: not a colour",
		"base02: not a colour", "base03: not a colour",
		"base04: not a colour", "base05: not a colour",
		"base06: not a colour", "base07: not a colour",
		"base08: not a colour", "base09: not a colour",
		"base0A: not a colour", "base0B: not a colour",
		"base0C: not a colour", "base0D: not a colour",
		"base0E: not a colour", "base0F: not a colour",
	};
	const char *end = text + len;
	uint16_t seen = 0;

	memset(scheme, 0, sizeof(*scheme));
	while (text < end) {
		const char *eol = memchr(text, '\n', (size_t)(end - text));
		const char *line = text;
		if (eol == nullptr)
			eol = end;
		text = eol + 1;

		while (line < eol && (*line == ' ' || *line == '\t'))
			line++;
		const char *value = nullptr;
		if ((value = match_key(line, eol, "name")) != nullptr ||
		    (value = match_key(line, eol, "scheme")) != nullptr) {
			const char *to = eol;
			trim_value(&value, &to);
			size_t n = (size_t)(to - value);
			if (n >= sizeof(scheme->name))
				n = sizeof(scheme->name) - 1;
			memcpy(scheme->name, value, n);
			scheme->name[n] = '\0';
			continue;
		}
		if (eol - line < 7 || memcmp(line, "base0", 5) != 0 ||
		    line[6] != ':')
			continue;
		int index = hex_digit(line[5]);
		if (index < 0)
			continue;
		value = line + 7;
		const char *to = eol;
		trim_value(&value, &to);
		if (!parse_colour(value, to, scheme->base[index])) {
			*error = bad[index];
			return false;
		}
		seen |= (uint16_t)(1u << index);
	}
	for (int index = 0; index < 16; index++) {
		if (!(seen & (1u << index))) {
			*error = seen == 0 ? "no base16 colours in file" :
			    missing[index];
			return false;
		}
	}
	return true;
}

void
base16_to_terminal(const struct base16 *scheme, struct base16_terminal *out)
{
	/* base16-shell's ANSI order: which baseXX each colour comes from. */
	static const uint8_t from[16] = {
		0x00, 0x08, 0x0B, 0x0A, 0x0D, 0x0E, 0x0C, 0x05,
		0x03, 0x08, 0x0B, 0x0A, 0x0D, 0x0E, 0x0C, 0x07,
	};

	for (int i = 0; i < 16; i++)
		memcpy(out->ansi[i], scheme->base[from[i]], 3);
	memcpy(out->fg, scheme->base[0x05], 3);
	memcpy(out->bg, scheme->base[0x00], 3);
}

/*
 * base16.h — base16 colour schemes (https://github.com/tinted-theming).
 *
 * A base16 scheme is sixteen colours, base00..base0F, with fixed roles:
 * 00-07 a ramp from background to brightest foreground, 08-0F the
 * accents (red, orange, yellow, green, cyan, blue, magenta, brown). The
 * files are YAML, in two generations:
 *
 *   tinted-theming (current)        base16 (older)
 *     system: "base16"                scheme: "3024"
 *     name: "3024"                    author: "..."
 *     palette:                        base00: "090300"
 *       base00: "#090300"             ...
 *       ...
 *
 * We don't need a YAML parser for this: each colour is one line
 * "baseXX: value", the value a six-digit hex colour with or without
 * quotes and '#'. Anything else (author, variant, comments) is skipped.
 *
 * Terminal mapping is base16-shell's, the one every base16 terminal
 * theme uses: ANSI 0-7 = base00, 08, 0B, 0A, 0D, 0E, 0C, 05; the bright
 * 8-15 = base03, 08, 0B, 0A, 0D, 0E, 0C, 07; foreground base05,
 * background base00.
 */
#ifndef BASE16_H
#define BASE16_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BASE16_NAME_MAX	48

struct base16 {
	char	name[BASE16_NAME_MAX];	/* "name:" or "scheme:", if any */
	uint8_t	base[16][3];		/* base00..base0F, 8-bit RGB */
};

struct base16_terminal {
	uint8_t	ansi[16][3];
	uint8_t	fg[3];
	uint8_t	bg[3];
};

/*
 * Parse a scheme file. On failure returns false and points *error at a
 * short reason ("base0C missing", "base03: not a colour").
 */
bool	base16_parse(const char *text, size_t len, struct base16 *scheme,
	    const char **error);
void	base16_to_terminal(const struct base16 *scheme,
	    struct base16_terminal *out);

#endif /* BASE16_H */

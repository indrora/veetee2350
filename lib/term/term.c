/*
 * term.c — libvterm state callbacks -> cell grid.
 *
 * Coordinates: libvterm uses VTermRect with half-open [start, end) rows
 * and columns, 0-based. Our grid uses logical rows (row_map), so all
 * cell access goes through grid_row().
 *
 * Colours: libvterm reports SGR colours as VTermColor (indexed, RGB, or
 * "default"). They're resolved to RGB565 when a cell is written, because
 * the grid stores render-ready colours. A side effect: changing the
 * palette (OSC 4) affects new text only, not text already on screen.
 * That's acceptable for now and noted in docs/terminal.md.
 */
#include "term.h"

#include "colour.h"
#include "vterm_internal.h"	/* modes and margins (vendored libvterm) */

/* printer.c (Media Copy). */
void	mc_feed(struct term *term, const uint8_t *bytes, size_t len);
void	printer_print_line(struct term *term);
void	term_feed_through(struct term *term, const uint8_t *bytes,
	    size_t len);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* xterm's default colours: light grey on black. */
#define DEFAULT_FG_RGB	229, 229, 229
#define DEFAULT_BG_RGB	0, 0, 0

/*
 * Emulation levels. DA1 attributes: 1 = 132 columns, 2 = printer port,
 * 6 = selective erase, 22 = ANSI colour; 62/63/64 = VT220/VT320/VT420
 * conformance. We only claim what we do (132 columns depends on the
 * resolution, so it's never claimed). DA2: terminal id ; firmware ; ROM.
 */
const struct term_emulation_info term_emulations[EMU_COUNT] = {
	[EMU_VT100] = {"VT100", "vt100", "?1;2c", nullptr,
	    DEPTH_MONO, false, false, 0},
	[EMU_VT220] = {"VT220", "vt220", "?62;6;22c", ">1;10;0c",
	    DEPTH_MONO, false, false, 0},
	[EMU_VT340] = {"VT340", "vt340", "?63;6;22c", ">19;10;0c",
	    DEPTH_16, false, false, 4},	/* 16 colour registers */
	[EMU_XTERM] = {"xterm", "xterm-256color", "?64;6;22c", ">41;390;0c",
	    DEPTH_TRUE, true, true, 8},
};

/*
 * SGR colour -> RGB565 at the configured depth. Mono ignores colours
 * (defaults only); 16 snaps everything to the ANSI palette; 256 snaps RGB
 * to the xterm cube/grey ramp; truecolor passes RGB through.
 */
static uint16_t
resolve_colour(const struct term *term, VTermColor colour, bool foreground)
{
	unsigned depth = term->options.colour_depth;

	if ((foreground && VTERM_COLOR_IS_DEFAULT_FG(&colour)) ||
	    (depth == DEPTH_MONO && foreground))
		return term->default_fg;
	if ((!foreground && VTERM_COLOR_IS_DEFAULT_BG(&colour)) ||
	    (depth == DEPTH_MONO && !foreground))
		return term->default_bg;

	bool indexed = VTERM_COLOR_IS_INDEXED(&colour);
	unsigned index = indexed ? colour.indexed.idx : 0;
	vterm_state_convert_color_to_rgb(term->state, &colour);
	uint8_t red = colour.rgb.red, green = colour.rgb.green;
	uint8_t blue = colour.rgb.blue;

	if (depth == DEPTH_16 && !(indexed && index < 16)) {
		const uint8_t *rgb = term->palette16[colour_nearest_16(red, green,
		    blue, (const uint8_t (*)[3])term->palette16)];
		red = rgb[0];
		green = rgb[1];
		blue = rgb[2];
	} else if (depth == DEPTH_256 && !indexed) {
		uint8_t rgb[3];
		colour_256_rgb(colour_nearest_256(red, green, blue), rgb);
		red = rgb[0];
		green = rgb[1];
		blue = rgb[2];
	}
	return grid_rgb565(red, green, blue);
}

/* Cache the 16 ANSI colours (for DEPTH_16 matching). */
static void
cache_palette16(struct term *term)
{
	for (int i = 0; i < 16; i++) {
		VTermColor colour;
		vterm_state_get_palette_color(term->state, i, &colour);
		vterm_state_convert_color_to_rgb(term->state, &colour);
		term->palette16[i][0] = colour.rgb.red;
		term->palette16[i][1] = colour.rgb.green;
		term->palette16[i][2] = colour.rgb.blue;
	}
}

/* A cell drawing `glyph` with the current pen (SGR state). */
static struct cell
pen_cell(const struct term *term, uint16_t glyph)
{
	const struct term_pen *pen = &term->pen;
	uint16_t fg = resolve_colour(term, pen->fg, true);
	uint16_t bg = resolve_colour(term, pen->bg, false);
	uint16_t attr = 0;

	if (pen->reverse) {
		uint16_t swap = fg;
		fg = bg;
		bg = swap;
	}
	if (pen->underline)
		attr |= CELL_UNDERLINE;
	if (pen->blink)
		attr |= CELL_BLINK;
	if (pen->conceal)
		attr |= CELL_INVISIBLE;
	if (pen->strike)
		attr |= CELL_STRIKE;
	/* No italic face: italic text renders upright. */
	return grid_make_cell(glyph, fg, bg, attr);
}

/*
 * The cell used for erasing. xterm-style "background colour erase": erased
 * cells take the current SGR background (honouring reverse), but no other
 * attributes.
 */
static struct cell
erase_cell(const struct term *term)
{
	uint16_t bg = term->pen.reverse ?
	    resolve_colour(term, term->pen.fg, true) :
	    resolve_colour(term, term->pen.bg, false);

	return grid_make_cell(term->space_glyph, term->default_fg, bg,
	    CELL_ERASED);
}

/* Push cursor/mode presentation state into the grid being shown. */
static void
sync_presentation(struct term *term)
{
	struct grid *grid = term->screen;

	grid->cursor_visible = term->cursor_visible;
	grid->cursor_blink = term->cursor_blink;
	grid->cursor_shape = term->cursor_shape;
	grid->reverse_video = term->reverse_video;
	grid->border2 = grid_dup565(term->default_bg);
}

/*
 * Clip a libvterm rect to the grid. Normally a no-op, but during a font
 * switch libvterm's resize logic can briefly issue rects for the old,
 * larger grid.
 */
static bool
clamp_rect(const struct grid *grid, VTermRect *rect)
{
	if (rect->start_row < 0)
		rect->start_row = 0;
	if (rect->start_col < 0)
		rect->start_col = 0;
	if (rect->end_row > grid->rows)
		rect->end_row = grid->rows;
	if (rect->end_col > grid->cols)
		rect->end_col = grid->cols;
	return rect->start_row < rect->end_row &&
	    rect->start_col < rect->end_col;
}

/* --- libvterm state callbacks --------------------------------------- */

static int
cb_putglyph(VTermGlyphInfo *info, VTermPos pos, void *user)
{
	struct term *term = user;
	struct grid *grid = term->screen;

	if (pos.row < 0 || pos.row >= grid->rows || pos.col < 0 ||
	    pos.col >= grid->cols)
		return 1;

	/* Combining characters (chars[1..]) are dropped: one glyph/cell. */
	uint16_t glyph = font_glyph_for(&term->font, info->chars[0]);
	struct cell *cells = grid_row(grid, pos.row);

	cells[pos.col] = pen_cell(term, glyph);
	if (info->protected_cell)
		cells[pos.col].attr |= CELL_PROTECTED;
	/*
	 * A double-width character covers two cells. Until wide glyphs are
	 * supported (Unicode stretch goal), show the glyph in the first cell
	 * and pen-coloured blank in the second so columns stay aligned.
	 */
	if (info->width == 2 && pos.col + 1 < grid->cols)
		cells[pos.col + 1] = pen_cell(term, term->space_glyph);
	return 1;
}

static int
cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
	struct term *term = user;

	(void)oldpos;
	(void)visible;	/* visibility arrives via settermprop */
	term->screen->cursor_row = (uint16_t)pos.row;
	term->screen->cursor_col = (uint16_t)pos.col;
	return 1;
}

/*
 * Fast path for the overwhelmingly common scroll: whole-width, vertical
 * only (LF at the bottom margin, IND/RI, IL/DL, SU/SD inside DECSTBM).
 * That's a row_map rotation in the grid. Anything else (horizontal
 * scrolling with DECSLRM, partial-width rects) returns 0 and libvterm falls
 * back to moverect + erase.
 */
static int
cb_scrollrect(VTermRect rect, int downward, int rightward, void *user)
{
	struct term *term = user;
	struct grid *grid = term->screen;

	if (rightward != 0 || rect.start_col != 0 || rect.end_col != grid->cols)
		return 0;
	if (downward > 0)
		grid_scroll_up(grid, rect.start_row, rect.end_row,
		    (unsigned)downward, erase_cell(term));
	else if (downward < 0)
		grid_scroll_down(grid, rect.start_row, rect.end_row,
		    (unsigned)-downward, erase_cell(term));
	return 1;
}

static int
cb_moverect(VTermRect dest, VTermRect src, void *user)
{
	struct term *term = user;
	struct grid *grid = term->screen;

	if (!clamp_rect(grid, &dest) || !clamp_rect(grid, &src))
		return 1;
	int height = src.end_row - src.start_row;
	int width = src.end_col - src.start_col;
	if (dest.end_row - dest.start_row < height)
		height = dest.end_row - dest.start_row;
	if (dest.end_col - dest.start_col < width)
		width = dest.end_col - dest.start_col;

	/*
	 * Copy rows in the order that never overwrites a source row before
	 * it's read; memmove handles overlap within a row.
	 */
	bool backwards = dest.start_row > src.start_row;
	for (int i = 0; i < height; i++) {
		int offset = backwards ? height - 1 - i : i;
		struct cell *to = grid_row(grid, dest.start_row + offset);
		struct cell *from = grid_row(grid, src.start_row + offset);
		memmove(&to[dest.start_col], &from[src.start_col],
		    (size_t)width * sizeof(struct cell));
	}
	return 1;
}

static int
cb_erase(VTermRect rect, int selective, void *user)
{
	struct term *term = user;
	struct cell blank = erase_cell(term);

	if (!clamp_rect(term->screen, &rect))
		return 1;
	for (int row = rect.start_row; row < rect.end_row; row++) {
		if (!selective) {
			grid_clear_span(term->screen, (unsigned)row,
			    (unsigned)rect.start_col, (unsigned)rect.end_col,
			    blank);
			continue;
		}
		/* DECSED/DECSEL: leave DECSCA-protected cells alone. */
		struct cell *cells = grid_row(term->screen, (unsigned)row);
		for (int col = rect.start_col; col < rect.end_col; col++)
			if (!(cells[col].attr & CELL_PROTECTED))
				cells[col] = blank;
	}
	return 1;
}

static int
cb_initpen(void *user)
{
	struct term *term = user;

	memset(&term->pen, 0, sizeof(term->pen));
	vterm_state_get_default_colors(term->state, &term->pen.fg,
	    &term->pen.bg);
	return 1;
}

static int
cb_setpenattr(VTermAttr attr, VTermValue *val, void *user)
{
	struct term_pen *pen = &((struct term *)user)->pen;

	switch (attr) {
	case VTERM_ATTR_BOLD:
		pen->bold = val->boolean;
		break;
	case VTERM_ATTR_UNDERLINE:
		pen->underline = val->number != 0;
		break;
	case VTERM_ATTR_ITALIC:
		pen->italic = val->boolean;
		break;
	case VTERM_ATTR_BLINK:
		pen->blink = val->boolean;
		break;
	case VTERM_ATTR_REVERSE:
		pen->reverse = val->boolean;
		break;
	case VTERM_ATTR_CONCEAL:
		pen->conceal = val->boolean;
		break;
	case VTERM_ATTR_STRIKE:
		pen->strike = val->boolean;
		break;
	case VTERM_ATTR_FOREGROUND:
		pen->fg = val->color;
		break;
	case VTERM_ATTR_BACKGROUND:
		pen->bg = val->color;
		break;
	default:
		/* FONT, SMALL, BASELINE: no visual representation yet. */
		break;
	}
	return 1;
}

/* Append a libvterm string fragment to a title buffer. */
static void
title_fragment(char *title, const VTermStringFragment *frag)
{
	size_t have = frag->initial ? 0 : strlen(title);
	size_t take = frag->len;

	if (have + take >= TERM_TITLE_MAX)
		take = TERM_TITLE_MAX - 1 - have;
	memcpy(title + have, frag->str, take);
	title[have + take] = '\0';
}

static int
cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
	struct term *term = user;

	switch (prop) {
	case VTERM_PROP_TITLE:
		title_fragment(term->title, &val->string);
		return 1;
	case VTERM_PROP_ICONNAME:
		title_fragment(term->icon_label, &val->string);
		return 1;
	case VTERM_PROP_CURSORVISIBLE:
		term->cursor_visible = val->boolean;
		break;
	case VTERM_PROP_CURSORBLINK:
		term->cursor_blink = val->boolean;
		break;
	case VTERM_PROP_CURSORSHAPE:
		term->cursor_shape = val->number == VTERM_PROP_CURSORSHAPE_UNDERLINE ?
		    CURSOR_UNDERLINE : val->number ==
		    VTERM_PROP_CURSORSHAPE_BAR_LEFT ? CURSOR_BAR : CURSOR_BLOCK;
		break;
	case VTERM_PROP_REVERSE:
		term->reverse_video = val->boolean;
		break;
	case VTERM_PROP_ALTSCREEN: {
		/*
		 * Alternate screen disabled (VT100/VT220, or by choice): stay
		 * on the primary, like xterm's titeInhibit. Returning 0 makes
		 * libvterm refuse the switch -- otherwise it would still erase
		 * "the alternate screen", which is our primary.
		 */
		if (!term->options.alt_screen)
			return 0;
		struct grid *next = val->boolean ? &term->alternate :
		    &term->primary;
		if (next == term->screen)
			break;
		/* No clearing here: libvterm erases the alternate screen
		 * when the mode calls for it (patch 25: 1049 on entry, 1047
		 * on exit, 47 never), through the erase callback. */
		next->cursor_row = term->screen->cursor_row;
		next->cursor_col = term->screen->cursor_col;
		term->screen = next;
		render_set_grid(term->render, next);
		break;
	}
	default:
		/* TITLE, ICONNAME, MOUSE, FOCUSREPORT: nothing to show. */
		return 1;
	}
	sync_presentation(term);
	return 1;
}

static int
cb_bell(void *user)
{
	((struct term *)user)->bells++;
	return 1;
}

/* DECSWL/DECDWL/DECDHL: remember the line size for the renderer. */
static int
cb_setlineinfo(int row, const VTermLineInfo *newinfo,
    const VTermLineInfo *oldinfo, void *user)
{
	struct term *term = user;
	uint8_t attr = ROW_NORMAL;

	(void)oldinfo;
	if (row < 0 || row >= term->screen->rows)
		return 1;
	if (newinfo->doubleheight == 1)
		attr = ROW_DOUBLE_TOP;
	else if (newinfo->doubleheight == 2)
		attr = ROW_DOUBLE_BOTTOM;
	else if (newinfo->doublewidth)
		attr = ROW_DOUBLE_WIDTH;
	*grid_row_attr(term->screen, (unsigned)row) = attr;
	return 1;
}

static int
cb_resize(int rows, int cols, VTermStateFields *fields, void *user)
{
	/* The grid size is fixed by the font; DECCOLM etc. can't change it. */
	(void)rows;
	(void)cols;
	(void)fields;
	(void)user;
	return 0;
}

static const VTermStateCallbacks state_callbacks = {
	.putglyph = cb_putglyph,
	.movecursor = cb_movecursor,
	.scrollrect = cb_scrollrect,
	.moverect = cb_moverect,
	.erase = cb_erase,
	.initpen = cb_initpen,
	.setpenattr = cb_setpenattr,
	.settermprop = cb_settermprop,
	.bell = cb_bell,
	.resize = cb_resize,
	.setlineinfo = cb_setlineinfo,
};

/* --- OSC colour control (xterm) ------------------------------------- */

static int
hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/*
 * Parse an X11 colour spec into 8-bit RGB:
 *   rgb:R/G/B   1-4 hex digits per channel, scaled to the full range
 *               (rgb:f0f0 -> 0xf0, rgb:f -> 0xff)
 *   #RGB...     3/6/9/12 digits, left-aligned per X11 (#fff -> 0xf0)
 *   rgbi:R/G/B  intensities 0.0..1.0
 * Names (e.g. "red") and the Xcms device-independent spaces (CIELab,
 * CIEXYZ, TekHVC...) are not supported: xterm gets those from Xlib's
 * display-dependent colour management.
 */
static bool
parse_colour_spec(const char *spec, size_t len, uint8_t rgb[3])
{
	if (len > 4 && memcmp(spec, "rgb:", 4) == 0) {
		size_t at = 4;
		for (int channel = 0; channel < 3; channel++) {
			uint32_t value = 0;
			unsigned digits = 0;
			while (at < len && spec[at] != '/') {
				int digit = hex_value(spec[at++]);
				if (digit < 0 || ++digits > 4)
					return false;
				value = value << 4 | (uint32_t)digit;
			}
			if (digits == 0 || (channel < 2 && at >= len))
				return false;
			at++;	/* skip '/' */
			uint32_t max = (1u << (4 * digits)) - 1;
			rgb[channel] = (uint8_t)((value * 255 + max / 2) / max);
		}
		return true;
	}
	if (len > 5 && memcmp(spec, "rgbi:", 5) == 0) {
		/* rgbi:R/G/B, each an intensity 0.0..1.0 (X11). Parsed by
		 * hand: no strtod dependence on locale or float printf. */
		size_t at = 5;
		for (int channel = 0; channel < 3; channel++) {
			uint32_t whole = 0, frac = 0, scale = 1;
			bool dot = false, any = false;
			while (at < len && spec[at] != '/') {
				char c = spec[at++];
				if (c == '.' && !dot) {
					dot = true;
				} else if (c >= '0' && c <= '9') {
					any = true;
					if (!dot)
						whole = whole * 10 + (uint32_t)(c - '0');
					else if (scale < 100000) {
						frac = frac * 10 + (uint32_t)(c - '0');
						scale *= 10;
					}
				} else {
					return false;
				}
			}
			if (!any || whole > 1 || (channel < 2 && at >= len))
				return false;
			at++;
			uint32_t v16 = whole ? 65535 : (frac * 65535 + scale / 2) / scale;
			rgb[channel] = (uint8_t)(v16 >> 8);
		}
		return true;
	}
	if (len >= 4 && spec[0] == '#' && (len - 1) % 3 == 0 && len - 1 <= 12) {
		unsigned digits = (unsigned)(len - 1) / 3;
		for (unsigned channel = 0; channel < 3; channel++) {
			uint32_t value = 0;
			for (unsigned i = 0; i < digits; i++) {
				int digit = hex_value(spec[1 + channel * digits + i]);
				if (digit < 0)
					return false;
				value = value << 4 | (uint32_t)digit;
			}
			/* Left-align to 16 bits, keep the top byte. */
			rgb[channel] = (uint8_t)((value << (16 - 4 * digits)) >> 8);
		}
		return true;
	}
	return false;
}

/* Reply "OSC <prefix> ; rgb:RRRR/GGGG/BBBB ST" (8-bit channels doubled). */
static void
reply_colour(struct term *term, const char *prefix, const uint8_t rgb[3])
{
	char reply[64];
	int len = snprintf(reply, sizeof(reply),
	    "\x1b]%s;rgb:%02x%02x/%02x%02x/%02x%02x\x1b\\", prefix,
	    rgb[0], rgb[0], rgb[1], rgb[1], rgb[2], rgb[2]);
	term->output(reply, (size_t)len, term->output_ctx);
}

static void
palette_get(struct term *term, int index, uint8_t rgb[3])
{
	VTermColor colour;

	vterm_state_get_palette_color(term->state, index, &colour);
	vterm_state_convert_color_to_rgb(term->state, &colour);
	rgb[0] = colour.rgb.red;
	rgb[1] = colour.rgb.green;
	rgb[2] = colour.rgb.blue;
}

static void
palette_set(struct term *term, int index, const uint8_t rgb[3])
{
	VTermColor colour;

	vterm_color_rgb(&colour, rgb[0], rgb[1], rgb[2]);
	vterm_state_set_palette_color(term->state, index, &colour);
	if (index < 16)
		memcpy(term->palette16[index], rgb, 3);
}

/* Default fg/bg follow OSC 10/11 (for text written from now on). */
static void
apply_default_colours(struct term *term)
{
	const uint8_t *fg = term->dynamic_colours[0];
	const uint8_t *bg = term->dynamic_colours[1];
	VTermColor vfg, vbg;

	term->default_fg = grid_rgb565(fg[0], fg[1], fg[2]);
	term->default_bg = grid_rgb565(bg[0], bg[1], bg[2]);
	vterm_color_rgb(&vfg, fg[0], fg[1], fg[2]);
	vterm_color_rgb(&vbg, bg[0], bg[1], bg[2]);
	vterm_state_set_default_colors(term->state, &vfg, &vbg);
}

/* Reset dynamic colours to the configured scheme (fg, bg; others fg/bg). */
static void
reset_dynamic_colour(struct term *term, unsigned which)
{
	const uint8_t *fg = term->options.fg_rgb;
	const uint8_t *bg = term->options.bg_rgb;
	/* 10 fg, 11 bg, 12 cursor, 13/14 mouse fg/bg, 15/16 Tek fg/bg,
	 * 17 highlight bg, 18 Tek cursor, 19 highlight fg. */
	static const bool is_background[10] = {
		false, true, false, false, true, false, true, true, false,
		false,
	};

	memcpy(term->dynamic_colours[which], is_background[which] ? bg : fg, 3);
}

/* Next ';'-separated field of buf[*at..len). */
static bool
next_field(const char *buf, size_t len, size_t *at, const char **field,
    size_t *field_len)
{
	if (*at > len)
		return false;
	*field = buf + *at;
	const char *semi = memchr(*field, ';', len - *at);
	*field_len = semi ? (size_t)(semi - *field) : len - *at;
	*at += *field_len + 1;
	return true;
}

static long
field_number(const char *field, size_t len)
{
	long value = 0;

	if (len == 0)
		return -1;
	for (size_t i = 0; i < len; i++) {
		if (field[i] < '0' || field[i] > '9')
			return -1;
		value = value * 10 + (field[i] - '0');
	}
	return value;
}

/* One OSC 104 (palette 0-255, specials 256-260) or 105 (specials) reset. */
static void
reset_colour(struct term *term, int command, long index)
{
	if (command == 104 && index < 256)
		palette_set(term, (int)index, term->palette_initial[index]);
	else
		memcpy(term->special_colours[command == 105 ? index :
		    index - 256], term->options.fg_rgb, 3);
}

/*
 * OSC 4 / 5 (pairs of index ; spec), 104 / 105 (reset listed or all),
 * 10-19 (successive specs), 110-119 (reset). "?" queries.
 */
static void
handle_colour_osc(struct term *term, int command, const char *buf, size_t len)
{
	const char *field, *spec;
	size_t field_len, spec_len, at = 0;
	uint8_t rgb[3];
	char prefix[16];

	switch (command) {
	case 4:
	case 5: {
		/* OSC 4 indices 256..260 alias the five special colours, as in
		 * xterm with a 256-colour palette. */
		int limit = command == 4 ? 256 + 5 : 5;
		while (next_field(buf, len, &at, &field, &field_len) &&
		    next_field(buf, len, &at, &spec, &spec_len)) {
			long index = field_number(field, field_len);
			if (index < 0 || index >= limit)
				continue;
			bool special = command == 5 || index >= 256;
			long slot = command == 5 ? index : index - 256;
			if (spec_len == 1 && spec[0] == '?') {
				if (!special)
					palette_get(term, (int)index, rgb);
				else
					memcpy(rgb, term->special_colours[slot], 3);
				snprintf(prefix, sizeof(prefix), "%d;%ld", command,
				    index);
				reply_colour(term, prefix, rgb);
			} else if (parse_colour_spec(spec, spec_len, rgb)) {
				if (!special)
					palette_set(term, (int)index, rgb);
				else
					memcpy(term->special_colours[slot], rgb, 3);
			}
		}
		break;
	}
	case 104:
	case 105: {
		/*
		 * As with OSC 4, 104's indices 256..260 are the special
		 * colours, and a bare OSC 104 resets those too: xterm's
		 * "reset all" runs over its whole colour table, specials
		 * included. (esctest relies on it: its per-test reset is a
		 * bare OSC 104, so specials changed by one test would
		 * otherwise leak into the next.)
		 */
		int limit = command == 104 ? 256 + 5 : 5;
		bool any = false;
		while (next_field(buf, len, &at, &field, &field_len)) {
			long index = field_number(field, field_len);
			if (index < 0 || index >= limit)
				continue;
			any = true;
			reset_colour(term, command, index);
		}
		for (int index = 0; !any && index < limit; index++)
			reset_colour(term, command, index);
		break;
	}
	default:
		if (command >= 10 && command <= 19) {
			for (int which = command - 10; which < 10 &&
			    next_field(buf, len, &at, &spec, &spec_len); which++) {
				if (spec_len == 1 && spec[0] == '?') {
					snprintf(prefix, sizeof(prefix), "%d",
					    10 + which);
					reply_colour(term, prefix,
					    term->dynamic_colours[which]);
				} else if (parse_colour_spec(spec, spec_len, rgb)) {
					memcpy(term->dynamic_colours[which], rgb, 3);
				}
			}
			apply_default_colours(term);
		} else if (command >= 110 && command <= 119) {
			reset_dynamic_colour(term, (unsigned)(command - 110));
			apply_default_colours(term);
		}
		break;
	}
}

/* Initial colour state: palette snapshot, specials and dynamics. */
static void
init_colour_state(struct term *term)
{
	for (int index = 0; index < 256; index++)
		palette_get(term, index, term->palette_initial[index]);
	for (int index = 0; index < 5; index++)
		memcpy(term->special_colours[index], term->options.fg_rgb, 3);
	for (unsigned which = 0; which < 10; which++)
		reset_dynamic_colour(term, which);
}

/* OSC 50 (xterm "set font") for the built-in fonts. */
static void
handle_font_osc(struct term *term, const char *arg)
{
	/* A card font isn't in the built-in list: step from the first. */
	bool builtin = term->font_source >= font_builtins &&
	    term->font_source < font_builtins + font_builtin_count;
	unsigned index = builtin ?
	    (unsigned)(term->font_source - font_builtins) : 0;

	if (strcmp(arg, "?") == 0) {
		term->output("\x1b]50;", 5, term->output_ctx);
		term->output(term->font_source->name,
		    strlen(term->font_source->name), term->output_ctx);
		term->output("\a", 1, term->output_ctx);
	} else if (strcmp(arg, "#+") == 0) {
		term->pending_font = &font_builtins[(index + 1) %
		    font_builtin_count];
	} else if (strcmp(arg, "#-") == 0) {
		term->pending_font = &font_builtins[(index + font_builtin_count -
		    1) % font_builtin_count];
	} else {
		/* Unknown names are ignored, as xterm does. */
		const struct font_builtin *found = font_builtin_find(arg);
		if (found != nullptr)
			term->pending_font = found;
	}
}

/*
 * OSC numbers libvterm doesn't handle (it does 0/1/2/52), via its
 * fallback. The string can arrive in several fragments.
 */
static int
fb_osc(int command, VTermStringFragment frag, void *user)
{
	struct term *term = user;
	bool colour = command == 4 || command == 5 || command == 104 ||
	    command == 105 || (command >= 10 && command <= 19) ||
	    (command >= 110 && command <= 119);

	if (command != 50 && !colour)
		return 0;
	if (frag.initial) {
		term->osc_len = 0;
		term->osc_overflow = false;
	}
	if (term->osc_len + frag.len >= sizeof(term->osc_buf))
		term->osc_overflow = true;
	else {
		memcpy(term->osc_buf + term->osc_len, frag.str, frag.len);
		term->osc_len += frag.len;
	}
	if (!frag.final || term->osc_overflow)
		return 1;
	term->osc_buf[term->osc_len] = '\0';

	if (command == 50)
		handle_font_osc(term, term->osc_buf);
	else
		handle_colour_osc(term, command, term->osc_buf, term->osc_len);
	return 1;
}

/* ENQ (0x05): reply with the answerback message, as VT terminals do. */
/* DECSCL level 61..65, mirrored into libvterm's limits (patch 18). */
static void
set_conformance(struct term *term, uint8_t level)
{
	term->conformance = level;
	term->state->vt_level = level - 60;
}

static int	fb_csi(const char *leader, const long args[], int argcount,
		    const char *intermed, char command, void *user);

static int
fb_control(unsigned char control, void *user)
{
	struct term *term = user;

	/*
	 * DECID, "ESC Z": the VT100's identify request. libvterm turns ESC
	 * plus 0x40-0x5F into the C1 control, so it arrives as SCI (0x9A).
	 * Answered exactly as DA1.
	 */
	if (control == 0x9a) {
		static const long none = CSI_ARG_MISSING;
		return fb_csi(nullptr, &none, 1, nullptr, 'c', user);
	}
	if (control != 0x05)
		return 0;
	term->output(term->options.answerback,
	    strnlen(term->options.answerback, sizeof(term->options.answerback)),
	    term->output_ctx);
	return 1;
}

static void
send_printf(struct term *term, const char *fmt, unsigned a, unsigned b,
    unsigned c, unsigned d)
{
	char reply[48];
	int len = snprintf(reply, sizeof(reply), fmt, a, b, c, d);

	if (len > 0)
		term->output(reply, (size_t)len, term->output_ctx);
}

/*
 * Modes we accept and report back (DECRQM) but that have no effect on
 * this hardware (no keyboard autorepeat, no printer, no 132-column
 * switch...). Remembering them is what xterm does for several, and it
 * keeps applications that toggle and restore them consistent.
 */
static const struct {
	uint16_t	num;
	bool		dec;		/* DEC private (?) or ANSI */
	uint8_t		initial;	/* DECRQM value: 1 set, 2 reset */
} stored_mode_table[] = {
	/* (DECARM, DECVCCM, DECPCCM, DECKBUM, DECXRLM, DECKPM aren't kept:
	 * like xterm, DECRQM reports them permanently reset.) */
	{2, false, 2},		/* KAM keyboard action */
	{12, false, 1},		/* SRM send/receive (set: no local echo) */
	{3, true, 2},		/* DECCOLM 132 columns, see set_columns() */
	{4, true, 2},		/* DECSCLM smooth scroll */
	{18, true, 2},		/* DECPFF print form feed */
	{19, true, 2},		/* DECPEX print extent */
	{34, true, 2},		/* DECRLM right-to-left */
	{40, true, 1},		/* xterm: allow 80 <-> 132 (DECCOLM) */
	{35, true, 2},		/* DECHEBM Hebrew keyboard */
	{36, true, 2},		/* DECHEM Hebrew encoding */
	{42, true, 2},		/* DECNRCM national replacement charsets */
	{57, true, 2},		/* DECNAKB Greek/N-A keyboard */
	{66, true, 2},		/* DECNKM numeric keypad */
	{67, true, 2},		/* DECBKM backarrow sends BS */
	{80, true, 2},		/* DECSDM sixel display (no scrolling) */
	{95, true, 2},		/* DECNCSM no clear on column change */
	{96, true, 2},		/* DECRLCM cursor right-to-left */
	{97, true, 2},		/* DECCRTSM CRT save */
	{98, true, 2},		/* DECARSM auto resize */
	{99, true, 2},		/* DECMCM modem control */
	{100, true, 2},		/* DECAAM auto answerback */
	{101, true, 2},		/* DECCANSM conceal answerback */
	{102, true, 2},		/* DECNULM ignore NUL */
	{103, true, 2},		/* DECHDPXM half duplex */
	{104, true, 2},		/* DECESKM secondary keyboard language */
	{106, true, 2},		/* DECOSCNM overscan */
	{1070, true, 1},	/* xterm: private sixel colour registers */
	{8452, true, 2},	/* xterm: cursor right of a sixel image */
};
static_assert(sizeof(stored_mode_table) / sizeof(stored_mode_table[0]) <=
    sizeof(((struct term *)0)->stored_modes));

/* ANSI modes ECMA-48 defines that a terminal like this never has. */
static bool
ansi_mode_permanently_reset(long num)
{
	switch (num) {
	case 1: case 5: case 7: case 10: case 11: case 13: case 14:
	case 15: case 16: case 17: case 18: case 19:
		return true;
	default:
		return false;
	}
}

static int
stored_mode_index(long num, bool dec)
{
	for (size_t i = 0; i < sizeof(stored_mode_table) /
	    sizeof(stored_mode_table[0]); i++)
		if (stored_mode_table[i].num == num &&
		    stored_mode_table[i].dec == dec)
			return (int)i;
	return -1;
}

/* For graphics.c: is DEC private mode `mode` (from the table) set? */
bool
term_dec_mode(const struct term *term, unsigned mode)
{
	int index = stored_mode_index(mode, true);

	return index >= 0 && term->stored_modes[index] == 1;
}

static void
init_stored_modes(struct term *term)
{
	for (size_t i = 0; i < sizeof(stored_mode_table) /
	    sizeof(stored_mode_table[0]); i++)
		term->stored_modes[i] = stored_mode_table[i].initial;
}

/*
 * Resolve a VT420 rectangle (1-based Pt;Pl;Pb;Pr from args[first..]) to
 * 0-based inclusive grid coordinates, honouring DECOM. False if empty.
 */
static bool
rect_from_args(const struct term *term, const long args[], int argcount,
    int first, long *top, long *left, long *bottom, long *right)
{
	const struct grid *grid = term->screen;
	long p[4];

	for (int i = 0; i < 4; i++) {
		long v = first + i < argcount ? CSI_ARG(args[first + i]) :
		    CSI_ARG_MISSING;
		p[i] = v == CSI_ARG_MISSING || v == 0 ? -1 : v;
	}
	long row0 = 0, col0 = 0;
	long max_row = grid->rows, max_col = grid->cols;
	if (term->state->mode.origin) {
		row0 = term->state->scrollregion_top;
		col0 = SCROLLREGION_LEFT(term->state);
		max_row = SCROLLREGION_BOTTOM(term->state);
		max_col = SCROLLREGION_RIGHT(term->state);
	}
	*top = (p[0] < 0 ? 1 : p[0]) + row0 - 1;
	*left = (p[1] < 0 ? 1 : p[1]) + col0 - 1;
	*bottom = p[2] < 0 ? max_row - 1 : p[2] + row0 - 1;
	*right = p[3] < 0 ? max_col - 1 : p[3] + col0 - 1;
	if (*bottom > max_row - 1)
		*bottom = max_row - 1;
	if (*right > max_col - 1)
		*right = max_col - 1;
	return *top <= *bottom && *left <= *right;
}

/*
 * DECFRA  CSI Pch;Pt;Pl;Pb;Pr $ x   fill with a character, current SGR
 * DECERA  CSI Pt;Pl;Pb;Pr $ z       erase
 * DECSERA CSI Pt;Pl;Pb;Pr $ {       selective erase (skip DECSCA cells)
 * DECCRA  CSI Pts;Pls;Pbs;Prs;Pps;Ptd;Pld;Ppd $ v   copy (pages ignored)
 * The cursor doesn't move.
 */
static void
rect_operation(struct term *term, const long args[], int argcount,
    char command)
{
	struct grid *grid = term->screen;
	long top, left, bottom, right;
	int first = command == 'x' ? 1 : 0;

	if (!rect_from_args(term, args, argcount, first, &top, &left, &bottom,
	    &right))
		return;

	if (command == 'x') {
		long ch = argcount > 0 ? CSI_ARG(args[0]) : CSI_ARG_MISSING;
		if (ch == CSI_ARG_MISSING || !((ch >= 32 && ch <= 126) ||
		    (ch >= 160 && ch <= 255)))
			return;
		struct cell fill = pen_cell(term, font_glyph_for(&term->font,
		    (uint32_t)ch));
		for (long row = top; row <= bottom; row++) {
			struct cell *cells = grid_row(grid, (unsigned)row);
			for (long col = left; col <= right; col++)
				cells[col] = fill;
		}
		return;
	}
	if (command == 'z' || command == '{') {
		/*
		 * DECERA erases everything; DECSERA spares protected cells,
		 * but only DEC (DECSCA) protection: after SPA (libvterm patch
		 * 27) the protection is ISO's, which DECSERA doesn't honour.
		 */
		struct cell blank = erase_cell(term);
		bool spare = command == '{' && !term->state->protect_iso;
		for (long row = top; row <= bottom; row++) {
			struct cell *cells = grid_row(grid, (unsigned)row);
			for (long col = left; col <= right; col++)
				if (!spare ||
				    !(cells[col].attr & CELL_PROTECTED))
					cells[col] = blank;
		}
		return;
	}

	/* DECCRA: destination top/left at args[5], args[6] (page args 4, 7). */
	long dest_top_arg = argcount > 5 ? CSI_ARG(args[5]) : CSI_ARG_MISSING;
	long dest_left_arg = argcount > 6 ? CSI_ARG(args[6]) : CSI_ARG_MISSING;
	long row0 = term->state->mode.origin ? term->state->scrollregion_top : 0;
	long col0 = term->state->mode.origin ? SCROLLREGION_LEFT(term->state) : 0;
	long dest_top = (dest_top_arg == CSI_ARG_MISSING || dest_top_arg == 0 ?
	    1 : dest_top_arg) + row0 - 1;
	long dest_left = (dest_left_arg == CSI_ARG_MISSING || dest_left_arg == 0 ?
	    1 : dest_left_arg) + col0 - 1;
	long height = bottom - top + 1, width = right - left + 1;
	if (dest_top + height > grid->rows)
		height = grid->rows - dest_top;
	if (dest_left + width > grid->cols)
		width = grid->cols - dest_left;
	if (height <= 0 || width <= 0)
		return;

	/* Through a temporary copy, so overlapping rectangles work. Not on
	 * the scanline path; a transient allocation is fine here. */
	struct cell *copy = malloc((size_t)(height * width) * sizeof(*copy));
	if (copy == nullptr)
		return;
	for (long r = 0; r < height; r++)
		memcpy(&copy[r * width], &grid_row(grid, (unsigned)(top + r))[left],
		    (size_t)width * sizeof(*copy));
	for (long r = 0; r < height; r++)
		memcpy(&grid_row(grid, (unsigned)(dest_top + r))[dest_left],
		    &copy[r * width], (size_t)width * sizeof(*copy));
	free(copy);
}

/* In-band resize report (DECSET 2048): CSI 48 ; rows ; cols ; h ; w t. */
static void
send_size_report(struct term *term)
{
	const struct grid *grid = term->screen;

	send_printf(term, "\x1b[48;%u;%u;%u;%ut", grid->rows, grid->cols,
	    grid->rows * term->font.height, grid->cols * term->font.width);
}

/* The size SET-UP asks for: what fits, capped by a fixed terminal size. */
static void
configured_size(const struct term *term, unsigned *cols, unsigned *rows)
{
	render_grid_size(&term->font, term->render->width,
	    term->render->height, cols, rows);
	if (term->options.max_rows != 0 && term->options.max_rows < *rows)
		*rows = term->options.max_rows;
	if (term->options.max_cols != 0 && term->options.max_cols < *cols)
		*cols = term->options.max_cols;
}

/*
 * Reallocate both screens at `cols` columns, keeping the row count. With
 * `keep` (DECNCSM) the old contents are copied across, clipped; that
 * needs both sizes in memory at once, so if the new grids can't be had
 * alongside the old ones, we fall back to a blank switch.
 */
static bool
regrid_columns(struct term *term, unsigned cols, bool keep)
{
	struct grid *screens[2] = {&term->primary, &term->alternate};
	unsigned rows = term->primary.rows;
	struct cell blank = grid_make_cell(term->space_glyph, term->default_fg,
	    term->default_bg, CELL_ERASED);
	struct grid next;

	for (unsigned i = 0; i < 2; i++) {
		struct grid *old = screens[i];

		if (keep && grid_init(&next, cols, rows, blank)) {
			unsigned width = cols < old->cols ? cols : old->cols;
			for (unsigned row = 0; row < rows; row++) {
				memcpy(grid_row(&next, row), grid_row(old, row),
				    width * sizeof(struct cell));
				*grid_row_attr(&next, row) =
				    *grid_row_attr(old, row);
			}
			grid_free(old);
		} else {
			grid_free(old);
			if (!grid_init(&next, cols, rows, blank))
				return false;
		}
		*old = next;
	}
	return true;
}

/*
 * DECCOLM, "CSI ? 3 h" / "CSI ? 3 l": 132 columns / back to normal.
 *
 * A VT100 switched between 80 and 132. Here "normal" is the width SET-UP
 * configured (80 for the fixed 80xN sizes, the whole screen for "full"),
 * so a host's `reset` (which sends ?3l) can't shrink a full-width
 * screen. 132 is only honoured when the resolution and font have room
 * for it -- 1056 px with an 8 px font, so 960x540 and the 720p modes,
 * not 640x480 or 800x480. Otherwise the request is ignored, as xterm
 * ignores it with 80/132 switching disallowed (mode 40 reset), and
 * DECRQM keeps reporting the old state.
 *
 * Per DEC STD 070 a switch clears the screen (unless DECNCSM, mode 95,
 * at level 5),
 * homes the cursor and resets the top/bottom and left/right margins,
 * even when the width doesn't actually change.
 *
 * This runs synchronously inside libvterm's CSI dispatch, unlike a font
 * switch, so the bytes that follow in the same chunk already land at the
 * new width (vttest draws right after ?3h). That's safe because
 * vterm_set_size() only touches the state layer (bounds, tab stops, line
 * info -- see libvterm patch 9), never the parser that's calling us.
 */
static void
set_columns(struct term *term, bool wide)
{
	VTermState *state = term->state;
	unsigned cols, rows;
	/* DECNCSM is a VT510 feature: below conformance level 5 (DECSCL
	 * 65) the switch clears whatever mode 95 says, as xterm does. */
	bool keep = term->conformance >= 65 &&
	    term->stored_modes[stored_mode_index(95, true)] == 1;

	if (term->stored_modes[stored_mode_index(40, true)] != 1)
		return;
	if (wide) {
		/*
		 * Where 132 columns don't fit, do what xterm does when it
		 * can't resize: the mode is set and the DEC side effects
		 * (clear, home, margins) happen, at the current width.
		 */
		render_grid_size(&term->font, term->render->width,
		    term->render->height, &cols, &rows);
		cols = cols >= 132 ? 132 : term->primary.cols;
	} else {
		configured_size(term, &cols, &rows);
	}
	term->stored_modes[stored_mode_index(3, true)] = wide ? 1 : 2;

	rows = term->primary.rows;
	bool resized = cols != term->primary.cols;
	if (resized) {
		render_pause(term->render);
		/* Images don't survive the switch, even with DECNCSM: the
		 * pool is given back so the grids can grow into it. */
		graphics_release(term);
		if (!regrid_columns(term, cols, keep))
			return;		/* out of memory: nothing to draw */
		render_init(term->render, &term->font, term->screen);
		graphics_acquire(term);
		vterm_set_size(term->vt, (int)rows, (int)cols);
		sync_presentation(term);
	} else if (!keep) {
		struct cell blank = grid_make_cell(term->space_glyph,
		    term->default_fg, term->default_bg, CELL_ERASED);
		for (unsigned row = 0; row < rows; row++) {
			grid_clear_span(term->screen, row, 0, cols, blank);
			*grid_row_attr(term->screen, row) = ROW_NORMAL;
		}
	}

	/* Margins off, cursor home, no double-size lines left behind. */
	state->scrollregion_top = 0;
	state->scrollregion_bottom = -1;
	state->scrollregion_left = 0;
	state->scrollregion_right = -1;
	state->at_phantom = 0;
	state->pos = (VTermPos){0, 0};
	if (!keep)
		for (unsigned buf = 0; buf < 2; buf++)
			if (state->lineinfos[buf] != nullptr)
				memset(state->lineinfos[buf], 0,
				    rows * sizeof(VTermLineInfo));
	term->screen->cursor_row = 0;
	term->screen->cursor_col = 0;
	if (resized && term->resize_reports)
		send_size_report(term);
}

/*
 * CSI sequences libvterm doesn't handle (and, via our libvterm patch,
 * unknown DEC private modes and their DECRQM queries).
 */
static int
fb_csi(const char *leader, const long args[], int argcount,
    const char *intermed, char command, void *user)
{
	struct term *term = user;
	const struct grid *grid = term->screen;
	bool private = leader != nullptr && leader[0] == '?';
	bool no_leader = leader == nullptr || leader[0] == '\0';
	long first = argcount > 0 ? CSI_ARG(args[0]) : CSI_ARG_MISSING;

	/* Device attributes, per emulation level (libvterm patch 2). */
	if (command == 'c' && intermed == nullptr &&
	    (first == CSI_ARG_MISSING || first == 0)) {
		const struct term_emulation_info *info =
		    &term_emulations[term->options.emulation % EMU_COUNT];
		const char *reply = no_leader ? info->da1 :
		    (leader[0] == '>' ? info->da2 : nullptr);
		if (no_leader || leader[0] == '>') {
			if (reply != nullptr) {
				/* DA1 with Sixel: ";4" after the class. */
				size_t head = strcspn(reply, ";c");
				term->output("\x1b[", 2, term->output_ctx);
				if (no_leader && term->options.sixel &&
				    term->graphics.tiles != 0) {
					term->output(reply, head,
					    term->output_ctx);
					term->output(";4", 2, term->output_ctx);
					reply += head;
				}
				term->output(reply, strlen(reply),
				    term->output_ctx);
			}
			return 1;	/* VT100 has no DA2: stay silent */
		}
	}

	/*
	 * DECRQCRA, "CSI Pid ; Pp ; Pt ; Pl ; Pb ; Pr * y": 16-bit sum of the
	 * characters in a rectangle (1-based, inclusive, defaults to the whole
	 * screen; Pp, the page, is ignored). Reply DCS Pid ! ~ XXXX ST.
	 * Written cells count their codepoint; erased cells count as a space,
	 * as xterm does since patch 334 (we claim 390 in DA2; esctest's
	 * empty() expects ' ' for xterm >= 334). esctest reads the screen
	 * back one cell at a time this way. Attributes
	 * aren't included. Origin mode isn't applied yet.
	 */
	if (no_leader && intermed != nullptr && intermed[0] == '*' &&
	    command == 'y') {
		long param[6];
		for (int i = 0; i < 6; i++)
			param[i] = i < argcount ? CSI_ARG(args[i]) :
			    CSI_ARG_MISSING;
		long top = param[2] == CSI_ARG_MISSING ? 1 : param[2];
		long left = param[3] == CSI_ARG_MISSING ? 1 : param[3];
		long bottom = param[4] == CSI_ARG_MISSING ? grid->rows : param[4];
		long right = param[5] == CSI_ARG_MISSING ? grid->cols : param[5];
		/* DECOM: coordinates are relative to the margins. */
		if (term->state->mode.origin) {
			long row0 = term->state->scrollregion_top;
			long col0 = SCROLLREGION_LEFT(term->state);
			top += row0;
			bottom += row0;
			left += col0;
			right += col0;
			if (param[4] == CSI_ARG_MISSING)
				bottom = SCROLLREGION_BOTTOM(term->state);
			if (param[5] == CSI_ARG_MISSING)
				right = SCROLLREGION_RIGHT(term->state);
		}
		if (top < 1)
			top = 1;
		if (left < 1)
			left = 1;
		if (bottom > grid->rows)
			bottom = grid->rows;
		if (right > grid->cols)
			right = grid->cols;

		uint32_t sum = 0;
		for (long row = top; row <= bottom; row++) {
			const struct cell *cells = grid_row(term->screen,
			    (unsigned)row - 1);
			for (long col = left; col <= right; col++)
				sum += (cells[col - 1].attr &
				    (CELL_ERASED | CELL_GRAPHIC)) ? 0x20 :
				    font_codepoint_for(&term->font,
				    cells[col - 1].glyph);
		}
		char reply[32];
		int len = snprintf(reply, sizeof(reply), "\x1bP%ld!~%04X\x1b\\",
		    param[0] == CSI_ARG_MISSING ? 0L : param[0],
		    (unsigned)(sum & 0xffff));
		term->output(reply, (size_t)len, term->output_ctx);
		return 1;
	}

	/*
	 * xterm window ops. There's no window to move, iconify or resize:
	 * those requests are accepted and remembered, so the matching
	 * reports agree with them (and esctest's checks pass). The title
	 * reports and the title stack are real. Resizing (4, 8) can't
	 * happen -- the size comes from SET-UP -- and is ignored.
	 */
	if (no_leader && intermed == nullptr && command == 't') {
		long arg1 = argcount > 1 ? CSI_ARG(args[1]) : CSI_ARG_MISSING;
		long arg2 = argcount > 2 ? CSI_ARG(args[2]) : CSI_ARG_MISSING;
		long which = arg1 == CSI_ARG_MISSING ? 0 : arg1;

		switch (first) {
		case 1:		/* de-iconify */
			term->iconified = false;
			return 1;
		case 2:		/* iconify */
			term->iconified = true;
			return 1;
		case 3:		/* move to x;y */
			term->window_x = arg1 == CSI_ARG_MISSING ? 0 :
			    (uint16_t)arg1;
			term->window_y = arg2 == CSI_ARG_MISSING ? 0 :
			    (uint16_t)arg2;
			return 1;
		case 4: case 5: case 6: case 7: case 8: case 9: case 10:
			return 1;	/* resize, raise, lower, ...: no window */
		case 11:	/* window state */
			send_printf(term, "\x1b[%ut", term->iconified ? 2u : 1u,
			    0, 0, 0);
			return 1;
		case 13:	/* window position */
			send_printf(term, "\x1b[3;%u;%ut", term->window_x,
			    term->window_y, 0, 0);
			return 1;
		case 15:	/* screen in pixels */
			send_printf(term, "\x1b[5;%u;%ut", term->render->height,
			    term->render->width, 0, 0);
			return 1;
		case 20:	/* icon label */
			term->output("\x1b]L", 3, term->output_ctx);
			term->output(term->icon_label, strlen(term->icon_label),
			    term->output_ctx);
			term->output("\x1b\\", 2, term->output_ctx);
			return 1;
		case 21:	/* window title */
			term->output("\x1b]l", 3, term->output_ctx);
			term->output(term->title, strlen(term->title),
			    term->output_ctx);
			term->output("\x1b\\", 2, term->output_ctx);
			return 1;
		case 22:	/* push (xterm saves both, whatever Ps says) */
			if (term->title_depth < TERM_TITLE_STACK) {
				memcpy(term->title_stack[term->title_depth].title,
				    term->title, TERM_TITLE_MAX);
				memcpy(term->title_stack[term->title_depth]
				    .icon_label, term->icon_label,
				    TERM_TITLE_MAX);
				term->title_depth++;
			}
			return 1;
		case 23:	/* pop: restore 0 both, 1 icon, 2 title */
			if (term->title_depth > 0) {
				term->title_depth--;
				if (which == 0 || which == 1)
					memcpy(term->icon_label, term->title_stack[
					    term->title_depth].icon_label,
					    TERM_TITLE_MAX);
				if (which == 0 || which == 2)
					memcpy(term->title, term->title_stack[
					    term->title_depth].title,
					    TERM_TITLE_MAX);
			}
			return 1;
		case 14:	/* text area in pixels */
			send_printf(term, "\x1b[4;%u;%ut", grid->rows *
			    term->font.height, grid->cols * term->font.width, 0, 0);
			return 1;
		case 16:	/* character cell in pixels */
			send_printf(term, "\x1b[6;%u;%ut", term->font.height,
			    term->font.width, 0, 0);
			return 1;
		case 18:	/* text area in characters */
			send_printf(term, "\x1b[8;%u;%ut", grid->rows,
			    grid->cols, 0, 0);
			return 1;
		case 19:	/* screen in characters: same, no window */
			send_printf(term, "\x1b[9;%u;%ut", grid->rows,
			    grid->cols, 0, 0);
			return 1;
		default:
			return 0;
		}
	}

	/*
	 * Media Copy (printer.c). CSI 4 i / 5 i (controller mode) are
	 * recognised before libvterm sees them; arriving here means there's
	 * no printer, and like the other MC controls they do nothing.
	 */
	if ((private || no_leader) && intermed == nullptr && command == 'i') {
		long what = first == CSI_ARG_MISSING ? 0 : first;
		if (no_leader && what == 0)
			printer_print_screen(term);
		else if (private && what == 1)
			printer_print_line(term);
		else if (private && (what == 4 || what == 5))
			term->auto_print = what == 5 && term->has_printer;
		else if (private && (what == 10 || what == 11))
			printer_print_screen(term);	/* one page: same */
		return 1;
	}

	/*
	 * XTSMGRAPHICS, "CSI ? Pi ; Pa ; Pv S": graphics capabilities, as
	 * Sixel-aware programs (notcurses, chafa) ask before sending images.
	 * Pi 1 = colour registers, 2 = Sixel geometry; Pa 1 read, 2 reset,
	 * 3 set (refused: fixed), 4 read maximum. Reply status 0 success,
	 * 1 bad Pi, 3 failure.
	 */
	if (private && intermed == nullptr && command == 'S') {
		long action = argcount > 1 ? CSI_ARG(args[1]) : 0;
		bool have = term->options.sixel && term->graphics.tiles != 0;
		unsigned width = grid->cols * term->font.width;
		unsigned height = grid->rows * term->font.height;

		if (!have || (first != 1 && first != 2))
			send_printf(term, "\x1b[?%u;1;0S", (unsigned)first, 0,
			    0, 0);
		else if (action == 3)
			send_printf(term, "\x1b[?%u;3;0S", (unsigned)first, 0,
			    0, 0);
		else if (first == 1)
			send_printf(term, "\x1b[?1;0;%uS", SIXEL_REGISTERS,
			    0, 0, 0);
		else
			send_printf(term, "\x1b[?2;0;%u;%uS", width, height,
			    0, 0);
		return 1;
	}

	/*
	 * DECSCL, "CSI Pl ; Pc \" p": select conformance level 61 (VT100)
	 * to 65 (VT500). A soft reset, as DEC specifies, and the level's
	 * limits (libvterm patch 18): no left/right margins below VT420,
	 * no DECRQM below VT320. (Pc, 7- or 8-bit controls, is ignored:
	 * 8-bit output isn't implemented.)
	 */
	if (no_leader && intermed != nullptr && intermed[0] == '"' &&
	    intermed[1] == '\0' && command == 'p') {
		if (first >= 61 && first <= 65) {
			/* No higher than the emulation level, as xterm caps
			 * DECSCL at its terminal ID (VT420 for us). */
			long most = 61 + term->options.emulation % EMU_COUNT;
			set_conformance(term, (uint8_t)(first < most ? first :
			    most));
			vterm_state_reset(term->state, 0);
		}
		return 1;
	}

	/* DECSACE, "CSI Ps * x": remembered for DECRQSS. */
	if (no_leader && intermed != nullptr && intermed[0] == '*' &&
	    intermed[1] == '\0' && command == 'x') {
		term->decsace = first == CSI_ARG_MISSING ? 0 : (uint8_t)first;
		return 1;
	}

	/* VT420 rectangle operations (libvterm has none). */
	if (no_leader && intermed != nullptr && intermed[0] == '$' &&
	    intermed[1] == '\0' &&
	    (command == 'x' || command == 'z' || command == '{' ||
	    command == 'v')) {
		rect_operation(term, args, argcount, command);
		return 1;
	}

	if (private && intermed == nullptr && first == 3 &&
	    (command == 'h' || command == 'l')) {
		set_columns(term, command == 'h');
		return 1;
	}

	/*
	 * XTSAVE / XTRESTORE ("CSI ? Pm s" / "CSI ? Pm r") of the stored
	 * modes. libvterm saves and restores its own (patch 22) and hands
	 * us the rest one mode at a time.
	 */
	if (private && intermed == nullptr &&
	    (command == 's' || command == 'r')) {
		for (int i = 0; i < argcount; i++) {
			int index = stored_mode_index(CSI_ARG(args[i]), true);
			if (index < 0)
				continue;
			if (command == 's')
				term->saved_stored_modes[index] =
				    term->stored_modes[index];
			else if (term->saved_stored_modes[index] != 0)
				term->stored_modes[index] =
				    term->saved_stored_modes[index];
		}
		return 1;
	}

	/* Set/reset of stored modes (unknown to libvterm; patches 1, 4). */
	if ((private || no_leader) && intermed == nullptr &&
	    (command == 'h' || command == 'l')) {
		int index = stored_mode_index(first, private);
		if (index >= 0) {
			term->stored_modes[index] = command == 'h' ? 1 : 2;
			return 1;
		}
	}

	/* DECRQM: "CSI [?] Ps $ p" -> "CSI [?] Ps ; Pm $ y". */
	if (intermed != nullptr && intermed[0] == '$' && command == 'p' &&
	    first != 2048) {
		if (term->conformance < 63)
			return 1;	/* VT100/VT220: no DECRQM */
		unsigned value = 0;	/* not recognised */
		int index = stored_mode_index(first, private);
		if (index >= 0)
			value = term->stored_modes[index];
		else if (!private && first == 4)
			value = term->state->mode.insert ? 1 : 2;	/* IRM */
		else if (!private && first == 20)
			value = term->state->mode.newline ? 1 : 2;	/* LNM */
		else if (!private && ansi_mode_permanently_reset(first))
			value = 4;
		else if (private && (first == 60 || first == 8 || first == 61 ||
		    first == 64 || first == 68 || first == 73 || first == 81))
			value = 4;	/* DECHCCM, and (as xterm) DECARM, DECVCCM,
					   DECPCCM, DECKBUM, DECXRLM, DECKPM:
					   permanently reset */
		send_printf(term, private ? "\x1b[?%u;%u$y" : "\x1b[%u;%u$y",
		    (unsigned)first, value, 0, 0);
		return 1;
	}

	/* DECDSR reports libvterm doesn't answer (patch 4). */
	if (private && intermed == nullptr && command == 'n') {
		static const struct {
			long		 request;
			const char	*reply;
		} reports[] = {
			{25, "\x1b[?20n"},		/* UDKs unlocked */
			{26, "\x1b[?27;1;0;0n"},	/* keyboard: US, ready */
			{53, "\x1b[?50n"},		/* locator: none */
			{55, "\x1b[?50n"},
			{56, "\x1b[?57;0n"},		/* locator type: none */
			{62, "\x1b[0*{"},		/* macro space: 0 */
			{75, "\x1b[?70n"},		/* data integrity: ok */
			{85, "\x1b[?83n"},		/* multi-session: none */
		};
		if (first == 15) {	/* printer: ready / not ready / none */
			enum term_printer_status status = term->has_printer ?
			    term->printer.status(term->printer.ctx) :
			    TERM_PRINTER_NONE;
			send_printf(term, "\x1b[?%un", status ==
			    TERM_PRINTER_READY ? 10u : status ==
			    TERM_PRINTER_NOT_READY ? 11u : 13u, 0, 0, 0);
			return 1;
		}
		for (size_t i = 0; i < sizeof(reports) / sizeof(reports[0]);
		    i++) {
			if (reports[i].request == first) {
				term->output(reports[i].reply,
				    strlen(reports[i].reply), term->output_ctx);
				return 1;
			}
		}
		if (first == 63) {	/* DECCKSR: memory checksum, none */
			long id = argcount > 1 ? CSI_ARG(args[1]) : 0;
			send_printf(term, "\x1bP%u!~0000\x1b\\",
			    id == CSI_ARG_MISSING ? 0u : (unsigned)id, 0, 0, 0);
			return 1;
		}
		return 0;
	}

	if (private && first == 2048) {
		if (intermed == nullptr && (command == 'h' || command == 'l')) {
			term->resize_reports = command == 'h';
			if (term->resize_reports)
				send_size_report(term);
			return 1;
		}
		if (intermed != nullptr && intermed[0] == '$' && command == 'p') {
			send_printf(term, "\x1b[?2048;%u$y",
			    term->resize_reports ? 1 : 2, 0, 0, 0);
			return 1;
		}
	}
	return 0;
}

/*
 * XTGETTCAP, "DCS + q <hex name> [; <hex name>...] ST": report termcap/
 * terminfo capabilities. Reply per name "DCS 1 + r <hex name> = <hex
 * value> ST", or "DCS 0 + r <hex name> ST" if unknown. Programs (and
 * esctest) use "Co" to learn the palette size.
 */
static void
xtgettcap_reply(struct term *term, const char *hex_name, size_t hex_len)
{
	char name[32];
	size_t name_len = 0;
	char value[32];
	const struct term_emulation_info *info =
	    &term_emulations[term->options.emulation % EMU_COUNT];

	for (size_t i = 0; i + 1 < hex_len && name_len < sizeof(name) - 1; i += 2) {
		int high = hex_value(hex_name[i]), low = hex_value(hex_name[i + 1]);
		if (high < 0 || low < 0)
			break;
		name[name_len++] = (char)(high << 4 | low);
	}
	name[name_len] = '\0';

	value[0] = '\0';
	unsigned depth = term->options.colour_depth;
	if (strcmp(name, "Co") == 0 || strcmp(name, "colors") == 0)
		snprintf(value, sizeof(value), "%u", depth == DEPTH_MONO ? 8u :
		    depth == DEPTH_16 ? 16u : 256u);
	else if (strcmp(name, "TN") == 0 || strcmp(name, "name") == 0)
		snprintf(value, sizeof(value), "%s", info->term_hint);
	else if (strcmp(name, "RGB") == 0 && depth == DEPTH_TRUE)
		snprintf(value, sizeof(value), "8/8/8");

	char reply[128];
	int len;
	if (value[0] == '\0') {
		len = snprintf(reply, sizeof(reply), "\x1bP0+r%.*s\x1b\\",
		    (int)hex_len, hex_name);
	} else {
		len = snprintf(reply, sizeof(reply), "\x1bP1+r%.*s=",
		    (int)hex_len, hex_name);
		for (const char *c = value; *c && len < (int)sizeof(reply) - 4;
		    c++)
			len += snprintf(reply + len, sizeof(reply) - (size_t)len,
			    "%02X", (unsigned char)*c);
		len += snprintf(reply + len, sizeof(reply) - (size_t)len,
		    "\x1b\\");
	}
	term->output(reply, (size_t)len, term->output_ctx);
}

/* DECRQSS for settings libvterm doesn't track (libvterm patch 8). */
static int
decrqss_reply(struct term *term, const char *setting, size_t len)
{
	char value[24];

	if (len == 2 && memcmp(setting, "\"p", 2) == 0)	/* DECSCL */
		snprintf(value, sizeof(value), "%u;1\"p", term->conformance);
	else if (len == 1 && setting[0] == 't')		/* DECSLPP */
		snprintf(value, sizeof(value), "%ut", term->screen->rows);
	else if (len == 2 && memcmp(setting, "*|", 2) == 0)	/* DECSNLS */
		snprintf(value, sizeof(value), "%u*|", term->screen->rows);
	else if (len == 2 && memcmp(setting, "*x", 2) == 0)	/* DECSACE */
		snprintf(value, sizeof(value), "%u*x", term->decsace);
	else if (len == 2 && memcmp(setting, "$}", 2) == 0)	/* DECSASD */
		snprintf(value, sizeof(value), "0$}");
	else if (len == 2 && memcmp(setting, "$~", 2) == 0)	/* DECSSDT */
		snprintf(value, sizeof(value), "0$~");
	else
		return 0;
	term->output("\x1bP1$r", 5, term->output_ctx);
	term->output(value, strlen(value), term->output_ctx);
	term->output("\x1b\\", 2, term->output_ctx);
	return 1;
}

static int
fb_dcs(const char *command, size_t commandlen, VTermStringFragment frag,
    void *user)
{
	struct term *term = user;

	if (commandlen == 2 && command[0] == '$' && command[1] == 'q')
		return decrqss_reply(term, frag.str, frag.len);
	/* Sixel: "DCS [P1 ; P2 ; P3] q" -- digits and ';' then 'q'. */
	size_t digits = 0;
	while (digits < commandlen && ((command[digits] >= '0' &&
	    command[digits] <= '9') || command[digits] == ';'))
		digits++;
	if (commandlen >= 1 && digits == commandlen - 1 &&
	    command[digits] == 'q') {
		if (term->options.sixel)
			graphics_sixel(term, command, commandlen - 1,
			    frag.str, frag.len, frag.initial, frag.final);
		return 1;
	}
	if (commandlen != 2 || command[0] != '+' || command[1] != 'q')
		return 0;
	if (frag.initial) {
		term->osc_len = 0;
		term->osc_overflow = false;
	}
	if (term->osc_len + frag.len >= sizeof(term->osc_buf))
		term->osc_overflow = true;
	else {
		memcpy(term->osc_buf + term->osc_len, frag.str, frag.len);
		term->osc_len += frag.len;
	}
	if (!frag.final || term->osc_overflow)
		return 1;

	size_t at = 0;
	const char *field;
	size_t field_len;
	while (next_field(term->osc_buf, term->osc_len, &at, &field,
	    &field_len))
		if (field_len > 0)
			xtgettcap_reply(term, field, field_len);
	return 1;
}

static const VTermStateFallbacks state_fallbacks = {
	.dcs = fb_dcs,
	.control = fb_control,
	.csi = fb_csi,
	.osc = fb_osc,
};

/*
 * Apply the mode-type options by feeding the terminal the escape
 * sequences a host would send: DECAWM (?7), LNM (20) and DECSCUSR. This
 * keeps libvterm the single owner of terminal state.
 */
static void
apply_modes(struct term *term)
{
	/* The emulation level's conformance: VT100 61 ... xterm 64 (VT420). */
	set_conformance(term, (uint8_t)(61 + term->options.emulation % EMU_COUNT));
	static const char shape_codes[] = {'1', '3', '5'};  /* blinking */
	const struct term_options *options = &term->options;
	char sequence[32];
	char shape = shape_codes[options->cursor_shape % 3];

	if (!options->cursor_blink)
		shape++;		/* steady variant is the next code */
	int len = snprintf(sequence, sizeof(sequence),
	    "\x1b[?7%c\x1b[20%c\x1b[%c q", options->autowrap ? 'h' : 'l',
	    options->newline_mode ? 'h' : 'l', shape);
	vterm_input_write(term->vt, sequence, (size_t)len);
}

static void
vterm_output(const char *bytes, size_t len, void *user)
{
	struct term *term = user;

	term->output(bytes, len, term->output_ctx);
}

/* --- public API ------------------------------------------------------ */

/* (Re)allocate both screens for the current font. */
static bool
build_screens(struct term *term, unsigned *cols, unsigned *rows)
{
	configured_size(term, cols, rows);
	term->space_glyph = font_glyph_for(&term->font, ' ');
	struct cell blank = grid_make_cell(term->space_glyph, term->default_fg,
	    term->default_bg, CELL_ERASED);
	return grid_init(&term->primary, *cols, *rows, blank) &&
	    grid_init(&term->alternate, *cols, *rows, blank);
}

bool
term_init(struct term *term, const struct font_builtin *builtin,
    struct render *render, term_output_fn output, void *output_ctx)
{
	unsigned cols, rows;
	VTermColor fg, bg;

	memset(term, 0, sizeof(*term));
	term->render = render;
	term->output = output;
	term->output_ctx = output_ctx;
	term->options = (struct term_options){
		.fg_rgb = {DEFAULT_FG_RGB},
		.bg_rgb = {DEFAULT_BG_RGB},
		.cursor_shape = CURSOR_BLOCK,
		.cursor_blink = true,
		.autowrap = true,
		.emulation = EMU_XTERM,
		.colour_depth = DEPTH_TRUE,
		.utf8 = true,
		.alt_screen = true,
		.sixel = true,
		.sixel_bpp = 8,
	};
	term->default_fg = grid_rgb565(DEFAULT_FG_RGB);
	term->default_bg = grid_rgb565(DEFAULT_BG_RGB);
	term->cursor_visible = true;
	term->cursor_blink = true;
	term->cursor_shape = CURSOR_BLOCK;

	if (font_load_psf2(&term->font, builtin->data, builtin->len) != FONT_OK)
		return false;
	term->font_source = builtin;
	if (!build_screens(term, &cols, &rows))
		goto fail;

	term->vt = vterm_new((int)rows, (int)cols);
	if (term->vt == nullptr)
		goto fail;
	vterm_set_utf8(term->vt, 1);
	vterm_output_set_callback(term->vt, vterm_output, term);

	/*
	 * Bind the screen and renderer before installing callbacks: the
	 * hard reset below immediately calls initpen/erase/settermprop.
	 */
	term->screen = &term->primary;
	render_init(render, &term->font, term->screen);

	term->state = vterm_obtain_state(term->vt);
	vterm_color_rgb(&fg, DEFAULT_FG_RGB);
	vterm_color_rgb(&bg, DEFAULT_BG_RGB);
	vterm_state_set_default_colors(term->state, &fg, &bg);
	vterm_state_set_bold_highbright(term->state, 1);
	cache_palette16(term);
	memcpy(term->ansi_default, term->palette16, sizeof(term->ansi_default));
	init_colour_state(term);
	init_stored_modes(term);
	set_conformance(term, (uint8_t)(61 + term->options.emulation %
	    EMU_COUNT));
	vterm_state_set_callbacks(term->state, &state_callbacks, term);
	vterm_state_set_unrecognised_fallbacks(term->state, &state_fallbacks,
	    term);
	vterm_state_reset(term->state, 1);
	sync_presentation(term);
	return true;

fail:
	term_free(term);
	return false;
}

bool
term_set_font(struct term *term, const struct font_builtin *builtin)
{
	struct font next;
	unsigned cols, rows;

	/* Load first: if this fails, nothing has changed. */
	if (font_load_psf2(&next, builtin->data, builtin->len) != FONT_OK)
		return false;

	render_pause(term->render);
	graphics_release(term);	/* re-taken below, sized to what's left */
	grid_free(&term->primary);
	grid_free(&term->alternate);
	font_free(&term->font);
	term->font = next;
	term->font_source = builtin;
	if (!build_screens(term, &cols, &rows))
		return false;

	term->screen = &term->primary;
	term->stored_modes[stored_mode_index(3, true)] = 2;	/* DECCOLM */
	render_init(term->render, &term->font, term->screen);
	graphics_acquire(term);	/* tiles are font-cell sized */
	/*
	 * Resize libvterm's state (margins, tab stops, line info) and then
	 * hard-reset it, which homes the cursor, clears both screens through
	 * our erase callback and restores default modes via settermprop.
	 */
	vterm_set_size(term->vt, (int)rows, (int)cols);
	vterm_state_reset(term->state, 1);
	apply_modes(term);
	sync_presentation(term);
	/* The size changed under the host: tell it, if it asked. */
	if (term->resize_reports)
		send_size_report(term);
	return true;
}

bool
term_configure(struct term *term, const struct font_builtin *builtin,
    const struct term_options *options)
{
	bool rebuild = builtin != term->font_source ||
	    options->max_rows != term->options.max_rows ||
	    options->max_cols != term->options.max_cols ||
	    options->emulation != term->options.emulation ||
	    options->colour_depth != term->options.colour_depth ||
	    memcmp(options->fg_rgb, term->options.fg_rgb, 3) != 0 ||
	    memcmp(options->bg_rgb, term->options.bg_rgb, 3) != 0 ||
	    options->ansi_set != term->options.ansi_set ||
	    options->sixel_bpp != term->options.sixel_bpp ||
	    (options->ansi_set && memcmp(options->ansi, term->options.ansi,
	    sizeof(options->ansi)) != 0);

	term->options = *options;
	term->options.answerback[sizeof(term->options.answerback) - 1] = '\0';
	/* Leaving the alternate screen allowed -> disallowed: go home. */
	if (!options->alt_screen && term->screen == &term->alternate) {
		term->screen = &term->primary;
		render_set_grid(term->render, term->screen);
		sync_presentation(term);
	}
	if (!rebuild) {
		apply_modes(term);
		return true;
	}

	VTermColor fg, bg;
	term->default_fg = grid_rgb565(options->fg_rgb[0], options->fg_rgb[1],
	    options->fg_rgb[2]);
	term->default_bg = grid_rgb565(options->bg_rgb[0], options->bg_rgb[1],
	    options->bg_rgb[2]);
	vterm_color_rgb(&fg, options->fg_rgb[0], options->fg_rgb[1],
	    options->fg_rgb[2]);
	vterm_color_rgb(&bg, options->bg_rgb[0], options->bg_rgb[1],
	    options->bg_rgb[2]);
	vterm_state_set_default_colors(term->state, &fg, &bg);
	/* ANSI 0-15: the scheme's, or back to libvterm's (xterm's) own. */
	for (int index = 0; index < 16; index++) {
		const uint8_t *rgb = options->ansi_set ? options->ansi[index] :
		    term->ansi_default[index];
		VTermColor colour;
		vterm_color_rgb(&colour, rgb[0], rgb[1], rgb[2]);
		vterm_state_set_palette_color(term->state, index, &colour);
	}
	cache_palette16(term);
	init_colour_state(term);	/* OSC 104 etc. reset to these */
	for (unsigned which = 0; which < 10; which++)
		reset_dynamic_colour(term, which);
	return term_set_font(term, builtin);
}

void
term_set_keepalive(struct term *term, void (*keepalive)(void))
{
	term->graphics.keepalive = keepalive;
}

void
term_set_graphics_budget(struct term *term, size_t budget,
    graphics_heap_fn heap_free)
{
	graphics_configure(term, budget, heap_free);
}

void
term_reset(struct term *term)
{
	vterm_state_reset(term->state, 1);
	apply_modes(term);
	sync_presentation(term);
}

void
term_free(struct term *term)
{
	graphics_release(term);
	if (term->vt != nullptr)
		vterm_free(term->vt);
	grid_free(&term->primary);
	grid_free(&term->alternate);
	font_free(&term->font);
	memset(term, 0, sizeof(*term));
}

/*
 * 8-bit mode (UTF-8 off), done in front of libvterm, which stays in UTF-8
 * mode and only knows 94-character sets: C1 controls 0x80-0x9F become
 * their 7-bit forms (ESC + byte-0x40: 0x9B CSI -> ESC [, 0x9C ST ->
 * ESC \), and 0xA0-0xFF are taken as ISO Latin-1 and re-encoded as UTF-8.
 * Byte-by-byte and stateless, so chunk boundaries don't matter. (A real
 * VT220's default GR set is DEC Supplemental Graphics, which differs from
 * Latin-1 in a few positions; see docs/terminal.md.)
 */
static void
feed_8bit(struct term *term, const uint8_t *bytes, size_t len)
{
	char out[128];
	size_t used = 0;

	for (size_t i = 0; i < len; i++) {
		uint8_t byte = bytes[i];
		if (used > sizeof(out) - 2) {
			vterm_input_write(term->vt, out, used);
			used = 0;
		}
		if (byte < 0x80) {
			out[used++] = (char)byte;
		} else if (byte < 0xa0) {
			out[used++] = 0x1b;
			out[used++] = (char)(byte - 0x40);
		} else {
			out[used++] = (char)(0xc0 | byte >> 6);
			out[used++] = (char)(0x80 | (byte & 0x3f));
		}
	}
	if (used > 0)
		vterm_input_write(term->vt, out, used);
}

/* Into libvterm (via the 8-bit translation if UTF-8 is off). */
void
term_feed_through(struct term *term, const uint8_t *bytes, size_t len)
{
	if (term->options.utf8)
		vterm_input_write(term->vt, (const char *)bytes, len);
	else
		feed_8bit(term, bytes, len);
}

void
term_feed(struct term *term, const uint8_t *bytes, size_t len)
{
	/* With a printer, Media Copy needs a look at the bytes first. */
	if (term->has_printer)
		mc_feed(term, bytes, len);
	else
		term_feed_through(term, bytes, len);

	/*
	 * Font switches requested by OSC 50 are applied here, outside
	 * libvterm's callbacks, since they resize and reset its state. Bytes
	 * after the OSC in the same chunk were drawn with the old font and
	 * are wiped by the reset; hosts should wait for the switch (as
	 * tools/setfont does by querying the size).
	 */
	if (term->pending_font != nullptr) {
		const struct font_builtin *next = term->pending_font;
		term->pending_font = nullptr;
		(void)term_set_font(term, next);
	}
}

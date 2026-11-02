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

#include <stdio.h>
#include <string.h>

/* xterm's default colours: light grey on black. */
#define DEFAULT_FG_RGB	229, 229, 229
#define DEFAULT_BG_RGB	0, 0, 0

static uint16_t
resolve_colour(const struct term *term, VTermColor colour, bool foreground)
{
	if (foreground && VTERM_COLOR_IS_DEFAULT_FG(&colour))
		return term->default_fg;
	if (!foreground && VTERM_COLOR_IS_DEFAULT_BG(&colour))
		return term->default_bg;
	vterm_state_convert_color_to_rgb(term->state, &colour);
	return grid_rgb565(colour.rgb.red, colour.rgb.green, colour.rgb.blue);
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

	return grid_make_cell(term->space_glyph, term->default_fg, bg, 0);
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

	/* DECSCA protection isn't tracked, so selective erase (DECSED/
	 * DECSEL) erases everything. Rarely used; see docs/terminal.md. */
	(void)selective;
	if (!clamp_rect(term->screen, &rect))
		return 1;
	for (int row = rect.start_row; row < rect.end_row; row++)
		grid_clear_span(term->screen, (unsigned)row,
		    (unsigned)rect.start_col, (unsigned)rect.end_col, blank);
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

static int
cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
	struct term *term = user;

	switch (prop) {
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
		struct grid *next = val->boolean ? &term->alternate :
		    &term->primary;
		if (next == term->screen)
			break;
		/* Entering the alternate screen starts it blank (xterm
		 * 1049 behaviour; libvterm leaves clearing to the screen
		 * layer, which we don't use). */
		if (next == &term->alternate)
			for (unsigned row = 0; row < next->rows; row++)
				grid_clear_span(next, row, 0, next->cols,
				    erase_cell(term));
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
};

/*
 * OSC 50 (xterm "set font"), via libvterm's fallback for OSC numbers it
 * doesn't handle. The string can arrive in several fragments.
 */
static int
fb_osc(int command, VTermStringFragment frag, void *user)
{
	struct term *term = user;

	if (command != 50)
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

	const char *arg = term->osc_buf;
	unsigned index = (unsigned)(term->font_source - font_builtins);
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
	return 1;
}

/* ENQ (0x05): reply with the answerback message, as VT terminals do. */
static int
fb_control(unsigned char control, void *user)
{
	struct term *term = user;

	if (control != 0x05)
		return 0;
	term->output(term->options.answerback,
	    strnlen(term->options.answerback, sizeof(term->options.answerback)),
	    term->output_ctx);
	return 1;
}

static const VTermStateFallbacks state_fallbacks = {
	.control = fb_control,
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
	render_grid_size(&term->font, term->render->width,
	    term->render->height, cols, rows);
	if (term->options.max_rows != 0 && term->options.max_rows < *rows)
		*rows = term->options.max_rows;
	term->space_glyph = font_glyph_for(&term->font, ' ');
	struct cell blank = grid_make_cell(term->space_glyph, term->default_fg,
	    term->default_bg, 0);
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
	grid_free(&term->primary);
	grid_free(&term->alternate);
	font_free(&term->font);
	term->font = next;
	term->font_source = builtin;
	if (!build_screens(term, &cols, &rows))
		return false;

	term->screen = &term->primary;
	render_init(term->render, &term->font, term->screen);
	/*
	 * Resize libvterm's state (margins, tab stops, line info) and then
	 * hard-reset it, which homes the cursor, clears both screens through
	 * our erase callback and restores default modes via settermprop.
	 */
	vterm_set_size(term->vt, (int)rows, (int)cols);
	vterm_state_reset(term->state, 1);
	apply_modes(term);
	sync_presentation(term);
	return true;
}

bool
term_configure(struct term *term, const struct font_builtin *builtin,
    const struct term_options *options)
{
	bool rebuild = builtin != term->font_source ||
	    options->max_rows != term->options.max_rows ||
	    memcmp(options->fg_rgb, term->options.fg_rgb, 3) != 0 ||
	    memcmp(options->bg_rgb, term->options.bg_rgb, 3) != 0;

	term->options = *options;
	term->options.answerback[sizeof(term->options.answerback) - 1] = '\0';
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
	return term_set_font(term, builtin);
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
	if (term->vt != nullptr)
		vterm_free(term->vt);
	grid_free(&term->primary);
	grid_free(&term->alternate);
	font_free(&term->font);
	memset(term, 0, sizeof(*term));
}

void
term_feed(struct term *term, const uint8_t *bytes, size_t len)
{
	vterm_input_write(term->vt, (const char *)bytes, len);

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

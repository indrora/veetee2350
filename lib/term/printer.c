/*
 * printer.c — Media Copy: the VT printer port.
 *
 * DEC terminals had a serial port for a printer and a set of "MC"
 * controls (CSI ... i) to drive it. What we implement:
 *
 *   CSI i, CSI 0 i     print screen: the scrolling region, or the whole
 *                      screen with DECPEX (mode 19); a form feed after
 *                      it with DECPFF (mode 18)
 *   CSI ? 1 i          print the cursor line
 *   CSI ? 5 i / ? 4 i  auto print on/off: each line is printed as the
 *                      cursor leaves it by LF, VT or FF
 *   CSI 5 i / 4 i      printer controller mode on/off: everything in
 *                      between goes to the printer, not the screen
 *   CSI ? 15 n         printer status (term.c answers via status())
 *
 * Controller mode can't be left to libvterm. The bytes after "CSI 5 i"
 * must bypass the screen, and libvterm consumes the whole buffer it's
 * given before any callback could stop it. So term_feed() runs incoming
 * bytes through mc_feed() first, which recognises "ESC [ 5 i" and
 * "ESC [ 4 i" (and the 8-bit CSI forms in 8-bit mode) itself, across
 * any chunk boundary: up to three bytes of a possible match are held
 * back until it's decided. The same pass splits the stream at LF/VT/FF
 * for auto print.
 */
#include "term.h"
#include "vterm_internal.h"

#include <string.h>

bool	term_dec_mode(const struct term *term, unsigned mode);
void	term_feed_through(struct term *term, const uint8_t *bytes,
	    size_t len);

#define ESC	0x1b
#define CSI8	0x9b

/* --- text out ---------------------------------------------------------- */

static size_t
put_utf8(uint8_t *out, uint32_t cp)
{
	if (cp < 0x80) {
		out[0] = (uint8_t)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (uint8_t)(0xc0 | cp >> 6);
		out[1] = (uint8_t)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (uint8_t)(0xe0 | cp >> 12);
		out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (uint8_t)(0x80 | (cp & 0x3f));
		return 3;
	}
	out[0] = (uint8_t)(0xf0 | cp >> 18);
	out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3f));
	out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3f));
	out[3] = (uint8_t)(0x80 | (cp & 0x3f));
	return 4;
}

/* One screen row as text: erased and picture cells are blanks. */
static void
print_row(struct term *term, unsigned row)
{
	uint8_t line[GRID_MAX_COLS * 4 + 2];
	const struct cell *cells = grid_row(term->screen, row);
	size_t len = 0, kept = 0;

	for (unsigned col = 0; col < term->screen->cols; col++) {
		uint32_t cp = ' ';
		if (!(cells[col].attr & (CELL_ERASED | CELL_GRAPHIC))) {
			cp = font_codepoint_for(&term->font, cells[col].glyph);
			if (cp == 0)
				cp = ' ';
		}
		len += put_utf8(line + len, cp);
		if (cp != ' ')
			kept = len;	/* trailing blanks are dropped */
	}
	line[kept++] = '\r';
	line[kept++] = '\n';
	(void)term->printer.write(line, kept, term->printer.ctx);
}

static void
job_done(struct term *term)
{
	if (term->printer.job_done != nullptr)
		term->printer.job_done(term->printer.ctx);
}

/* CSI i: the scrolling region (whole screen with DECPEX). */
void
printer_print_screen(struct term *term)
{
	VTermState *state = term->state;
	unsigned top = 0, bottom = term->screen->rows;

	if (!term->has_printer)
		return;
	if (!term_dec_mode(term, 19)) {		/* DECPEX reset: region */
		top = (unsigned)state->scrollregion_top;
		bottom = (unsigned)SCROLLREGION_BOTTOM(state);
	}
	for (unsigned row = top; row < bottom; row++)
		print_row(term, row);
	if (term_dec_mode(term, 18))		/* DECPFF */
		(void)term->printer.write((const uint8_t *)"\f", 1,
		    term->printer.ctx);
	job_done(term);
}

/* CSI ? 1 i: the line the cursor is on. */
void
printer_print_line(struct term *term)
{
	if (!term->has_printer)
		return;
	print_row(term, (unsigned)term->state->pos.row);
	job_done(term);
}

/* --- the input pass ---------------------------------------------------- */

/* Bytes collected for one destination before handing them over. */
struct span {
	uint8_t	bytes[128];
	size_t	len;
};

static void
span_flush(struct term *term, struct span *span)
{
	if (span->len == 0)
		return;
	if (term->print_controller)
		(void)term->printer.write(span->bytes, span->len,
		    term->printer.ctx);
	else
		term_feed_through(term, span->bytes, span->len);
	span->len = 0;
}

static void
span_add(struct term *term, struct span *span, uint8_t byte)
{
	if (span->len == sizeof(span->bytes))
		span_flush(term, span);
	span->bytes[span->len++] = byte;
}

/*
 * The held bytes turned out not to be an MC sequence: they belong to
 * the current destination after all.
 */
static void
release_held(struct term *term, struct span *span)
{
	for (unsigned i = 0; i < term->mc_held_len; i++)
		span_add(term, span, term->mc_held[i]);
	term->mc_held_len = 0;
}

/*
 * Advance the "CSI <digit> i" matcher by one byte. Returns true if the
 * byte was taken (held, or completed the sequence).
 */
static bool
match_mc(struct term *term, struct span *span, uint8_t byte)
{
	uint8_t want_digit = term->print_controller ? '4' : '5';
	uint8_t *held = term->mc_held;
	unsigned len = term->mc_held_len;
	bool eight_bit = !term->options.utf8;

	if (len == 0) {
		if (byte == ESC || (byte == CSI8 && eight_bit)) {
			held[term->mc_held_len++] = byte;
			return true;
		}
		return false;
	}
	/* Position of the digit: after "ESC [" or after the 8-bit CSI. */
	unsigned digit_at = held[0] == ESC ? 2 : 1;
	bool ok;
	if (len < digit_at)
		ok = byte == '[';
	else if (len == digit_at)
		ok = byte == want_digit;
	else
		ok = byte == 'i';
	if (!ok) {
		release_held(term, span);
		return match_mc(term, span, byte);	/* may start anew */
	}
	if (len < digit_at + 1u) {
		held[term->mc_held_len++] = byte;
		return true;
	}
	/* Complete: switch destination; the sequence itself goes nowhere. */
	term->mc_held_len = 0;
	span_flush(term, span);
	term->print_controller = !term->print_controller;
	if (!term->print_controller)
		job_done(term);
	return true;
}

void
mc_feed(struct term *term, const uint8_t *bytes, size_t len)
{
	struct span span = {.len = 0};

	for (size_t i = 0; i < len; i++) {
		uint8_t byte = bytes[i];

		if (match_mc(term, &span, byte))
			continue;
		/*
		 * Auto print: the line is done when LF, VT or FF leaves it.
		 * Flush first either way -- the bytes before this one may be
		 * the very "CSI ? 5 i" that turns auto print on.
		 */
		if (!term->print_controller &&
		    (byte == '\n' || byte == '\v' || byte == '\f')) {
			span_flush(term, &span);
			if (term->auto_print)
				print_row(term,
				    (unsigned)term->state->pos.row);
		}
		span_add(term, &span, byte);
	}
	span_flush(term, &span);
}

void
term_set_printer(struct term *term, const struct term_printer *printer)
{
	if (term->print_controller)
		job_done(term);
	term->has_printer = printer != nullptr;
	if (printer != nullptr)
		term->printer = *printer;
	term->print_controller = false;
	term->auto_print = false;
	term->mc_held_len = 0;
}

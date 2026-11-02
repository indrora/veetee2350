/*
 * term.h — the terminal emulator: host bytes in, cell grid out.
 *
 * Layering: pure module above render/ and font/ (STYLE.md §4).
 *
 * Escape-sequence parsing and terminal state (cursor, modes, margins,
 * charsets, SGR pen, tab stops, DA/DSR replies...) are libvterm's state
 * layer. This module is the glue: libvterm's state callbacks become writes
 * into our render-ready grid (lib/render/grid.h), and its output callback
 * (device replies) goes back to the host through `output`.
 *
 * libvterm's own screen layer is deliberately NOT used: it keeps ~36 bytes
 * per cell for two screens, where our grid needs 12 (docs/spike-libghostty-
 * vt.md has the memory budget). The cost is that this file implements
 * scrolling, rectangle moves and erases itself.
 *
 * The primary and alternate screens are two grids; switching (DECSET
 * 47/1047/1049) just points the renderer at the other one.
 *
 * Fonts: the terminal owns its font. xterm's OSC 50 changes it:
 *   ESC ] 50 ; <name> BEL   switch to built-in font <name> (fonts.toml)
 *   ESC ] 50 ; #+ BEL       next built-in font (#- previous)
 *   ESC ] 50 ; ? BEL        reply ESC ] 50 ; <current name> BEL
 * A switch changes the grid size, so it rebuilds both screens and does a
 * full terminal reset (RIS-equivalent); the host is expected to re-read
 * the size (tools/setfont does it the resize(1) way) and redraw.
 *
 * Size: the host can ask (xterm window ops CSI 14/16/18/19 t), or enable
 * in-band resize notifications (DECSET 2048, as in contour/kitty/foot):
 * the terminal then sends CSI 48 ; rows ; cols ; height ; width t at once
 * and after every change (font, size setting, resolution).
 *
 * Runs on core 0. Not reentrant; one struct term per screen.
 */
#ifndef TERM_H
#define TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "font.h"
#include "grid.h"
#include "render.h"
#include "vterm.h"

/* Sink for bytes the terminal sends to the host (DA/DSR replies, ...). */
typedef void (*term_output_fn)(const char *bytes, size_t len, void *ctx);

/*
 * Emulation level: what the terminal claims to be (DA1/DA2 replies) and
 * the default feature set. SET-UP picks a level, then lets each feature be
 * overridden. term_emulation_preset() gives a level's defaults.
 */
enum term_emulation {
	EMU_VT100,
	EMU_VT220,
	EMU_VT340,
	EMU_XTERM,
	EMU_COUNT
};

enum colour_depth {
	DEPTH_MONO,		/* SGR colours ignored (VT100/VT220) */
	DEPTH_16,		/* ANSI 16 */
	DEPTH_256,		/* xterm 256 */
	DEPTH_TRUE,		/* 24-bit (shown as RGB565) */
	DEPTH_COUNT
};

struct term_emulation_info {
	const char	*name;
	const char	*term_hint;	/* TERM to use on the host */
	const char	*da1;		/* reply to CSI c, without CSI */
	const char	*da2;		/* reply to CSI > c, or nullptr */
	uint8_t		 depth;		/* defaults: */
	bool		 utf8;
	bool		 alt_screen;
};

extern const struct term_emulation_info term_emulations[EMU_COUNT];

/*
 * User-configurable behaviour (from SET-UP). Geometry and colours need a
 * screen rebuild + reset to apply; the rest are terminal modes applied in
 * place.
 */
struct term_options {
	uint8_t		fg_rgb[3];	/* default foreground */
	uint8_t		bg_rgb[3];	/* default background */
	unsigned	max_rows;	/* 0 = as many as the font fits */
	unsigned	max_cols;	/* 0 = as many as the font fits */
	uint8_t		cursor_shape;	/* enum cursor_shape */
	bool		cursor_blink;
	bool		autowrap;	/* DECAWM */
	bool		newline_mode;	/* LNM */
	char		answerback[32];	/* sent in reply to ENQ */
	uint8_t		emulation;	/* enum term_emulation */
	uint8_t		colour_depth;	/* enum colour_depth */
	bool		utf8;		/* off: 8-bit bytes, C1 + Latin-1 GR */
	bool		alt_screen;	/* off: ignore DECSET 47/1047/1049 */
};

/* Current SGR state, as libvterm reports it through setpenattr. */
struct term_pen {
	VTermColor	fg;
	VTermColor	bg;
	bool		bold;
	bool		underline;
	bool		italic;
	bool		blink;
	bool		reverse;
	bool		conceal;
	bool		strike;
};

struct term {
	VTerm		*vt;
	VTermState	*state;
	struct font	 font;
	const struct font_builtin *font_source;
	const struct font_builtin *pending_font; /* OSC 50, applied after feed */
	struct render	*render;
	struct grid	 primary;
	struct grid	 alternate;
	struct grid	*screen;	/* the grid being written and shown */
	struct term_pen	 pen;
	struct term_options options;
	uint8_t		 palette16[16][3];	/* for DEPTH_16 mapping */
	uint16_t	 default_fg;	/* RGB565 */
	uint16_t	 default_bg;
	uint16_t	 space_glyph;
	term_output_fn	 output;
	void		*output_ctx;
	uint32_t	 bells;		/* BEL count, for a future visual bell */
	uint32_t	 last_char;	/* last graphic character, for REP */

	/* DECSET 2048: report the size in-band whenever it changes. */
	bool		 resize_reports;

	/* Modes accepted and reported (DECRQM) but with no visible effect
	 * here, e.g. DECARM, DECNKM, KAM. Index into term.c's table. */
	uint8_t		 stored_modes[32];
	uint8_t		 decsace;	/* DECSACE, reported by DECRQSS only */

	/* OSC colour state (4/5/10-19 and their resets). */
	uint8_t		 palette_initial[256][3];	/* for OSC 104 */
	uint8_t		 special_colours[5][3];		/* OSC 5: bold..italic */
	uint8_t		 dynamic_colours[10][3];	/* OSC 10..19 */

	/* OSC argument, reassembled from libvterm's string fragments. */
	char		 osc_buf[160];
	size_t		 osc_len;
	bool		 osc_overflow;

	/* Presentation modes kept here so both grids can be updated
	 * when the screen switches. */
	bool		 cursor_visible;
	bool		 cursor_blink;
	uint8_t		 cursor_shape;
	bool		 reverse_video;
};

/*
 * Build a terminal using built-in font `builtin`, sized to fit the screen
 * (see render_grid_size), and point `render` at its primary screen.
 * Returns false if the font is bad or memory runs out.
 */
[[nodiscard]] bool term_init(struct term *term,
	    const struct font_builtin *builtin, struct render *render,
	    term_output_fn output, void *output_ctx);

/*
 * Switch font: rebuild both screens at the new grid size and reset the
 * terminal. On a font load failure nothing changes and false is returned.
 * Allocation failure after the old screens are freed also returns false,
 * leaving the renderer paused (blank screen); callers treat that as fatal.
 */
[[nodiscard]] bool term_set_font(struct term *term,
	    const struct font_builtin *builtin);

/*
 * Apply a font + options together (SET-UP exit/recall). Rebuilds and
 * resets only if the font, row limit or default colours changed;
 * otherwise just applies the modes. Same failure contract as
 * term_set_font.
 */
[[nodiscard]] bool term_configure(struct term *term,
	    const struct font_builtin *builtin,
	    const struct term_options *options);

/* Hard reset (like RIS), then re-apply the configured modes. */
void	term_reset(struct term *term);
void	term_free(struct term *term);

/* Feed bytes received from the host. Any amount, any split. */
void	term_feed(struct term *term, const uint8_t *bytes, size_t len);

/* Send keyboard input towards the host (encoded by the caller). */
static inline void
term_send(struct term *term, const char *bytes, size_t len)
{
	term->output(bytes, len, term->output_ctx);
}

#endif /* TERM_H */

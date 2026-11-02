/*
 * setup.h — the SET-UP screen, VT-terminal style.
 *
 * Layering: pure module above render/ (grid) and font/. It edits a working
 * copy of the settings and draws into a grid of its own, which the app
 * shows in place of the terminal's screen while SET-UP is open, so the
 * terminal session underneath is left exactly as it was.
 *
 * The module never touches hardware or the terminal: setup_key() returns
 * what the user asked for (save, exit, ...) and the app carries it out,
 * then reports back with setup_message().
 */
#ifndef SETUP_H
#define SETUP_H

#include <stdbool.h>

#include "font.h"
#include "grid.h"
#include "keys.h"
#include "settings.h"

enum setup_action {
	SETUP_NONE,		/* key handled inside the menu (or ignored) */
	SETUP_EXIT,		/* leave SET-UP, applying `work` */
	SETUP_SAVE,		/* apply `work` and write it to flash */
	SETUP_RECALL,		/* replace `work` with the saved settings */
	SETUP_DEFAULTS,		/* replace `work` with factory defaults */
	SETUP_RESET_TERMINAL,	/* hard-reset the terminal (RIS) */
};

enum setup_item {
	ITEM_FONT,
	ITEM_ROWS,
	ITEM_SCHEME,
	ITEM_CURSOR,
	ITEM_BLINK,
	ITEM_WRAP,
	ITEM_NEWLINE,
	ITEM_ECHO,
	ITEM_BELL,
	ITEM_ANSWERBACK,
	ITEM_SAVE,
	ITEM_RECALL,
	ITEM_DEFAULTS,
	ITEM_RESET,
	ITEM_EXIT,
	ITEM_COUNT
};

struct setup {
	struct settings	 work;		/* the settings being edited */
	struct grid	*grid;
	const struct font *font;	/* to look up glyphs while drawing */
	unsigned	 selected;	/* enum setup_item */
	bool		 editing;	/* typing into the answerback field */
	char		 message[64];
};

/* Open SET-UP on `grid` (any size >= 64x22) editing a copy of `current`. */
void	setup_begin(struct setup *setup, const struct settings *current,
	    struct grid *grid, const struct font *font);

/* Handle one key; redraws as needed. */
enum setup_action setup_key(struct setup *setup, const struct key_event *key);

/* Show a status line message (e.g. "Saved.") and redraw. */
void	setup_message(struct setup *setup, const char *text);

void	setup_draw(struct setup *setup);

/* Rows a font gives with this rows setting (resolves SETTINGS_ROWS_AUTO). */
unsigned setup_rows_for(const struct font_builtin *builtin, unsigned rows);

#endif /* SETUP_H */

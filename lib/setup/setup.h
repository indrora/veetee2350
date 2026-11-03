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
	/*
	 * Leave with nothing unsaved: `work` is as SET-UP opened, or as
	 * saved with S (applied now, restarting for a saved new mode).
	 */
	SETUP_EXIT,
	SETUP_SAVE_EXIT,	/* write `work` to flash, apply it, leave */
	SETUP_DISCARD,		/* leave as if never here: nothing applied */
	SETUP_SAVE,		/* apply `work` and write it to flash */
	SETUP_RECALL,		/* replace `work` with the saved settings */
	SETUP_DEFAULTS,		/* replace `work` with factory defaults */
	SETUP_RESET_TERMINAL,	/* hard-reset the terminal (RIS) */
};

enum setup_item {
	ITEM_RESOLUTION,
	ITEM_FONT,
	ITEM_SIZE,
	ITEM_EMULATION,
	ITEM_DEPTH,
	ITEM_UTF8,
	ITEM_ALTSCREEN,
	ITEM_SIXEL,
	ITEM_SCHEME,
	ITEM_CURSOR,
	ITEM_BLINK,
	ITEM_WRAP,
	ITEM_NEWLINE,
	ITEM_ECHO,
	ITEM_BELL,
	ITEM_SCREENSHOT,
	ITEM_PRINTER,
	ITEM_CAPTURE,
	ITEM_NEWLINES,
	ITEM_STATUS,
	ITEM_PEEK,
	ITEM_PROFILER,
	ITEM_ANSWERBACK,
	ITEM_HOTKEYS,		/* opens the Hotkeys page */
	ITEM_SAVE,
	ITEM_RECALL,
	ITEM_DEFAULTS,
	ITEM_RESET,
	ITEM_SAVE_EXIT,
	ITEM_EXIT,
	ITEM_COUNT
};

/*
 * What's on the SD card, scanned by the app when SET-UP opens: fonts
 * from /fonts (name without ".psf", with the cell size from its PSF2
 * header) and base16 schemes from /schemes (name without ".yaml"). They
 * follow the built-in fonts and schemes in their SET-UP lists, and are
 * stored in the settings as "sd:name" / the scheme name.
 */
#define SETUP_CARD_NAME_MAX	(SETTINGS_FONT_MAX - 3)	/* minus "sd:" */

struct setup_card_font {
	char		name[SETUP_CARD_NAME_MAX];
	uint8_t		width;
	uint8_t		height;
};

struct setup_catalogue {
	unsigned	font_count;
	const struct setup_card_font *fonts;
	unsigned	scheme_count;
	const char	(*schemes)[SETTINGS_SCHEME_MAX];
};

struct setup {
	struct settings	 work;		/* the settings being edited */
	/* As SET-UP opened, or as last saved: "changed" is against this. */
	struct settings	 baseline;
	bool		 confirming;	/* leaving asked "Save changes?" */
	struct grid	*grid;
	const struct font *font;	/* to look up glyphs while drawing */
	unsigned	 selected;	/* enum setup_item */
	unsigned	 running_resolution;	/* mode the hardware is in now */
	bool		 editing;	/* typing into the answerback field */
	unsigned	 scroll;	/* first item shown (short screens) */
	/* The Hotkeys page: a list of tools, Enter to press a new key. */
	bool		 hotkeys_page;
	unsigned	 hotkey_selected;	/* index into the page's list */
	bool		 capturing;	/* waiting for the key to bind */
	char		 message[64];
	char		 storage[36];	/* SD card status, title row */
	struct setup_catalogue card;	/* empty without a card */
};

/*
 * Open SET-UP on `grid` (any size >= 64x22) editing a copy of `current`.
 * `running_resolution` is the video mode actually in use, so the menu can
 * flag a pending change (which needs a restart).
 */
void	setup_begin(struct setup *setup, const struct settings *current,
	    struct grid *grid, const struct font *font,
	    unsigned running_resolution);

/*
 * Handle one key; redraws as needed. F4 saves and leaves; F3 and Esc
 * leave via setup_exit().
 */
enum setup_action setup_key(struct setup *setup, const struct key_event *key);

/*
 * Leave (F3, Esc, Exit, or a BOOTSEL tap). Nothing changed: SETUP_EXIT.
 * Changed (setup_dirty): ask "Save changes?" (and "...and restart?" if
 * setup_dirty_reset) and return SETUP_NONE; the answer is the next key:
 * Y or F4 is SETUP_SAVE_EXIT, N is SETUP_DISCARD, anything else goes
 * back to the menu.
 */
enum setup_action setup_exit(struct setup *setup);

/*
 * Has anything changed since SET-UP opened, or since the last save? The
 * user's two flags, worked out rather than kept, so changing a setting
 * back clears them: "dirty" for any change, "dirty reset" for a new
 * resolution (saving it means a restart).
 */
bool	setup_dirty(const struct setup *setup);
bool	setup_dirty_reset(const struct setup *setup);

/* The app wrote `work` to flash: changes are counted from here now. */
void	setup_saved(struct setup *setup);

/* Offer the card's fonts and schemes (the arrays must outlive SET-UP). */
void	setup_catalogue(struct setup *setup,
	    const struct setup_catalogue *card);
/* Set the SD card status shown in the title row ("SD: 7.4 GB FAT32"). */
void	setup_storage(struct setup *setup, const char *text);
/* Show a status line message (e.g. "Saved.") and redraw. */
void	setup_message(struct setup *setup, const char *text);

void	setup_draw(struct setup *setup);

/*
 * The grid a font gives on a width x height screen with terminal size
 * setting `size` (enum terminal_size): full, or the fixed size clamped to
 * what fits. Returns true if the fixed size had to be clamped.
 */
bool	setup_grid_for(const struct font_builtin *builtin, unsigned size,
	    unsigned width, unsigned height, unsigned *cols, unsigned *rows);
/* The same, from a cell size (for fonts on the card). */
bool	setup_grid_for_cell(unsigned cell_w, unsigned cell_h, unsigned size,
	    unsigned width, unsigned height, unsigned *cols, unsigned *rows);

#endif /* SETUP_H */

/*
 * menu.h — the Tools menu: a small box over the screen (Ctrl+F3 by
 * default) with the terminal's local actions, and the hotkeys that run
 * those actions directly.
 *
 * Layering: pure module (host-testable). It draws into its own cells and
 * hands them to the renderer as an overlay (render.h), so the screen
 * underneath is never touched and keeps updating while the menu is open.
 * Running the chosen action is the app's job; this only picks it.
 *
 * The box uses the theme's colours swapped (theme background on theme
 * foreground), like the status line; the selected row is in the theme's
 * own colours.
 */
#ifndef MENU_H
#define MENU_H

#include <stdbool.h>

#include "font.h"
#include "keys.h"
#include "render.h"
#include "settings.h"

#define MENU_COLS	40
#define MENU_ROWS	(TOOL_COUNT + 3)	/* border, title, items, border */
#define MENU_STATE_MAX	8

/* menu_key results that aren't a tool. */
#define MENU_STAY	(-1)	/* key handled (or ignored), menu still open */
#define MENU_CLOSE	(-2)	/* Esc or the menu's own hotkey */

struct menu {
	const struct font *font;
	uint16_t	 fg;		/* the theme's colours */
	uint16_t	 bg;
	unsigned	 count;		/* items shown */
	unsigned	 selected;	/* index into items[] */
	uint8_t		 items[TOOL_COUNT];	/* enum tool, menu order */
	char		 state[TOOL_COUNT][MENU_STATE_MAX];	/* "on" ... */
	struct hotkey	 hotkeys[TOOL_COUNT];
	struct cell	 cells[MENU_ROWS * MENU_COLS];
	struct render_overlay overlay;	/* points into cells */
};

/*
 * Open the menu centred on a cols x rows screen. `available[tool]` says
 * which tools to list (things not built, or not possible right now, are
 * left out); `state[tool]` is a short state shown beside one ("on",
 * "off") or nullptr. The overlay is ready in menu->overlay.
 */
void	menu_open(struct menu *menu, const struct font *font, uint16_t fg,
	    uint16_t bg, unsigned cols, unsigned rows,
	    const bool available[TOOL_COUNT],
	    const char *const state[TOOL_COUNT],
	    const struct hotkey hotkeys[TOOL_COUNT]);

/* Up/Down move, Enter picks (returns the enum tool), Esc closes. */
int	menu_key(struct menu *menu, const struct key_event *key);

/* Does `key` press `hotkey`? (An unbound hotkey matches nothing.) */
bool	hotkey_matches(const struct hotkey *hotkey,
	    const struct key_event *key);

/* Is the tool built yet? (Capture, replay and transfers come later.) */
bool	tool_ready(enum tool tool);

/* The name of a tool, as the menu and SET-UP's Hotkeys page show it. */
const char *tool_name(enum tool tool);

#endif /* MENU_H */

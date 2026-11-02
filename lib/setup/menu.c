/*
 * menu.c — the Tools menu (see menu.h).
 *
 * Layout, MENU_COLS wide, one row per listed tool:
 *
 *   +-------------- Tools ---------------+
 *   | Set-Up...                          |
 *   | Screenshot                     F12 |
 *   | Printer              on            |
 *   | ...                                |
 *   +------------------------------------+
 *
 * The name on the left, a state ("on"/"off") in the middle, the hotkey
 * on the right. The box itself is box.c's.
 */
#include "menu.h"

#include <string.h>

#include "box.h"

static const char *const tool_names[TOOL_COUNT] = {
	[TOOL_MENU] = "Tools menu",
	[TOOL_SETUP] = "Set-Up...",
	[TOOL_SCREENSHOT] = "Screenshot",
	[TOOL_PRINT_SCREEN] = "Print screen",
	[TOOL_PRINTER] = "Printer",
	[TOOL_CAPTURE] = "Capture",
	[TOOL_REPLAY] = "Replay file...",
	[TOOL_SEND] = "Send file...",
	[TOOL_RECEIVE] = "Receive file...",
	[TOOL_CLEAR] = "Clear screen",
	[TOOL_SOFT_RESET] = "Soft reset",
	[TOOL_HARD_RESET] = "Hard reset",
};

bool
tool_ready(enum tool tool)
{
	switch (tool) {
	case TOOL_SEND:
	case TOOL_RECEIVE:
		return false;	/* phase H steps still to come */
	default:
		return (unsigned)tool < TOOL_COUNT;
	}
}

const char *
tool_name(enum tool tool)
{
	return (unsigned)tool < TOOL_COUNT ? tool_names[tool] : "?";
}

bool
hotkey_matches(const struct hotkey *hotkey, const struct key_event *key)
{
	if (hotkey->code == KEY_NONE || hotkey->code != key->code ||
	    hotkey->mods != key->mods)
		return false;
	return key->code != KEY_CHAR || hotkey->byte == key->byte;
}

static void
draw(struct menu *menu)
{
	const struct box box = {
		.font = menu->font,
		.cells = menu->cells,
		.stride = MENU_COLS,
		.cols = MENU_COLS,
		.rows = menu->count + 3,
		.fg = menu->fg,
		.bg = menu->bg,
	};

	box_frame(&box, "Tools");
	(void)box_text(&box, 1, 2, MENU_COLS, "Up/Down, Enter; Esc closes",
	    false);
	for (unsigned i = 0; i < menu->count; i++) {
		unsigned row = i + 2;
		enum tool tool = menu->items[i];
		bool chosen = i == menu->selected;
		char key[24];

		box_row(&box, row, chosen);
		(void)box_text(&box, row, 2, MENU_COLS, tool_name(tool),
		    chosen);
		(void)box_text(&box, row, 20, MENU_COLS, menu->state[tool],
		    chosen);
		if (menu->hotkeys[tool].code != KEY_NONE) {
			keys_name(menu->hotkeys[tool].code,
			    menu->hotkeys[tool].byte,
			    menu->hotkeys[tool].mods, key, sizeof(key));
			box_text_right(&box, row, key, 14, chosen);
		}
	}
}

void
menu_open(struct menu *menu, const struct font *font, uint16_t fg,
    uint16_t bg, unsigned cols, unsigned rows,
    const bool available[TOOL_COUNT], const char *const state[TOOL_COUNT],
    const struct hotkey hotkeys[TOOL_COUNT])
{
	memset(menu, 0, sizeof(*menu));
	menu->font = font;
	menu->fg = fg;
	menu->bg = bg;
	for (unsigned tool = 0; tool < TOOL_COUNT; tool++) {
		if (tool != TOOL_MENU && available[tool])
			menu->items[menu->count++] = (uint8_t)tool;
		if (state[tool] != nullptr) {
			strncpy(menu->state[tool], state[tool],
			    MENU_STATE_MAX - 1);
		}
		menu->hotkeys[tool] = hotkeys[tool];
	}
	draw(menu);

	unsigned height = menu->count + 3;
	menu->overlay = (struct render_overlay){
		.row = (uint8_t)(rows > height ? (rows - height) / 2 : 0),
		.rows = (uint8_t)height,
		.col = (uint16_t)(cols > MENU_COLS ? (cols - MENU_COLS) / 2 : 0),
		.cols = MENU_COLS,
		.cells = menu->cells,
	};
}

int
menu_key(struct menu *menu, const struct key_event *key)
{
	if (key->code == KEY_ESCAPE ||
	    hotkey_matches(&menu->hotkeys[TOOL_MENU], key))
		return MENU_CLOSE;
	if (menu->count == 0)
		return MENU_STAY;
	switch (key->code) {
	case KEY_UP:
		menu->selected = (menu->selected + menu->count - 1) %
		    menu->count;
		break;
	case KEY_DOWN:
		menu->selected = (menu->selected + 1) % menu->count;
		break;
	case KEY_HOME:
		menu->selected = 0;
		break;
	case KEY_END:
		menu->selected = menu->count - 1;
		break;
	case KEY_ENTER:
		return menu->items[menu->selected];
	default:
		/* Any other key is the menu's: nothing reaches the host. */
		return MENU_STAY;
	}
	draw(menu);
	return MENU_STAY;
}

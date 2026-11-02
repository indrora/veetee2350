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
 * on the right. Box drawing uses U+250x when the font has it, else
 * +, - and |.
 */
#include "menu.h"

#include <string.h>

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
	case TOOL_CAPTURE:
	case TOOL_REPLAY:
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

/* A glyph, or `fallback` where the font lacks the codepoint. */
static uint16_t
glyph(const struct menu *menu, uint32_t codepoint, char fallback)
{
	uint16_t index = font_glyph_for(menu->font, codepoint);

	return index != FONT_GLYPH_MISSING ? index :
	    font_glyph_for(menu->font, (uint8_t)fallback);
}

static void
put_cell(struct menu *menu, unsigned row, unsigned col, uint16_t index,
    uint16_t fg, uint16_t bg)
{
	if (row < MENU_ROWS && col < MENU_COLS)
		menu->cells[row * MENU_COLS + col] =
		    grid_make_cell(index, fg, bg, 0);
}

static void
put_text(struct menu *menu, unsigned row, unsigned col, const char *text,
    uint16_t fg, uint16_t bg)
{
	for (; *text != '\0' && col < MENU_COLS - 1; text++, col++)
		put_cell(menu, row, col, glyph(menu, (uint8_t)*text, '?'), fg,
		    bg);
}

static void
draw(struct menu *menu)
{
	/* Swapped theme colours for the box, the theme's own for the bar. */
	const uint16_t box_fg = menu->bg, box_bg = menu->fg;
	const unsigned rows = menu->count + 3;
	const uint16_t space = glyph(menu, ' ', ' ');
	const uint16_t horizontal = glyph(menu, 0x2500, '-');
	const uint16_t vertical = glyph(menu, 0x2502, '|');

	for (unsigned row = 0; row < rows; row++)
		for (unsigned col = 0; col < MENU_COLS; col++)
			put_cell(menu, row, col, space, box_fg, box_bg);
	for (unsigned col = 1; col < MENU_COLS - 1; col++) {
		put_cell(menu, 0, col, horizontal, box_fg, box_bg);
		put_cell(menu, rows - 1, col, horizontal, box_fg, box_bg);
	}
	for (unsigned row = 1; row < rows - 1; row++) {
		put_cell(menu, row, 0, vertical, box_fg, box_bg);
		put_cell(menu, row, MENU_COLS - 1, vertical, box_fg, box_bg);
	}
	put_cell(menu, 0, 0, glyph(menu, 0x250c, '+'), box_fg, box_bg);
	put_cell(menu, 0, MENU_COLS - 1, glyph(menu, 0x2510, '+'), box_fg,
	    box_bg);
	put_cell(menu, rows - 1, 0, glyph(menu, 0x2514, '+'), box_fg, box_bg);
	put_cell(menu, rows - 1, MENU_COLS - 1, glyph(menu, 0x2518, '+'),
	    box_fg, box_bg);
	put_text(menu, 0, (MENU_COLS - 7) / 2, " Tools ", box_fg, box_bg);
	put_text(menu, 1, 2, "Up/Down, Enter; Esc closes", box_fg, box_bg);

	for (unsigned i = 0; i < menu->count; i++) {
		unsigned row = i + 2;
		enum tool tool = menu->items[i];
		bool chosen = i == menu->selected;
		uint16_t fg = chosen ? menu->fg : box_fg;
		uint16_t bg = chosen ? menu->bg : box_bg;
		char key[24];

		for (unsigned col = 1; col < MENU_COLS - 1; col++)
			put_cell(menu, row, col, space, fg, bg);
		put_text(menu, row, 2, tool_name(tool), fg, bg);
		put_text(menu, row, 20, menu->state[tool], fg, bg);
		if (menu->hotkeys[tool].code != KEY_NONE) {
			keys_name(menu->hotkeys[tool].code,
			    menu->hotkeys[tool].byte,
			    menu->hotkeys[tool].mods, key, sizeof(key));
			size_t len = strlen(key);
			if (len > 14)
				len = 14;
			key[len] = '\0';
			put_text(menu, row, MENU_COLS - 2 - (unsigned)len, key,
			    fg, bg);
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

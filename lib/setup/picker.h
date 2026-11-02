/*
 * picker.h — a file picker over the screen: one directory of the SD
 * card in a box, Up/Down to choose, Enter to open a directory or take a
 * file. Used by the Tools menu's "Replay file...".
 *
 * Layering: pure module (host-testable). The app reads the directory
 * (lib/hal/storage.h) and adds its entries; this sorts, draws and
 * handles keys, and hands the drawing to the renderer as an overlay
 * (render.h), like the Tools menu (menu.h).
 *
 *   +------------- /capture --------------+
 *   | Enter plays; Esc closes             |
 *   | ../                                 |
 *   | 0001.rec                       12.0K|
 *   | 0002.rec                        310 |
 *   |                                     |
 *   +-------------------------------------+
 *
 * Directories first, then files, each sorted by name, ignoring case.
 * Only the first PICKER_MAX entries a directory lists are kept (as with
 * fonts and schemes); the title says when there were more.
 */
#ifndef PICKER_H
#define PICKER_H

#include <stdbool.h>
#include <stdint.h>

#include "box.h"
#include "font.h"
#include "keys.h"
#include "render.h"

#define PICKER_MAX	64
#define PICKER_NAME	64	/* storage.h's STORAGE_NAME_MAX */
#define PICKER_COLS	52
#define PICKER_LIST	16	/* names shown at once */
#define PICKER_ROWS	(PICKER_LIST + 3)	/* border, hint, list, border */

/* picker_key results that aren't an entry. */
#define PICKER_STAY	(-1)
#define PICKER_CLOSE	(-2)

struct picker_entry {
	char		 name[PICKER_NAME];
	uint32_t	 size;
	bool		 dir;
};

struct picker {
	struct box	 box;
	char		 dir[PICKER_NAME];
	char		 hint[PICKER_COLS];
	unsigned	 count;
	unsigned	 selected;
	unsigned	 top;		/* first entry shown */
	unsigned	 list;		/* rows of names that fit */
	bool		 more;		/* the directory had more than MAX */
	struct picker_entry entries[PICKER_MAX];
	struct cell	 cells[PICKER_ROWS * PICKER_COLS];
	struct render_overlay overlay;
};

/* Start a listing of `dir` ("/capture"); "../" comes first unless "/". */
void	picker_begin(struct picker *picker, const char *dir, const char *hint);

/* Add an entry; false once full (the rest are dropped). */
bool	picker_add(struct picker *picker, const char *name, uint32_t size,
	    bool dir);

/* Sort, draw, centre on a cols x rows screen; the overlay is ready. */
void	picker_show(struct picker *picker, const struct font *font,
	    uint16_t fg, uint16_t bg, unsigned cols, unsigned rows);

/* Up/Down/PgUp/PgDn/Home/End move; Enter picks (the entry's index);
 * Esc closes. */
int	picker_key(struct picker *picker, const struct key_event *key);

/* An entry by index (from picker_key), and the path it names. */
const struct picker_entry *picker_entry(const struct picker *picker,
	    int index);
/* `dir` joined with the entry, ".." going up a level. */
void	picker_path(const struct picker *picker, int index, char *path,
	    unsigned len);

#endif /* PICKER_H */

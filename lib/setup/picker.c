/*
 * picker.c — the file picker (see picker.h).
 */
#include "picker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool
is_root(const char *dir)
{
	return dir[0] == '\0' || strcmp(dir, "/") == 0;
}

void
picker_begin(struct picker *picker, const char *dir, const char *hint)
{
	picker->count = 0;
	picker->selected = 0;
	picker->top = 0;
	picker->more = false;
	snprintf(picker->dir, sizeof(picker->dir), "%s", dir);
	snprintf(picker->hint, sizeof(picker->hint), "%s", hint);
	if (!is_root(dir))
		(void)picker_add(picker, "..", 0, true);
}

bool
picker_add(struct picker *picker, const char *name, uint32_t size,
    bool dir)
{
	/* A name too long to keep can't be opened by it: leave it out. */
	if (strlen(name) >= PICKER_NAME)
		return true;
	if (picker->count == PICKER_MAX) {
		picker->more = true;
		return false;
	}
	struct picker_entry *entry = &picker->entries[picker->count++];
	snprintf(entry->name, sizeof(entry->name), "%s", name);
	entry->size = size;
	entry->dir = dir;
	return true;
}

/* "..", then directories, then files; by name, ignoring case. */
static int
compare(const void *a, const void *b)
{
	const struct picker_entry *left = a, *right = b;

	if (strcmp(left->name, "..") == 0)
		return -1;
	if (strcmp(right->name, "..") == 0)
		return 1;
	if (left->dir != right->dir)
		return left->dir ? -1 : 1;
	return strcasecmp(left->name, right->name);
}

/* 310, 12.0K, 4.5M: at most 5 characters. */
static void
size_text(uint32_t size, char *out, size_t len)
{
	if (size < 1000)
		snprintf(out, len, "%lu", (unsigned long)size);
	else if (size < 100u * 1024u)
		snprintf(out, len, "%lu.%luK", (unsigned long)(size / 1024u),
		    (unsigned long)(size % 1024u * 10u / 1024u));
	else if (size < 1000u * 1024u)
		snprintf(out, len, "%luK", (unsigned long)(size / 1024u));
	else
		snprintf(out, len, "%lu.%luM",
		    (unsigned long)(size / (1024u * 1024u)),
		    (unsigned long)(size % (1024u * 1024u) * 10u /
		    (1024u * 1024u)));
}

static void
draw(struct picker *picker)
{
	const struct box *box = &picker->box;
	char title[PICKER_NAME + 16];

	snprintf(title, sizeof(title), "%s%s", picker->dir,
	    picker->more ? " (first 64)" : "");
	box_frame(box, title);
	(void)box_text(box, 1, 2, box->cols, picker->hint, false);
	if (picker->count == 0)
		(void)box_text(box, 2, 2, box->cols, "(empty)", false);
	for (unsigned i = 0; i < picker->list; i++) {
		unsigned index = picker->top + i;
		if (index >= picker->count)
			break;
		const struct picker_entry *entry = &picker->entries[index];
		bool chosen = index == picker->selected;
		unsigned row = i + 2;
		char name[PICKER_NAME + 1];

		box_row(box, row, chosen);
		snprintf(name, sizeof(name), "%s%s", entry->name,
		    entry->dir ? "/" : "");
		/* Long names stop short of the size column. */
		(void)box_text(box, row, 2, box->cols - 8, name, chosen);
		if (!entry->dir) {
			char size[8];
			size_text(entry->size, size, sizeof(size));
			box_text_right(box, row, size, 5, chosen);
		}
	}
	/* More above or below: a mark in the border. */
	if (picker->top > 0)
		(void)box_text(box, 0, box->cols - 3, box->cols, "^", false);
	if (picker->top + picker->list < picker->count)
		(void)box_text(box, box->rows - 1, box->cols - 3, box->cols,
		    "v", false);
}

/* Keep the selection on screen. */
static void
follow(struct picker *picker)
{
	if (picker->selected < picker->top)
		picker->top = picker->selected;
	else if (picker->selected >= picker->top + picker->list)
		picker->top = picker->selected + 1 - picker->list;
}

void
picker_show(struct picker *picker, const struct font *font, uint16_t fg,
    uint16_t bg, unsigned cols, unsigned rows)
{
	qsort(picker->entries, picker->count, sizeof(picker->entries[0]),
	    compare);

	/* As tall as the entries need (one row for "(empty)"), up to
	 * PICKER_LIST of them and what the screen has room for. */
	unsigned width = cols < PICKER_COLS ? cols : PICKER_COLS;
	unsigned height = (picker->count > 0 ? picker->count : 1) + 3;
	if (height > PICKER_ROWS)
		height = PICKER_ROWS;
	if (height > rows)
		height = rows;
	picker->list = height > 3 ? height - 3 : 1;
	picker->box = (struct box){
		.font = font,
		.cells = picker->cells,
		.stride = width,	/* the overlay's rows are `cols` apart */
		.cols = width,
		.rows = height,
		.fg = fg,
		.bg = bg,
	};
	follow(picker);
	draw(picker);
	picker->overlay = (struct render_overlay){
		.row = (uint8_t)((rows - height) / 2),
		.rows = (uint8_t)height,
		.col = (uint16_t)((cols - width) / 2),
		.cols = (uint16_t)width,
		.cells = picker->cells,
	};
}

int
picker_key(struct picker *picker, const struct key_event *key)
{
	const unsigned last = picker->count > 0 ? picker->count - 1 : 0;

	switch (key->code) {
	case KEY_ESCAPE:
		return PICKER_CLOSE;
	case KEY_UP:
		if (picker->selected > 0)
			picker->selected--;
		break;
	case KEY_DOWN:
		if (picker->selected < last)
			picker->selected++;
		break;
	case KEY_PAGE_UP:
		picker->selected = picker->selected > picker->list ?
		    picker->selected - picker->list : 0;
		break;
	case KEY_PAGE_DOWN:
		picker->selected = picker->selected + picker->list < last ?
		    picker->selected + picker->list : last;
		break;
	case KEY_HOME:
		picker->selected = 0;
		break;
	case KEY_END:
		picker->selected = last;
		break;
	case KEY_ENTER:
		return picker->count > 0 ? (int)picker->selected : PICKER_STAY;
	default:
		return PICKER_STAY;
	}
	follow(picker);
	draw(picker);
	return PICKER_STAY;
}

const struct picker_entry *
picker_entry(const struct picker *picker, int index)
{
	return index >= 0 && (unsigned)index < picker->count ?
	    &picker->entries[index] : nullptr;
}

void
picker_path(const struct picker *picker, int index, char *path,
    unsigned len)
{
	const struct picker_entry *entry = picker_entry(picker, index);

	if (entry == nullptr) {
		snprintf(path, len, "%s", picker->dir);
		return;
	}
	if (strcmp(entry->name, "..") == 0) {
		snprintf(path, len, "%s", picker->dir);
		char *slash = strrchr(path, '/');
		if (slash == path || slash == nullptr)
			snprintf(path, len, "/");
		else
			*slash = '\0';
		return;
	}
	snprintf(path, len, "%s%s%s", picker->dir,
	    is_root(picker->dir) ? "" : "/", entry->name);
}

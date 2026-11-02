/*
 * grid.c — cell grid storage, clearing and row-map scrolling.
 */
#include "grid.h"

#include <stdlib.h>
#include <string.h>

bool
grid_init(struct grid *grid, unsigned cols, unsigned rows, struct cell blank)
{
	memset(grid, 0, sizeof(*grid));
	if (cols == 0 || cols > GRID_MAX_COLS || rows == 0 ||
	    rows > GRID_MAX_ROWS)
		return false;

	grid->cells = malloc((size_t)cols * rows * sizeof(*grid->cells));
	if (grid->cells == nullptr)
		return false;
	grid->cols = (uint16_t)cols;
	grid->rows = (uint16_t)rows;
	for (unsigned row = 0; row < rows; row++) {
		grid->row_map[row] = (uint8_t)row;
		grid_clear_span(grid, row, 0, cols, blank);
	}
	grid->cursor_visible = true;
	grid->cursor_blink = true;
	grid->cursor_shape = CURSOR_BLOCK;
	grid->border2 = grid_dup565(blank.bg);
	return true;
}

void
grid_free(struct grid *grid)
{
	free(grid->cells);
	memset(grid, 0, sizeof(*grid));
}

void
grid_clear_span(struct grid *grid, unsigned row, unsigned col_start,
    unsigned col_end, struct cell blank)
{
	struct cell *cells = grid_row(grid, row);

	if (col_end > grid->cols)
		col_end = grid->cols;
	for (unsigned col = col_start; col < col_end; col++)
		cells[col] = blank;
}

/*
 * Rotate row_map[top..bottom) left by `count` and blank the rows that
 * rotated to the end. The physical rows that scroll off are reused for
 * the new blank rows, so no cell data is copied.
 */
void
grid_scroll_up(struct grid *grid, unsigned top, unsigned bottom,
    unsigned count, struct cell blank)
{
	uint8_t spill[GRID_MAX_ROWS];
	unsigned height;

	if (bottom > grid->rows)
		bottom = grid->rows;
	if (top >= bottom)
		return;
	height = bottom - top;
	if (count > height)
		count = height;

	memcpy(spill, &grid->row_map[top], count);
	memmove(&grid->row_map[top], &grid->row_map[top + count],
	    height - count);
	memcpy(&grid->row_map[bottom - count], spill, count);
	for (unsigned row = bottom - count; row < bottom; row++) {
		grid_clear_span(grid, row, 0, grid->cols, blank);
		*grid_row_attr(grid, row) = ROW_NORMAL;
	}
}

void
grid_scroll_down(struct grid *grid, unsigned top, unsigned bottom,
    unsigned count, struct cell blank)
{
	uint8_t spill[GRID_MAX_ROWS];
	unsigned height;

	if (bottom > grid->rows)
		bottom = grid->rows;
	if (top >= bottom)
		return;
	height = bottom - top;
	if (count > height)
		count = height;

	memcpy(spill, &grid->row_map[bottom - count], count);
	memmove(&grid->row_map[top + count], &grid->row_map[top],
	    height - count);
	memcpy(&grid->row_map[top], spill, count);
	for (unsigned row = top; row < top + count; row++) {
		grid_clear_span(grid, row, 0, grid->cols, blank);
		*grid_row_attr(grid, row) = ROW_NORMAL;
	}
}

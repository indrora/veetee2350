/*
 * screenshot.h — save what's on the HDMI output to the SD card.
 */
#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#include <stdbool.h>
#include <stddef.h>

#include "render.h"

#define SCREENSHOT_DIR	"/screens"

/*
 * Render every line of the screen through `render` and write it to the
 * next free /screens/NNNN.bmp or .qoi (`format`: enum screenshot_format;
 * numbers are shared, so 0007.qoi follows 0006.bmp). On success the
 * path is copied to `path`; on failure a reason to `error`. Blocking
 * (about a second at 640x480); feeds the watchdog as it goes.
 */
bool	screenshot_save(struct render *render, unsigned format, char *path,
	    size_t path_len, char *error, size_t error_len);

#endif /* SCREENSHOT_H */

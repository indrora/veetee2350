/*
 * screenshot.c — /screens/NNNN.bmp from the live renderer.
 *
 * No framebuffer exists to copy: the picture is generated a scanline at
 * a time as the DVI output needs it. So a screenshot asks the renderer
 * for each line again, here on core 0 in the main loop, into a line
 * buffer, and streams that to the card. The interrupt-driven renderer
 * keeps feeding the display meanwhile; both only read the grid. What's
 * captured is exactly what the encoder sees: borders, cursor, SET-UP,
 * Sixel cells.
 *
 * Two formats:
 *  - BMP in the renderer's own pixel format, 16-bit RGB565 (BI_BITFIELDS
 *    with 5-6-5 masks), stored top-down (negative height), which every
 *    common viewer reads. Every supported width x 2 bytes is a multiple
 *    of 4, so rows need no padding. 600 KB at 640x480.
 *  - QOI (lib/qoi, streamed), RGB565 widened to 8 bits a channel by bit
 *    replication (0x1f -> 0xff), so white stays white. A text screen is
 *    mostly runs: typically a few tens of KB.
 */
#include "screenshot.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "qoi_stream.h"
#include "settings.h"
#include "storage.h"
#include "video_modes.h"

#define BMP_HEADER_BYTES	(14 + 40 + 12)	/* file + info + 3 masks */

/*
 * The renderer stores pixels a word or two at a time (STRD/STM), which
 * fault on an address that isn't word-aligned; a uint16_t array only
 * promises 2. The DVI line ring is aligned for the same reason.
 */
alignas(8) static uint16_t line_pixels[VIDEO_MAX_WIDTH];

static void
put_le16(uint8_t *at, uint16_t value)
{
	at[0] = (uint8_t)value;
	at[1] = (uint8_t)(value >> 8);
}

static void
put_le32(uint8_t *at, uint32_t value)
{
	put_le16(at, (uint16_t)value);
	put_le16(at + 2, (uint16_t)(value >> 16));
}

static void
bmp_header(uint8_t header[BMP_HEADER_BYTES], unsigned width,
    unsigned height)
{
	uint32_t image = (uint32_t)width * height * 2;

	memset(header, 0, BMP_HEADER_BYTES);
	/* BITMAPFILEHEADER */
	header[0] = 'B';
	header[1] = 'M';
	put_le32(header + 2, BMP_HEADER_BYTES + image);
	put_le32(header + 10, BMP_HEADER_BYTES);
	/* BITMAPINFOHEADER */
	put_le32(header + 14, 40);
	put_le32(header + 18, width);
	put_le32(header + 22, (uint32_t)-(int32_t)height);	/* top-down */
	put_le16(header + 26, 1);			/* planes */
	put_le16(header + 28, 16);			/* bits per pixel */
	put_le32(header + 30, 3);			/* BI_BITFIELDS */
	put_le32(header + 34, image);
	put_le32(header + 38, 2835);			/* 72 dpi */
	put_le32(header + 42, 2835);
	/* Channel masks: RGB565. */
	put_le32(header + 54, 0xf800);
	put_le32(header + 58, 0x07e0);
	put_le32(header + 62, 0x001f);
}

struct sink {
	int	handle;
};

static bool
write_out(const uint8_t *bytes, size_t len, void *ctx)
{
	const struct sink *sink = ctx;

	return storage_write(sink->handle, bytes, len) == (long)len;
}

/* RGB565 -> 8-bit channels, replicating the top bits into the bottom. */
static void
widen565(uint16_t pixel, uint8_t rgb[3])
{
	unsigned red = pixel >> 11, green = (pixel >> 5) & 63, blue = pixel & 31;

	rgb[0] = (uint8_t)(red << 3 | red >> 2);
	rgb[1] = (uint8_t)(green << 2 | green >> 4);
	rgb[2] = (uint8_t)(blue << 3 | blue >> 2);
}

/* One scanline, without the side effect line 0 has on the frame count. */
static void
render_line(struct render *render, unsigned line)
{
	uint32_t frame = render->frame;

	render_scanline(line_pixels, line, render);
	if (line == 0)
		render->frame = frame;
	if ((line & 31) == 0)
		board_watchdog_feed();
}

static bool
write_bmp(struct render *render, int handle)
{
	uint8_t header[BMP_HEADER_BYTES];
	size_t row = render->width * sizeof(uint16_t);

	bmp_header(header, render->width, render->height);
	if (storage_write(handle, header, sizeof(header)) != (long)sizeof(header))
		return false;
	for (unsigned line = 0; line < render->height; line++) {
		render_line(render, line);
		if (storage_write(handle, line_pixels, row) != (long)row)
			return false;
	}
	return true;
}

static bool
write_qoi(struct render *render, int handle)
{
	struct sink sink = {handle};
	static struct qoi_encoder qoi;	/* 760 bytes: not on the stack */
	uint8_t rgb[3];

	if (!qoi_begin(&qoi, render->width, render->height, write_out, &sink))
		return false;
	for (unsigned line = 0; line < render->height; line++) {
		render_line(render, line);
		for (unsigned x = 0; x < render->width; x++) {
			widen565(line_pixels[x], rgb);
			qoi_pixel(&qoi, rgb[0], rgb[1], rgb[2]);
		}
	}
	return qoi_end(&qoi);
}

/* Highest NNNN in "NNNN.bmp" names, so the next one can follow it. */
static bool
note_number(const char *name, uint32_t size, bool is_dir,
    const uint8_t *head, size_t head_len, void *ctx)
{
	unsigned *highest = ctx;
	unsigned number = 0;

	(void)size;
	(void)head;
	(void)head_len;
	board_watchdog_feed();
	if (is_dir || strlen(name) != 8 || (strcmp(name + 4, ".bmp") != 0 &&
	    strcmp(name + 4, ".qoi") != 0))
		return true;
	for (int i = 0; i < 4; i++) {
		if (name[i] < '0' || name[i] > '9')
			return true;
		number = number * 10 + (unsigned)(name[i] - '0');
	}
	if (number > *highest)
		*highest = number;
	return true;
}

bool
screenshot_save(struct render *render, unsigned format, char *path,
    size_t path_len, char *error, size_t error_len)
{
	unsigned highest = 0;
	int handle;
	bool ok;

	if (!storage_mount()) {
		snprintf(error, error_len, "%s", storage_error());
		return false;
	}
	if (!storage_mkdir(SCREENSHOT_DIR)) {
		snprintf(error, error_len, "can't create " SCREENSHOT_DIR);
		return false;
	}
	(void)storage_list(SCREENSHOT_DIR, 0, note_number, &highest);
	if (highest >= 9999) {
		snprintf(error, error_len, SCREENSHOT_DIR " is full (9999)");
		return false;
	}
	snprintf(path, path_len, SCREENSHOT_DIR "/%04u.%s", highest + 1,
	    format == SHOT_QOI ? "qoi" : "bmp");
	handle = storage_open(path, STORAGE_CREATE);
	if (handle < 0) {
		snprintf(error, error_len, "%s", storage_error());
		return false;
	}
	ok = format == SHOT_QOI ? write_qoi(render, handle) :
	    write_bmp(render, handle);
	if (!storage_close(handle))
		ok = false;
	if (!ok)
		snprintf(error, error_len, "%s", storage_error());
	return ok;
}

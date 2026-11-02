/*
 * qoi_stream.h — a streaming QOI ("Quite OK Image") encoder.
 *
 * QOI (https://qoiformat.org, Dominic Szablewski, 2021) is a lossless
 * format built from five tiny ops: a run of the previous pixel, a
 * reference into a 64-entry hash of recent colours, a small or medium
 * difference from the previous pixel, or a literal. Terminal screens
 * are mostly long runs of background, so a 600 KB screen typically
 * becomes a few tens of kilobytes.
 *
 * The reference implementation (qoi.h) encodes a whole image held in
 * memory; we have no framebuffer -- pixels exist one scanline at a time
 * -- so this follows the specification as a stream: begin, feed pixels
 * in order, end. Output goes out through a callback in chunks. RGB only
 * (channels = 3, sRGB); alpha is always 255, as the format requires
 * for 3-channel images.
 */
#ifndef QOI_STREAM_H
#define QOI_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Write out encoded bytes; false aborts the image. */
typedef bool (*qoi_write_fn)(const uint8_t *bytes, size_t len, void *ctx);

struct qoi_encoder {
	qoi_write_fn	 write;
	void		*ctx;
	bool		 failed;
	/*
	 * Hash of recently seen colours, RGBA. Alpha matters even though
	 * it's always 255 in our pixels: the decoder starts every slot at
	 * (0,0,0,0), so an untouched slot must not match opaque black.
	 */
	uint8_t		 index[64][4];
	uint8_t		 prev[3];
	unsigned	 run;		/* pending repeats of prev */
	size_t		 used;
	uint8_t		 buf[512];
};

bool	qoi_begin(struct qoi_encoder *qoi, uint32_t width, uint32_t height,
	    qoi_write_fn write, void *ctx);
void	qoi_pixel(struct qoi_encoder *qoi, uint8_t red, uint8_t green,
	    uint8_t blue);
/* Flush the run and the end marker; false if any write failed. */
bool	qoi_end(struct qoi_encoder *qoi);

#endif /* QOI_STREAM_H */

/*
 * qoi_stream.c — streaming QOI encoder, after the QOI specification
 * (v1.0, 2022). Each pixel is encoded, in order of preference, as:
 *
 *   QOI_OP_RUN    11xxxxxx          1-62 repeats of the previous pixel
 *   QOI_OP_INDEX  00xxxxxx          the colour at index[hash]
 *   QOI_OP_DIFF   01rrggbb          each channel -2..1 from previous
 *   QOI_OP_LUMA   10gggggg rrrrbbbb green -32..31, red/blue -8..7
 *                                   relative to green's change
 *   QOI_OP_RGB    0xfe r g b        literal
 *
 * hash = (r*3 + g*5 + b*7 + a*11) % 64, with a = 255 here. Differences
 * wrap modulo 256. The stream starts from previous = (0,0,0,255) and an
 * all-zero index, and ends with seven 0x00 bytes and one 0x01.
 */
#include "qoi_stream.h"

#include <string.h>

#define OP_INDEX	0x00
#define OP_DIFF		0x40
#define OP_LUMA		0x80
#define OP_RUN		0xc0
#define OP_RGB		0xfe
#define RUN_MAX		62

static void
flush(struct qoi_encoder *qoi)
{
	if (qoi->used > 0 && !qoi->failed &&
	    !qoi->write(qoi->buf, qoi->used, qoi->ctx))
		qoi->failed = true;
	qoi->used = 0;
}

static void
emit(struct qoi_encoder *qoi, uint8_t byte)
{
	if (qoi->used == sizeof(qoi->buf))
		flush(qoi);
	qoi->buf[qoi->used++] = byte;
}

static void
emit_run(struct qoi_encoder *qoi)
{
	if (qoi->run > 0) {
		emit(qoi, (uint8_t)(OP_RUN | (qoi->run - 1)));
		qoi->run = 0;
	}
}

static void
put_be32(struct qoi_encoder *qoi, uint32_t value)
{
	for (int shift = 24; shift >= 0; shift -= 8)
		emit(qoi, (uint8_t)(value >> shift));
}

bool
qoi_begin(struct qoi_encoder *qoi, uint32_t width, uint32_t height,
    qoi_write_fn write, void *ctx)
{
	memset(qoi, 0, sizeof(*qoi));
	qoi->write = write;
	qoi->ctx = ctx;
	emit(qoi, 'q');
	emit(qoi, 'o');
	emit(qoi, 'i');
	emit(qoi, 'f');
	put_be32(qoi, width);
	put_be32(qoi, height);
	emit(qoi, 3);	/* channels: RGB */
	emit(qoi, 0);	/* colorspace: sRGB with linear alpha */
	return !qoi->failed;
}

void
qoi_pixel(struct qoi_encoder *qoi, uint8_t red, uint8_t green, uint8_t blue)
{
	const uint8_t *prev = qoi->prev;

	if (red == prev[0] && green == prev[1] && blue == prev[2]) {
		if (++qoi->run == RUN_MAX)
			emit_run(qoi);
		return;
	}
	emit_run(qoi);

	unsigned slot = (red * 3u + green * 5u + blue * 7u + 255u * 11u) % 64u;
	uint8_t *entry = qoi->index[slot];
	if (entry[0] == red && entry[1] == green && entry[2] == blue &&
	    entry[3] == 255) {
		emit(qoi, (uint8_t)(OP_INDEX | slot));
	} else {
		int dr = (int8_t)(uint8_t)(red - prev[0]);
		int dg = (int8_t)(uint8_t)(green - prev[1]);
		int db = (int8_t)(uint8_t)(blue - prev[2]);
		int dr_dg = dr - dg, db_dg = db - dg;

		entry[0] = red;
		entry[1] = green;
		entry[2] = blue;
		entry[3] = 255;
		if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 &&
		    db <= 1) {
			emit(qoi, (uint8_t)(OP_DIFF | (dr + 2) << 4 |
			    (dg + 2) << 2 | (db + 2)));
		} else if (dg >= -32 && dg <= 31 && dr_dg >= -8 && dr_dg <= 7 &&
		    db_dg >= -8 && db_dg <= 7) {
			emit(qoi, (uint8_t)(OP_LUMA | (dg + 32)));
			emit(qoi, (uint8_t)((dr_dg + 8) << 4 | (db_dg + 8)));
		} else {
			emit(qoi, OP_RGB);
			emit(qoi, red);
			emit(qoi, green);
			emit(qoi, blue);
		}
	}
	qoi->prev[0] = red;
	qoi->prev[1] = green;
	qoi->prev[2] = blue;
}

bool
qoi_end(struct qoi_encoder *qoi)
{
	emit_run(qoi);
	for (int i = 0; i < 7; i++)
		emit(qoi, 0);
	emit(qoi, 1);
	flush(qoi);
	return !qoi->failed;
}

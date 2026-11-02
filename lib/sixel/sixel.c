/*
 * sixel.c — streaming DEC Sixel decoder. See sixel.h for the format.
 *
 * A byte-at-a-time state machine. Numeric parameters accumulate into
 * params[] and a command runs when the next non-parameter byte arrives,
 * which is why sixel_end() exists: "#1;2;100;0;0" may be the last thing
 * before ST, with nothing after it to trigger the definition.
 *
 * Tolerance follows xterm rather than the letter of DEC STD 070:
 * unknown bytes are ignored, controls inside the data (CR, LF, which
 * some encoders wrap lines with) are ignored, and out-of-range
 * parameters are clamped.
 */
#include "sixel.h"

#include <string.h>

enum {
	STATE_DATA,		/* sixels and command introducers */
	STATE_RASTER,		/* after '"' */
	STATE_COLOUR,		/* after '#' */
	STATE_REPEAT,		/* after '!' */
};

/* DEC STD 070 table 9-4 / VT340 defaults, from percentages. */
#define PCT(v)	((uint8_t)(((v) * 255 + 50) / 100))
const uint8_t sixel_vt340_palette[16][3] = {
	{PCT(0), PCT(0), PCT(0)},	/* 0 black */
	{PCT(20), PCT(20), PCT(80)},	/* 1 blue */
	{PCT(80), PCT(13), PCT(13)},	/* 2 red */
	{PCT(20), PCT(80), PCT(20)},	/* 3 green */
	{PCT(80), PCT(20), PCT(80)},	/* 4 magenta */
	{PCT(20), PCT(80), PCT(80)},	/* 5 cyan */
	{PCT(80), PCT(80), PCT(20)},	/* 6 yellow */
	{PCT(53), PCT(53), PCT(53)},	/* 7 grey 50% */
	{PCT(26), PCT(26), PCT(26)},	/* 8 grey 25% */
	{PCT(33), PCT(33), PCT(60)},	/* 9 blue* */
	{PCT(60), PCT(26), PCT(26)},	/* 10 red* */
	{PCT(33), PCT(60), PCT(33)},	/* 11 green* */
	{PCT(60), PCT(33), PCT(60)},	/* 12 magenta* */
	{PCT(33), PCT(60), PCT(60)},	/* 13 cyan* */
	{PCT(60), PCT(60), PCT(33)},	/* 14 yellow* */
	{PCT(80), PCT(80), PCT(80)},	/* 15 grey 75% */
};

static unsigned
clamp(uint32_t value, unsigned max)
{
	return value > max ? max : (unsigned)value;
}

/* One channel of HSL -> RGB, the usual piecewise-linear hue ramp. */
static uint8_t
hue_channel(unsigned hue, unsigned low, unsigned high)
{
	unsigned value;

	hue %= 360;
	if (hue < 60)
		value = low + (high - low) * hue / 60;
	else if (hue < 180)
		value = high;
	else if (hue < 240)
		value = low + (high - low) * (240 - hue) / 60;
	else
		value = low;
	return (uint8_t)(value * 255 / 100);
}

void
sixel_hls_to_rgb(unsigned hue, unsigned lightness, unsigned saturation,
    uint8_t rgb[3])
{
	unsigned high, low;

	lightness = clamp(lightness, 100);
	saturation = clamp(saturation, 100);
	/* DEC puts blue at 0 degrees; standard HSL puts red there. */
	hue = (hue + 240) % 360;
	if (lightness <= 50)
		high = lightness * (100 + saturation) / 100;
	else
		high = lightness + saturation - lightness * saturation / 100;
	low = 2 * lightness - high;
	rgb[0] = hue_channel(hue + 120, low, high);
	rgb[1] = hue_channel(hue, low, high);
	rgb[2] = hue_channel(hue + 240, low, high);
}

void
sixel_begin(struct sixel_decoder *decoder, const struct sixel_sink *sink,
    void *ctx)
{
	memset(decoder, 0, sizeof(*decoder));
	decoder->sink = sink;
	decoder->ctx = ctx;
	decoder->state = STATE_DATA;
}

/* Run the command whose parameters have been collected. */
static void
finish_command(struct sixel_decoder *decoder)
{
	const uint32_t *params = decoder->params;
	unsigned count = decoder->nparams;

	switch (decoder->state) {
	case STATE_RASTER:
		if (decoder->sink->raster != nullptr)
			decoder->sink->raster(decoder->ctx,
			    count > 0 ? clamp(params[0], 65535) : 1,
			    count > 1 ? clamp(params[1], 65535) : 1,
			    count > 2 ? clamp(params[2], 65535) : 0,
			    count > 3 ? clamp(params[3], 65535) : 0);
		break;
	case STATE_COLOUR:
		decoder->reg = count > 0 ? params[0] % SIXEL_REGISTERS : 0;
		if (count >= 5) {
			uint8_t rgb[3];
			if (params[1] == 1) {
				sixel_hls_to_rgb(clamp(params[2], 360),
				    params[3], params[4], rgb);
			} else if (params[1] == 2) {
				rgb[0] = PCT(clamp(params[2], 100));
				rgb[1] = PCT(clamp(params[3], 100));
				rgb[2] = PCT(clamp(params[4], 100));
			} else {
				break;
			}
			decoder->sink->define(decoder->ctx, decoder->reg,
			    rgb[0], rgb[1], rgb[2]);
		}
		break;
	case STATE_REPEAT:
		/* "!0" and "!" alone mean once, like xterm. */
		decoder->repeat = count > 0 && params[0] > 0 ?
		    clamp(params[0], 65535) : 1;
		break;
	}
	decoder->state = STATE_DATA;
	decoder->nparams = 0;
}

static void
paint(struct sixel_decoder *decoder, uint8_t bits)
{
	unsigned count = decoder->repeat != 0 ? decoder->repeat : 1;

	decoder->repeat = 0;
	if (bits != 0) {
		decoder->sink->paint(decoder->ctx, decoder->x, decoder->y,
		    bits, count, decoder->reg);
		if (decoder->y + 6 > decoder->height)
			decoder->height = decoder->y + 6;
	}
	decoder->x += count;
	if (bits != 0 && decoder->x > decoder->width)
		decoder->width = decoder->x;
}

void
sixel_feed(struct sixel_decoder *decoder, const char *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		uint8_t byte = (uint8_t)data[i];

		if (decoder->state != STATE_DATA) {
			if (byte >= '0' && byte <= '9') {
				if (decoder->nparams == 0)
					decoder->nparams = 1;
				uint32_t *param =
				    &decoder->params[decoder->nparams - 1];
				if (*param < 100000000)
					*param = *param * 10 + (byte - '0');
				continue;
			}
			if (byte == ';') {
				if (decoder->nparams == 0)
					decoder->nparams = 1;
				if (decoder->nparams < SIXEL_PARAMS)
					decoder->params[decoder->nparams++] =
					    0;
				continue;
			}
			finish_command(decoder);
		}

		if (byte >= '?' && byte <= '~') {
			paint(decoder, (uint8_t)(byte - '?'));
		} else if (byte == '"' || byte == '#' || byte == '!') {
			decoder->state = byte == '"' ? STATE_RASTER :
			    byte == '#' ? STATE_COLOUR : STATE_REPEAT;
			decoder->nparams = 0;
			memset(decoder->params, 0, sizeof(decoder->params));
		} else if (byte == '$') {
			decoder->x = 0;
		} else if (byte == '-') {
			decoder->x = 0;
			decoder->y += 6;
		}
		/* Anything else (CR, LF, spaces, junk): ignored. */
	}
}

void
sixel_end(struct sixel_decoder *decoder)
{
	if (decoder->state != STATE_DATA)
		finish_command(decoder);
}

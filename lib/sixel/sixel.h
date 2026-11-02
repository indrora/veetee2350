/*
 * sixel.h — a streaming DEC Sixel decoder.
 *
 * Sixel data is the body of "DCS P1 ; P2 ; P3 q <data> ST" (VT340, DEC
 * STD 070 chapter 9 "Sixel Graphics"). Each data character 0x3F-0x7E
 * carries six vertical pixels (bit 0 on top) at the current column of
 * the current six-pixel band. The rest is a tiny command language:
 *
 *   "Pan;Pad;Ph;Pv   raster attributes (aspect ratio, image size)
 *   #Pc              select colour register Pc
 *   #Pc;Pu;Px;Py;Pz  define register Pc: Pu=1 HLS (deg, %, %),
 *                    Pu=2 RGB (%, %, %)
 *   !Pn<char>        repeat the following sixel Pn times
 *   $                graphics CR: back to column 0 of the band
 *   -                graphics NL: column 0 of the next band (6 px down)
 *
 * The decoder keeps no image: it streams pixels out through a sink as it
 * parses, so an image of any size costs a few dozen bytes of state and
 * can arrive in as many fragments as the transport likes. Where the
 * pixels go -- and what an image is clipped or scrolled to -- is the
 * sink's business (lib/term/graphics.c). Pure C, no allocation, so the
 * host tests exercise it directly.
 */
#ifndef SIXEL_H
#define SIXEL_H

#include <stddef.h>
#include <stdint.h>

#define SIXEL_REGISTERS	256	/* colour registers (xterm has 1024) */
#define SIXEL_PARAMS	5

struct sixel_sink {
	/* Register `reg` is now this colour (0-255 per channel). */
	void	(*define)(void *ctx, unsigned reg, uint8_t red, uint8_t green,
		    uint8_t blue);
	/*
	 * Paint the set bits of `bits` (bit 0 = pixel row y) in colour
	 * register `reg`, in `count` consecutive columns starting at x.
	 */
	void	(*paint)(void *ctx, unsigned x, unsigned y, uint8_t bits,
		    unsigned count, unsigned reg);
	/* Raster attributes: pixel aspect pan:pad, declared size w x h. */
	void	(*raster)(void *ctx, unsigned pan, unsigned pad,
		    unsigned width, unsigned height);
};

struct sixel_decoder {
	const struct sixel_sink *sink;
	void		*ctx;
	uint8_t		 state;		/* enum in sixel.c */
	uint8_t		 nparams;
	uint32_t	 params[SIXEL_PARAMS];
	unsigned	 x;		/* current column, pixels */
	unsigned	 y;		/* top of the current band, pixels */
	unsigned	 reg;		/* current colour register */
	unsigned	 repeat;	/* pending !Pn count, 0 = none */
	unsigned	 width;		/* extent so far: max x painted + 1 */
	unsigned	 height;	/* bottom of the lowest band used */
};

/*
 * Start an image at pixel (0, 0) in register 0. The DCS parameters
 * (P1 aspect, P2 background select) are the sink's business.
 */
void	sixel_begin(struct sixel_decoder *decoder,
	    const struct sixel_sink *sink, void *ctx);
void	sixel_feed(struct sixel_decoder *decoder, const char *data,
	    size_t len);
/* Flush a pending command (e.g. "#1;2;0;0;100" right before ST). */
void	sixel_end(struct sixel_decoder *decoder);

/* DEC's HLS (hue 0 = blue, 120 = red, 240 = green) to 8-bit RGB. */
void	sixel_hls_to_rgb(unsigned hue, unsigned lightness,
	    unsigned saturation, uint8_t rgb[3]);

/* VT340 default colour registers 0-15, 8-bit RGB. */
extern const uint8_t sixel_vt340_palette[16][3];

#endif /* SIXEL_H */

/*
 * colour.h — colour-depth reduction for the emulation level's colour mode.
 *
 * Layering: pure helper for lib/term. A VT220-era terminal can't show
 * 24-bit colour, so when the colour depth is limited, SGR colours are
 * mapped to what such a terminal could display -- the way xterm itself
 * downgrades truecolor on a 256-colour build.
 */
#ifndef COLOUR_H
#define COLOUR_H

#include <stdint.h>

/*
 * Nearest xterm 256-colour index (16..255) for an RGB colour: the closer
 * of the 6x6x6 cube (levels 0,95,135,175,215,255) and the 24-step grey
 * ramp (8..238). Squared Euclidean distance in RGB.
 */
unsigned colour_nearest_256(uint8_t red, uint8_t green, uint8_t blue);

/* RGB of an xterm 256-colour cube/grey index (16..255). */
void	colour_256_rgb(unsigned index, uint8_t rgb[3]);

/* Nearest entry (0..15) of a 16-colour palette. */
unsigned colour_nearest_16(uint8_t red, uint8_t green, uint8_t blue,
	    const uint8_t palette[16][3]);

#endif /* COLOUR_H */

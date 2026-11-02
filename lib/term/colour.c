/*
 * colour.c — nearest-colour search for reduced colour depths.
 */
#include "colour.h"

static const uint8_t cube_levels[6] = {0, 95, 135, 175, 215, 255};

static uint32_t
distance(const uint8_t a[3], uint8_t red, uint8_t green, uint8_t blue)
{
	int dr = a[0] - red, dg = a[1] - green, db = a[2] - blue;

	return (uint32_t)(dr * dr + dg * dg + db * db);
}

/* Index (0..5) of the cube level nearest to `value`. */
static unsigned
nearest_level(uint8_t value)
{
	unsigned best = 0;

	for (unsigned i = 1; i < 6; i++) {
		int here = value - cube_levels[i];
		int prev = value - cube_levels[best];
		if (here * here < prev * prev)
			best = i;
	}
	return best;
}

void
colour_256_rgb(unsigned index, uint8_t rgb[3])
{
	if (index >= 232) {
		uint8_t grey = (uint8_t)(8 + 10 * (index - 232));
		rgb[0] = rgb[1] = rgb[2] = grey;
		return;
	}
	index -= 16;
	rgb[0] = cube_levels[index / 36];
	rgb[1] = cube_levels[(index / 6) % 6];
	rgb[2] = cube_levels[index % 6];
}

unsigned
colour_nearest_256(uint8_t red, uint8_t green, uint8_t blue)
{
	unsigned cube = 16 + 36 * nearest_level(red) + 6 * nearest_level(green) +
	    nearest_level(blue);

	/* Grey ramp candidate: 8 + 10k, k in 0..23. */
	int average = (red + green + blue) / 3;
	int step = (average - 8 + 5) / 10;
	if (step < 0)
		step = 0;
	if (step > 23)
		step = 23;
	unsigned grey = 232 + (unsigned)step;

	uint8_t cube_rgb[3], grey_rgb[3];
	colour_256_rgb(cube, cube_rgb);
	colour_256_rgb(grey, grey_rgb);
	return distance(grey_rgb, red, green, blue) <
	    distance(cube_rgb, red, green, blue) ? grey : cube;
}

unsigned
colour_nearest_16(uint8_t red, uint8_t green, uint8_t blue,
    const uint8_t palette[16][3])
{
	unsigned best = 0;
	uint32_t best_distance = UINT32_MAX;

	for (unsigned i = 0; i < 16; i++) {
		uint32_t here = distance(palette[i], red, green, blue);
		if (here < best_distance) {
			best_distance = here;
			best = i;
		}
	}
	return best;
}

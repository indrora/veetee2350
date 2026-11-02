/*
 * video_modes.h — the output resolutions VeeTee can drive.
 *
 * Pure data, shared by hal/video.c (which maps each mode to a libdvi
 * timing and a core voltage) and by SET-UP (which lists them). Index into
 * video_modes[] is what the settings store.
 *
 * Every mode runs the system clock at its TMDS bit clock (10 x pixel
 * clock). Budgets below are per line, from measured costs of ~6.8
 * cycles/pixel for TMDS encode (core 1) and ~5.7 for text render (core 0);
 * both must stay under the line period with margin:
 *
 *   mode            sysclk   line    encode  render
 *   640x480 @60     252 MHz   8000    ~4350   ~3650
 *   720x480 @60     270 MHz   8580    ~4900   ~4100
 *   800x480 @60     295 MHz   9920    ~5450   ~4550
 *   960x540 @60     372 MHz  11040    ~6500   ~5500
 *   1280x720 @30    372 MHz  16500    ~8700   ~7300
 *   1280x720 @24    297 MHz  16500    ~8700   ~7300
 *
 * 800x600 @60 (400 MHz) is left out: beyond what the RP2350 sensibly
 * overclocks to. 1280x720 @30 is PicoDVI's nonstandard half-rate 720p
 * (37.125 MHz pixel clock); sinks that only take CEA 720p60 won't lock.
 * 1280x720 @24 is the same frame at 29.7 MHz: the per-line budget is
 * identical (it depends only on the line length), but sysclk drops to
 * 297 MHz at 1.2 V -- a much gentler overclock, for anyone happy with 24
 * frames a second on a text terminal.
 */
#ifndef VIDEO_MODES_H
#define VIDEO_MODES_H

#include <stdint.h>

struct video_mode {
	const char	*name;
	uint16_t	 width;
	uint16_t	 height;
	uint8_t		 refresh_hz;
};

enum video_mode_id {
	VIDEO_MODE_640x480,
	VIDEO_MODE_720x480,
	VIDEO_MODE_800x480,
	VIDEO_MODE_960x540,
	VIDEO_MODE_1280x720,
	VIDEO_MODE_1280x720_24,
	VIDEO_MODE_COUNT
};

#define VIDEO_MAX_WIDTH		1280
#define VIDEO_MAX_HEIGHT	720

static const struct video_mode video_modes[VIDEO_MODE_COUNT] = {
	[VIDEO_MODE_640x480] = {"640x480 @ 60 Hz", 640, 480, 60},
	[VIDEO_MODE_720x480] = {"720x480 @ 60 Hz", 720, 480, 60},
	[VIDEO_MODE_800x480] = {"800x480 @ 60 Hz", 800, 480, 60},
	[VIDEO_MODE_960x540] = {"960x540 @ 60 Hz", 960, 540, 60},
	[VIDEO_MODE_1280x720] = {"1280x720 @ 30 Hz", 1280, 720, 30},
	[VIDEO_MODE_1280x720_24] = {"1280x720 @ 24 Hz (low clock)", 1280, 720,
	    24},
};

#endif /* VIDEO_MODES_H */

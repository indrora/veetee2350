/*
 * button.h — one push button's tap and long hold (BOOTSEL).
 *
 * Layering: pure module (host-testable), beside keys.c and hidkbd.c. The
 * caller samples the button however it can (app.c reads BOOTSEL every 50
 * ms, because each read takes the flash offline for a few microseconds)
 * and hands each sample here with the time; this says what it means.
 *
 * Why a hold at all: BOOTSEL is the one input every VeeTee has, keyboard
 * or not. A tap opens SET-UP, which needs a keyboard to do anything in;
 * a long hold resets every setting to factory defaults, which doesn't.
 * That's the way out of a setting you can't undo without a keyboard
 * (devlog 028), or of a mode the monitor can't show: it works blind.
 *
 * The tap fires when the button is let go, not when it's pressed: a
 * press can't yet tell a tap from the start of a hold, and a hold must
 * not do the tap's thing first.
 *
 *   down < hint_ms, let go        BUTTON_TAP
 *   down >= hint_ms               BUTTON_HOLDING on every sample, with
 *                                 the time left until fire_ms
 *   let go in hint_ms..fire_ms    BUTTON_CANCELLED
 *   down >= fire_ms               BUTTON_FIRED, once; then nothing until
 *                                 it's let go (and that's no tap)
 *
 * Times are ms and may wrap (uint32_t arithmetic throughout).
 */
#ifndef BUTTON_H
#define BUTTON_H

#include <stdbool.h>
#include <stdint.h>

enum button_event {
	BUTTON_NONE,
	BUTTON_TAP,
	BUTTON_HOLDING,
	BUTTON_CANCELLED,
	BUTTON_FIRED,
};

struct button {
	uint32_t	 hint_ms;	/* down this long: a hold, not a tap */
	uint32_t	 fire_ms;	/* down this long: the hold fires */
	bool		 down;		/* at the last sample */
	bool		 fired;		/* this press already fired */
	uint32_t	 down_at;	/* when this press began */
};

void	button_init(struct button *button, uint32_t hint_ms, uint32_t fire_ms);

/*
 * One sample: is the button down now? For BUTTON_HOLDING, `left_ms` (may
 * be null) gets the time until it fires.
 */
enum button_event button_sample(struct button *button, bool pressed,
	    uint32_t now_ms, uint32_t *left_ms);

#endif /* BUTTON_H */

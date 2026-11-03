/*
 * button.c — one push button's tap and long hold (see button.h).
 */
#include "button.h"

#include <string.h>

void
button_init(struct button *button, uint32_t hint_ms, uint32_t fire_ms)
{
	memset(button, 0, sizeof(*button));
	button->hint_ms = hint_ms;
	button->fire_ms = fire_ms;
}

enum button_event
button_sample(struct button *button, bool pressed, uint32_t now_ms,
    uint32_t *left_ms)
{
	if (!pressed) {
		if (!button->down)
			return BUTTON_NONE;
		/* Let go: what the press was depends on how long it lasted. */
		button->down = false;
		if (button->fired)
			return BUTTON_NONE;
		if (now_ms - button->down_at < button->hint_ms)
			return BUTTON_TAP;
		return BUTTON_CANCELLED;
	}
	if (!button->down) {
		/* Pressed. Nothing to say yet: it may be a tap or a hold. */
		button->down = true;
		button->fired = false;
		button->down_at = now_ms;
		return BUTTON_NONE;
	}
	if (button->fired)
		return BUTTON_NONE;
	uint32_t held = now_ms - button->down_at;
	if (held >= button->fire_ms) {
		button->fired = true;
		return BUTTON_FIRED;
	}
	if (held < button->hint_ms)
		return BUTTON_NONE;
	if (left_ms != nullptr)
		*left_ms = button->fire_ms - held;
	return BUTTON_HOLDING;
}

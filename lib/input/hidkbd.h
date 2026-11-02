/*
 * hidkbd.h — USB keyboard reports to key events.
 *
 * Layering: pure module (host-testable), beside keys.c. keys.c reads
 * what a terminal on the Mac sends down the keyboard CDC port; this
 * reads a USB keyboard on the board's own host port (lib/hal/usbhost.h)
 * and makes the same struct key_event, so hotkeys, the Tools menu,
 * SET-UP and replay take either without knowing which. What a key sends
 * to the host is then term.c's job (term_key_bytes): there is no `raw`
 * here, because with a USB keyboard the VeeTee is the terminal and its
 * modes (cursor keys, keypad, new line) decide the bytes.
 *
 * Reports are HID boot protocol: a modifier byte, a reserved byte, and
 * up to six key usages. A key is pressed when its usage appears in a
 * report it wasn't in, so a report repeated unchanged (this board's
 * PIO-USB keeps delivering the last one after an unplug) does nothing.
 *
 * Layout: US. Caps Lock and Num Lock toggle here (and their LEDs, via
 * hidkbd_leds); Num Lock starts on. Auto-repeat, which a USB keyboard
 * leaves to the host: the last key pressed repeats after
 * HIDKBD_REPEAT_DELAY_MS, every HIDKBD_REPEAT_MS, while it's held.
 */
#ifndef HIDKBD_H
#define HIDKBD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "keys.h"

#define HIDKBD_REPEAT_DELAY_MS	500
#define HIDKBD_REPEAT_MS	33	/* 30 a second, as a VT220 */
#define HIDKBD_KEYS		6	/* usages in a boot report */

/* HID keyboard LED bits (an output report's byte). */
#define HIDKBD_LED_NUM		0x01
#define HIDKBD_LED_CAPS		0x02
#define HIDKBD_LED_SCROLL	0x04

struct hidkbd {
	uint8_t		 held[HIDKBD_KEYS];	/* usages down, last report */
	uint8_t		 modifiers;		/* HID modifier byte */
	bool		 caps_lock;
	bool		 num_lock;
	bool		 scroll_lock;
	uint8_t		 repeat_usage;		/* 0: nothing repeating */
	uint32_t	 repeat_at;		/* ms */
};

void	hidkbd_init(struct hidkbd *kbd);

/*
 * A report arrived: the keys newly pressed in it, as events, in report
 * order (up to `max`; returns how many).
 */
unsigned hidkbd_report(struct hidkbd *kbd, const uint8_t *report,
	    size_t len, uint32_t now_ms, struct key_event *out, unsigned max);

/* Main loop: the held key's next repeat, if it's due. */
bool	hidkbd_poll(struct hidkbd *kbd, uint32_t now_ms,
	    struct key_event *out);

/* The LEDs the lock keys call for (HIDKBD_LED_*). */
uint8_t	hidkbd_leds(const struct hidkbd *kbd);

#endif /* HIDKBD_H */

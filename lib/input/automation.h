/*
 * automation.h — reports from the automation HID, as key events.
 *
 * Layering: pure module (host-testable), beside hidkbd.c and keys.c,
 * which it reuses: a script's keyboard reports go through a hidkbd of
 * their own (so they don't disturb a real keyboard's held keys), its
 * typed bytes through a keys decoder. The report layout is in
 * include/automation_hid.h; src/main.cpp moves the reports. Debug builds
 * only (app.c), but nothing here depends on that.
 */
#ifndef AUTOMATION_H
#define AUTOMATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hidkbd.h"
#include "keys.h"

struct automation {
	struct hidkbd	 keyboard;	/* AUTOMATION_KEYBOARD reports */
	struct keys	 typed;		/* AUTOMATION_TYPE bytes */
	bool		 bootsel;	/* AUTOMATION_BOOTSEL: held down */
};

void	automation_init(struct automation *automation);

/*
 * One OUT report: the key events in it, in order (up to `max`; returns
 * how many). A BOOTSEL report sets `bootsel` and has none; an unknown
 * type has none.
 */
unsigned automation_report(struct automation *automation,
	    const uint8_t *report, size_t len, uint32_t now_ms,
	    struct key_event *out, unsigned max);

/*
 * Main loop: what time alone produces, the keyboard's auto-repeat and
 * the typed decoder's lone Esc (which only a pause can tell from the
 * start of a sequence). Up to two events; returns how many.
 */
unsigned automation_poll(struct automation *automation, uint32_t now_ms,
	    struct key_event out[2]);

#endif /* AUTOMATION_H */

/*
 * automation.c — reports from the automation HID (see automation.h).
 */
#include "automation.h"

#include "automation_hid.h"

void
automation_init(struct automation *automation)
{
	hidkbd_init(&automation->keyboard);
	keys_init(&automation->typed);
	automation->bootsel = false;
}

unsigned
automation_report(struct automation *automation, const uint8_t *report,
    size_t len, uint32_t now_ms, struct key_event *out, unsigned max)
{
	unsigned count = 0;

	if (len < 2)
		return 0;
	switch (report[0]) {
	case AUTOMATION_KEYBOARD:
		return hidkbd_report(&automation->keyboard, report + 1,
		    len - 1 < 8 ? len - 1 : 8, now_ms, out, max);
	case AUTOMATION_TYPE: {
		size_t bytes = report[1];
		if (bytes > len - 2)
			bytes = len - 2;	/* a lying length: what's there */
		for (size_t i = 0; i < bytes && count < max; i++)
			if (keys_feed(&automation->typed, report[2 + i], now_ms,
			    &out[count]))
				count++;
		return count;
	}
	case AUTOMATION_BOOTSEL:
		automation->bootsel = report[1] != 0;
		return 0;
	default:
		return 0;
	}
}

unsigned
automation_poll(struct automation *automation, uint32_t now_ms,
    struct key_event out[2])
{
	unsigned count = 0;

	if (hidkbd_poll(&automation->keyboard, now_ms, &out[count]))
		count++;
	if (keys_poll(&automation->typed, now_ms, &out[count]))
		count++;
	return count;
}

/*
 * usbio.c — printf front end for debug text (see usbio.h).
 */
#include "usbio.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "automation_hid.h"
#include "build.h"

#if VEETEE_DEBUG_BUILD
/*
 * Debug text up the automation HID, AUTOMATION_TEXT_MAX bytes a report.
 * A line may span reports; the tool joins them. When the queue is full
 * (nobody reading, or a burst), the rest is dropped, never waited for:
 * this is called from the render path's statistics and from key
 * handling, which mustn't stall on a host that isn't listening.
 */
static void
log_text(const char *text, size_t len)
{
	uint8_t report[AUTOMATION_REPORT_SIZE];

	while (len > 0) {
		size_t chunk = len < AUTOMATION_TEXT_MAX ? len :
		    AUTOMATION_TEXT_MAX;
		memset(report, 0, sizeof(report));
		report[0] = AUTOMATION_LOG;
		report[1] = (uint8_t)chunk;
		memcpy(report + 2, text, chunk);
		if (!usbio_automation_write(report))
			return;
		text += chunk;
		len -= chunk;
	}
}
#endif

void
usbio_printf(const char *fmt, ...)
{
	char buf[160];
	va_list args;
	int len;

	va_start(args, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	if (len < 0)
		return;
	if ((size_t)len >= sizeof(buf))
		len = sizeof(buf) - 1;	/* truncated; still print what fits */
#if VEETEE_DEBUG_BUILD
	log_text(buf, (size_t)len);
#else
	usbio_write(USBIO_LINE, buf, (size_t)len);
#endif
}

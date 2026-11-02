/*
 * main.cpp — the only C++ file in the project (STYLE.md §1).
 *
 * arduino-pico insists on setup()/loop() (declared with C linkage by the
 * core); this shim forwards them to the C application in app.c so the rest
 * of the codebase stays pure C. It also provides console_write(), the byte
 * sink behind hal/console.h, since Serial is a C++ object.
 */
#include <Arduino.h>

extern "C" {
#include "app.h"
#include "console.h"

void
console_write(const char *buf, size_t len)
{
	Serial.write(reinterpret_cast<const uint8_t *>(buf), len);
}

size_t
console_read(char *buf, size_t cap)
{
	size_t avail = (size_t)Serial.available();

	if (avail == 0)
		return 0;
	if (avail > cap)
		avail = cap;
	return Serial.readBytes(buf, avail);
}

void
setup(void)
{
	Serial.begin(115200);
	app_init();
}

void
loop(void)
{
	app_poll();
}
}

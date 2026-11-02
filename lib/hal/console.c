/*
 * console.c — printf-style front end for console_write().
 */
#include "console.h"

#include <stdarg.h>
#include <stdio.h>

void
console_printf(const char *fmt, ...)
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
	console_write(buf, (size_t)len);
}

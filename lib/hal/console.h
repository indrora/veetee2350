/*
 * console.h — debug text output over the USB CDC "Serial" port.
 *
 * Goal 1 temporary: once usbio owns the CDC interfaces this becomes a
 * dedicated debug channel. The byte sink (console_write) is implemented
 * in src/main.cpp, because arduino-pico's Serial object is C++.
 */
#ifndef HAL_CONSOLE_H
#define HAL_CONSOLE_H

#include <stddef.h>

void	console_write(const char *buf, size_t len);
void	console_printf(const char *fmt, ...)
	    __attribute__((format(printf, 1, 2)));

#endif /* HAL_CONSOLE_H */

/*
 * usbio.h — the two USB CDC serial ports the VeeTee presents to its host.
 *
 *   USBIO_LINE      the terminal's communication line ("VeeTee line"): a
 *                   program on the host writes here and reads the
 *                   terminal's replies and keystrokes back.
 *   USBIO_KEYBOARD  a stand-in keyboard ("VeeTee keyboard"): bytes written
 *                   here are treated as keys typed on the terminal (Goal 1
 *                   "loopback"); later a USB HID keyboard feeds the same
 *                   path.
 *
 * Implemented in src/main.cpp because the CDC objects (Adafruit TinyUSB)
 * are C++. Both calls are non-blocking.
 */
#ifndef HAL_USBIO_H
#define HAL_USBIO_H

#include <stddef.h>

enum usbio_port {
	USBIO_LINE,
	USBIO_KEYBOARD,
};

/* Copy up to `cap` received bytes into buf; returns the count. */
size_t	usbio_read(enum usbio_port port, char *buf, size_t cap);
/* Queue bytes for the host; drops them if the port isn't open. */
void	usbio_write(enum usbio_port port, const char *buf, size_t len);
/* printf to the line port (debug/stats builds only). */
void	usbio_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* HAL_USBIO_H */

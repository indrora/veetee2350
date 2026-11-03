/*
 * usbio.h — what the VeeTee presents to its host over the native USB.
 *
 *   USBIO_LINE   the terminal's communication line ("VeeTee line"), a CDC
 *                serial port: a program on the host writes here and reads
 *                the terminal's replies and keystrokes back.
 *
 * Debug builds (include/build.h) add the automation HID ("VeeTee
 * automation", include/automation_hid.h): a script on the host types,
 * presses keys and BOOTSEL through it, and reads the debug text back.
 * It took the place of a second CDC port, "VeeTee keyboard", which stood
 * in for a keyboard until the board got a USB keyboard of its own
 * (devlog 029); release builds have the line alone.
 *
 * Implemented in src/main.cpp because the USB objects (Adafruit TinyUSB)
 * are C++. Every call is non-blocking.
 */
#ifndef HAL_USBIO_H
#define HAL_USBIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum usbio_port {
	USBIO_LINE,
};

/* Copy up to `cap` received bytes into buf; returns the count. */
size_t	usbio_read(enum usbio_port port, char *buf, size_t cap);
/* Queue bytes for the host; drops them if the port isn't open. */
void	usbio_write(enum usbio_port port, const char *buf, size_t len);
/*
 * Debug text. Debug builds send it up the automation HID (tools/
 * sendkeys.py --log shows it), keeping it out of the terminal session;
 * the flash-probe build, which has no HID, writes it to the line.
 */
void	usbio_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/*
 * The automation HID (debug builds only; not defined otherwise). Reports
 * are AUTOMATION_REPORT_SIZE bytes. usbio_automation_read takes the next
 * report the host sent, if any (they queue: the USB stack runs in an
 * interrupt here). usbio_automation_write queues one for the host, false
 * if the queue was full and it was dropped; usbio_poll, from the main
 * loop, sends what's queued as the host takes it.
 */
bool	usbio_automation_read(uint8_t *report);
bool	usbio_automation_write(const uint8_t *report);
void	usbio_poll(void);

#endif /* HAL_USBIO_H */

/*
 * usbhost.h — the board's second USB port, as a host: keyboards.
 *
 * Wiring (Waveshare RP2350-PiZero "PIO-USB" port): D+ GPIO 28, D- GPIO
 * 29, VBUS straight from 5 V (no switch). There's no USB host controller
 * there; Pico-PIO-USB (sekigon-gonnoc, MIT, fetched by PlatformIO) does
 * USB full/low speed in software on PIO 1, under TinyUSB's host stack
 * (root port 1; the native port 0 stays our CDC device).
 *
 * Timing is the hard part. PIO-USB runs each 1 ms USB frame from a timer
 * interrupt and answers handshakes from the CPU within microseconds, so
 * nothing may interrupt it mid-packet: its interrupt is the only one on
 * core 0 above the renderer's doorbell (video.c), and the 8-line ring of
 * rendered lines rides out the wait. It also needs the system clock to
 * be a multiple of 12 MHz (it divides it down to the USB bit rate): true
 * of 252 and 372 MHz, not of the 270, 295, 297 or 315 MHz modes, where
 * the port stays off and says why.
 *
 * Known on this board (another project's long soak): an unplugged device
 * isn't noticed, and its last report keeps arriving; keyboards are only
 * read for changes, so that's harmless here.
 *
 * Core 0 only: usbhost_init after video_init, usbhost_poll from the main
 * loop (TinyUSB's callbacks, and so `report`, run inside it).
 */
#ifndef HAL_USBHOST_H
#define HAL_USBHOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A boot-protocol keyboard report (8 bytes: modifiers, 0, 6 usages). */
typedef void (*usbhost_report_fn)(const uint8_t *report, size_t len,
    void *ctx);
/* A keyboard came or went; `text` says which ("046d:c31c attached"). */
typedef void (*usbhost_event_fn)(const char *text, void *ctx);

bool	usbhost_init(usbhost_report_fn report, usbhost_event_fn event,
	    void *ctx);
void	usbhost_poll(void);

/* Keyboard LEDs (HID: bit 0 Num, 1 Caps, 2 Scroll) on every keyboard. */
void	usbhost_set_leds(uint8_t leds);

/* "off: clock 315 MHz" / "on, 1 keyboard" -- for the boot banner. */
const char *usbhost_describe(void);

#endif /* HAL_USBHOST_H */

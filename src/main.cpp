/*
 * main.cpp — the only C++ file in the project (STYLE.md §1).
 *
 * arduino-pico insists on setup()/loop() (declared with C linkage by the
 * core); this shim forwards them to the C application in app.c so the rest
 * of the codebase stays pure C. It also implements the C-facing pieces
 * whose Arduino objects are C++: the two USB CDC ports (hal/usbio.h) and
 * the BOOTSEL button.
 *
 * USB: built with USE_TINYUSB (Adafruit TinyUSB), because arduino-pico's
 * native stack is compiled for a single CDC interface. `Serial` is CDC 0
 * (the line); keyboard_port is CDC 1. Adafruit TinyUSB still honours the
 * 1200-baud "touch" on CDC 0, so `pio run -t upload` keeps working.
 */
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

extern "C" {
#include "app.h"
#include "usbio.h"
#include "board.h"
}

static Adafruit_USBD_CDC keyboard_port;

static Stream &
port_stream(enum usbio_port port)
{
	if (port == USBIO_KEYBOARD)
		return keyboard_port;
	return Serial;
}

extern "C" size_t
usbio_read(enum usbio_port port, char *buf, size_t cap)
{
	Stream &stream = port_stream(port);
	size_t avail = (size_t)stream.available();

	if (avail == 0)
		return 0;
	if (avail > cap)
		avail = cap;
	return stream.readBytes(buf, avail);
}

extern "C" void
usbio_write(enum usbio_port port, const char *buf, size_t len)
{
	port_stream(port).write(reinterpret_cast<const uint8_t *>(buf), len);
}

extern "C" bool
board_bootsel_pressed(void)
{
	/*
	 * Briefly floats the flash chip select with interrupts off; safe for
	 * core 1 only once video_flash_safe() (checked by the caller).
	 * rp2040.idleOtherCore() inside is a no-op here: we never use
	 * arduino-pico's own core 1 (no setup1/loop1).
	 */
	return BOOTSEL;
}

extern "C" void
setup(void)
{
	Serial.setStringDescriptor("VeeTee line");
	Serial.begin(115200);
	keyboard_port.setStringDescriptor("VeeTee keyboard");
	keyboard_port.begin(115200);
	/* Interfaces added after enumeration need a re-enumeration. */
	if (TinyUSBDevice.mounted()) {
		TinyUSBDevice.detach();
		delay(10);
		TinyUSBDevice.attach();
	}
	app_init();
}

extern "C" void
loop(void)
{
	app_poll();
}

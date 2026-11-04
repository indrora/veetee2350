/*
 * main.cpp — the only C++ file in the project (STYLE.md §1).
 *
 * arduino-pico insists on setup()/loop() (declared with C linkage by the
 * core); this shim forwards them to the C application in app.c so the rest
 * of the codebase stays pure C. It also implements the C-facing pieces
 * whose Arduino objects are C++: the native USB's line port and, in debug
 * builds, its automation HID (hal/usbio.h), and the BOOTSEL button.
 *
 * USB: built with USE_TINYUSB (Adafruit TinyUSB), because arduino-pico's
 * native stack can't add interfaces. `Serial` is the CDC line port.
 * Adafruit TinyUSB still honours the 1200-baud "touch" on it, so `pio run
 * -t upload` keeps working.
 */
#include <Arduino.h>
#include <Adafruit_TinyUSB.h>

#include "pico/mutex.h"
#include "hardware/sync.h"

extern "C" {
#include "app.h"
#include "automation_hid.h"
#include "board.h"
#include "build.h"
#include "usbio.h"
}

extern "C" size_t
usbio_read(enum usbio_port port, char *buf, size_t cap)
{
	(void)port;			/* the line is the only port */
	size_t avail = (size_t)Serial.available();

	if (avail == 0)
		return 0;
	if (avail > cap)
		avail = cap;
	return Serial.readBytes(buf, avail);
}

extern "C" size_t
usbio_available(enum usbio_port port)
{
	(void)port;
	return (size_t)Serial.available();
}

extern "C" void
usbio_write(enum usbio_port port, const char *buf, size_t len)
{
	(void)port;
	Serial.write(reinterpret_cast<const uint8_t *>(buf), len);
}

#if VEETEE_DEBUG_BUILD
/*
 * The automation HID (include/automation_hid.h): vendor-defined, one
 * 64-byte report each way, no report IDs, an interrupt OUT endpoint so
 * the host's writes don't go through control transfers. It replaced the
 * "VeeTee keyboard" CDC port (devlog 029).
 *
 * On this port TinyUSB's device task runs in an interrupt (usb_task_irq
 * in Adafruit_TinyUSB_rp2040.cpp), so OUT reports arrive in interrupt
 * context. They're only copied into `out_ring` there; the main loop
 * takes them (usbio_automation_read). One producer, one consumer, so the
 * indices need nothing more than a barrier between the copy and the
 * publish. TinyUSB re-arms the endpoint as soon as the callback returns,
 * so there's no holding the host off: a full ring drops the report and
 * counts it, and the next read says so in the log. The host tool paces
 * itself well inside 32 reports.
 */
static const uint8_t automation_desc[] = {
	TUD_HID_REPORT_DESC_GENERIC_INOUT(AUTOMATION_REPORT_SIZE)
};
static Adafruit_USBD_HID automation_hid(automation_desc,
    sizeof(automation_desc), HID_ITF_PROTOCOL_NONE, 1, true);

#define OUT_SLOTS	32		/* host -> VeeTee: 2 KB */
#define IN_SLOTS	64		/* VeeTee -> host (debug text): 4 KB */
static uint8_t out_ring[OUT_SLOTS][AUTOMATION_REPORT_SIZE];
static volatile uint32_t out_head;	/* written by the interrupt */
static volatile uint32_t out_tail;	/* written by the main loop */
static volatile uint32_t out_dropped;
static uint32_t out_dropped_told;
static uint8_t in_ring[IN_SLOTS][AUTOMATION_REPORT_SIZE];
static uint32_t in_head, in_tail;	/* main loop only */

/* TinyUSB's own lock: its interrupt skips a tick while we hold it. */
extern mutex_t __usb_mutex;

static void
automation_received(uint8_t report_id, hid_report_type_t type,
    const uint8_t *buffer, uint16_t len)
{
	(void)report_id;
	(void)type;
	uint32_t head = out_head;

	if (head - out_tail >= OUT_SLOTS) {
		out_dropped = out_dropped + 1;
		return;
	}
	uint8_t *slot = out_ring[head % OUT_SLOTS];
	if (len > AUTOMATION_REPORT_SIZE)
		len = AUTOMATION_REPORT_SIZE;
	memset(slot, 0, AUTOMATION_REPORT_SIZE);
	memcpy(slot, buffer, len);
	__dmb();			/* the copy lands before the publish */
	out_head = head + 1;
}

extern "C" bool
usbio_automation_read(uint8_t *report)
{
	uint32_t tail = out_tail;

	if (out_dropped != out_dropped_told) {
		out_dropped_told = out_dropped;
		usbio_printf("[auto] %lu reports dropped: send slower\r\n",
		    (unsigned long)out_dropped_told);
	}
	if (tail == out_head)
		return false;
	__dmb();			/* see the copy the publish covers */
	memcpy(report, out_ring[tail % OUT_SLOTS], AUTOMATION_REPORT_SIZE);
	out_tail = tail + 1;
	return true;
}

extern "C" void
usbio_poll(void)
{
	while (in_tail != in_head) {
		bool sent = false;

		mutex_enter_blocking(&__usb_mutex);
		if (TinyUSBDevice.mounted() && automation_hid.ready())
			sent = automation_hid.sendReport(0,
			    in_ring[in_tail % IN_SLOTS], AUTOMATION_REPORT_SIZE);
		mutex_exit(&__usb_mutex);
		if (!sent)
			return;		/* the host takes one a millisecond */
		in_tail++;
	}
}

extern "C" bool
usbio_automation_write(const uint8_t *report)
{
	if (in_head - in_tail >= IN_SLOTS) {
		usbio_poll();		/* make room if the host has taken one */
		if (in_head - in_tail >= IN_SLOTS)
			return false;
	}
	memcpy(in_ring[in_head % IN_SLOTS], report, AUTOMATION_REPORT_SIZE);
	in_head++;
	usbio_poll();
	return true;
}
#endif /* VEETEE_DEBUG_BUILD */

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
#if VEETEE_DEBUG_BUILD
	automation_hid.setStringDescriptor("VeeTee automation");
	automation_hid.setReportCallback(nullptr, automation_received);
	automation_hid.begin();
#endif
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

/*
 * usbhost.c — keyboards on the PIO-USB port (see usbhost.h).
 *
 * TinyUSB's host stack (Adafruit TinyUSB, already here for the CDC
 * device) with Pico-PIO-USB as its controller. Including pio_usb.h is
 * what switches that on: tusb_config_rp2040.h enables the PIO-USB host
 * when the header can be found.
 *
 * PIO and DMA: PIO 0 is the DVI output (rebased to GPIO 16-47), so
 * PIO-USB transmits on PIO 1 and receives (with its end-of-packet
 * detector) on PIO 2; both default windows (GPIO 0-31) cover 28 and 29.
 * Not one block for both: the two programs each use PIO interrupt flag 1
 * for something different, and in one block they trip each other
 * (every SETUP failed; lib/pico_pio_usb/PATCHES.md, patch 3). The
 * transmit DMA channel is fixed in its config (channel 0 by default,
 * which libdvi may have claimed), so it gets one we know is free.
 */
#include "usbhost.h"

#include <stdio.h>
#include <string.h>

#if !__has_include("pio_usb.h")
/*
 * Built without Pico-PIO-USB (it's fetched by PlatformIO's lib_deps):
 * the port stays off and says so, and everything else still builds.
 */
static const char missing[] = "off: no Pico-PIO-USB in this build";

bool
usbhost_init(usbhost_report_fn report, usbhost_event_fn event, void *ctx)
{
	(void)report;
	(void)event;
	(void)ctx;
	return false;
}

void
usbhost_poll(void)
{
}

void
usbhost_set_leds(uint8_t leds)
{
	(void)leds;
}

const char *
usbhost_describe(void)
{
	return missing;
}

const char *
usbhost_refresh(void)
{
	return missing;
}

#else /* Pico-PIO-USB is here */

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/timer.h"
#include "pico/time.h"
#include "build.h"
#include "pio_usb.h"
#include "pio_usb_ll.h"
#include "tusb.h"
#include "usbio.h"

#define USBHOST_PIN_DP		28
#define USBHOST_RHPORT		1	/* TinyUSB: PIO-USB is root port 1 */
#define USBHOST_KEYBOARDS	4

static usbhost_report_fn report_fn;
static usbhost_event_fn event_fn;
static void *callback_ctx;
static bool running;
static char state[112] = "off";

static struct {
	bool	 used;
	uint8_t	 dev;
	uint8_t	 instance;
} keyboards[USBHOST_KEYBOARDS];
static uint8_t leds;
/* Any device at all, and HID interfaces of any kind: for describe(). */
static unsigned devices, hid_interfaces;
static uint16_t last_vid, last_pid;
static uint8_t last_protocol = 0xff;

static unsigned
keyboard_count(void)
{
	unsigned count = 0;

	for (unsigned i = 0; i < USBHOST_KEYBOARDS; i++)
		count += keyboards[i].used;
	return count;
}

static void
describe(void)
{
	if (running && devices == 0 && !VEETEE_DEBUG_BUILD) {
		snprintf(state, sizeof(state), "on, no keyboard yet");
		return;
	}
	if (running && devices == 0) {
		/* Nothing enumerated: what the root port itself sees. */
		root_port_t *root = PIO_USB_ROOT_PORT(0);
		const pio_usb_veetee_stats_t *bus = &pio_usb_veetee_stats;
		snprintf(state, sizeof(state), "on, no device: %s%s;"
		    " conn %lu rst %lu ok %lu err %lu stall %lu nak %lu"
		    " noreply %lu",
		    root->connected ? "connected" : "nothing",
		    root->connected ? (root->is_fullspeed ? " FS" : " LS") :
		    "", (unsigned long)bus->connects,
		    (unsigned long)bus->resets, (unsigned long)bus->completes,
		    (unsigned long)bus->errors, (unsigned long)bus->stalls,
		    (unsigned long)bus->naks, (unsigned long)bus->no_reply);
		return;
	}
	if (running && !VEETEE_DEBUG_BUILD)
		snprintf(state, sizeof(state), "on, %u keyboard%s",
		    keyboard_count(), keyboard_count() == 1 ? "" : "s");
	else if (running)
		snprintf(state, sizeof(state), "on, %u keyboard%s; %u device%s"
		    " (last %04x:%04x), %u HID (last protocol %u)",
		    keyboard_count(), keyboard_count() == 1 ? "" : "s", devices,
		    devices == 1 ? "" : "s", last_vid, last_pid, hid_interfaces,
		    last_protocol);
}

bool
usbhost_init(usbhost_report_fn report, usbhost_event_fn event, void *ctx)
{
	static pio_usb_configuration_t config = PIO_USB_DEFAULT_CONFIG;
	uint32_t hz = clock_get_hz(clk_sys);

	report_fn = report;
	event_fn = event;
	callback_ctx = ctx;
	if (hz % 12000000u != 0) {
		snprintf(state, sizeof(state), "off: %lu MHz isn't 12 x n",
		    (unsigned long)(hz / 1000000u));
		return false;
	}
	config.pin_dp = USBHOST_PIN_DP;
	config.pio_tx_num = 1;
	config.sm_tx = 0;
	config.pio_rx_num = 2;	/* not 1: see the PIO note at the top */
	config.sm_rx = 0;
	config.sm_eop = 1;
	/* A free channel: claimed to find it, released for PIO-USB to
	 * claim (it panics if it's taken). */
	int channel = dma_claim_unused_channel(true);
	dma_channel_unclaim((uint)channel);
	config.tx_ch = (uint8_t)channel;
	/* Its own hardware alarm, so we know the interrupt to raise. */
	alarm_pool_t *pool = alarm_pool_create_with_unused_hardware_alarm(2);
	config.alarm_pool = pool;

	tuh_configure(USBHOST_RHPORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION,
	    &config);
	if (!tuh_init(USBHOST_RHPORT)) {
		snprintf(state, sizeof(state), "off: host stack failed");
		return false;
	}
	/*
	 * Above the renderer's doorbell (video.c): a USB transaction must
	 * not be cut off mid-packet, and a few lines' wait is what the ring
	 * of rendered lines is for.
	 */
	irq_set_priority(hardware_alarm_get_irq_num(
	    alarm_pool_hardware_alarm_num(pool)), PICO_HIGHEST_IRQ_PRIORITY);
	running = true;
	describe();
	return true;
}

void
usbhost_poll(void)
{
	if (running)
		tuh_task();
}

void
usbhost_set_leds(uint8_t new_leds)
{
	static uint8_t report;	/* must outlive the control transfer */

	leds = new_leds;
	report = leds;
	for (unsigned i = 0; i < USBHOST_KEYBOARDS; i++)
		if (keyboards[i].used)
			(void)tuh_hid_set_report(keyboards[i].dev,
			    keyboards[i].instance, 0, HID_REPORT_TYPE_OUTPUT,
			    &report, sizeof(report));
}

const char *
usbhost_describe(void)
{
	return state;
}

const char *
usbhost_refresh(void)
{
	describe();
	return state;
}

/* --- TinyUSB host callbacks (from tuh_task, so in usbhost_poll) ------- */

static void
announce(uint8_t dev, const char *what)
{
	uint16_t vid = 0, pid = 0;
	char text[48];

	(void)tuh_vid_pid_get(dev, &vid, &pid);
	snprintf(text, sizeof(text), "USB keyboard %04x:%04x %s", vid, pid,
	    what);
	describe();
	if (event_fn != nullptr)
		event_fn(text, callback_ctx);
}

/* Any device: counted, so "no keyboard" can be told from "nothing". */
void
tuh_mount_cb(uint8_t dev)
{
	devices++;
	(void)tuh_vid_pid_get(dev, &last_vid, &last_pid);
	describe();
}

void
tuh_umount_cb(uint8_t dev)
{
	(void)dev;
	if (devices > 0)
		devices--;
	describe();
}

void
tuh_hid_mount_cb(uint8_t dev, uint8_t instance, uint8_t const *desc,
    uint16_t desc_len)
{
	(void)desc;
	(void)desc_len;
	hid_interfaces++;
	last_protocol = tuh_hid_interface_protocol(dev, instance);
	describe();
	if (last_protocol != HID_ITF_PROTOCOL_KEYBOARD) {
		/* Mice, media keys: not yet. Debug builds read them anyway
		 * and print what they send (tuh_hid_report_received_cb). */
		if (VEETEE_DEBUG_BUILD)
			(void)tuh_hid_receive_report(dev, instance);
		return;
	}
	for (unsigned i = 0; i < USBHOST_KEYBOARDS; i++) {
		if (keyboards[i].used)
			continue;
		keyboards[i].used = true;
		keyboards[i].dev = dev;
		keyboards[i].instance = instance;
		(void)tuh_hid_receive_report(dev, instance);
		announce(dev, "attached");
		if (leds != 0)
			usbhost_set_leds(leds);
		return;
	}
}

void
tuh_hid_umount_cb(uint8_t dev, uint8_t instance)
{
	for (unsigned i = 0; i < USBHOST_KEYBOARDS; i++) {
		if (keyboards[i].used && keyboards[i].dev == dev &&
		    keyboards[i].instance == instance) {
			keyboards[i].used = false;
			announce(dev, "removed");
		}
	}
}

void
tuh_hid_report_received_cb(uint8_t dev, uint8_t instance,
    uint8_t const *report, uint16_t len)
{
	/*
	 * Ask for the next report before handling this one, so a handler
	 * that takes a while (a screenshot: a second of SD writes) doesn't
	 * leave the key-up unread meanwhile. The next transfer reuses the
	 * buffer `report` points into, so this one is copied first.
	 */
	uint8_t copy[64];
	if (len > sizeof(copy))
		len = sizeof(copy);
	memcpy(copy, report, len);
	(void)tuh_hid_receive_report(dev, instance);

	bool keyboard = false;
	for (unsigned i = 0; i < USBHOST_KEYBOARDS; i++)
		keyboard = keyboard || (keyboards[i].used &&
		    keyboards[i].dev == dev && keyboards[i].instance == instance);
	if (keyboard && report_fn != nullptr) {
		report_fn(copy, len, callback_ctx);
	} else if (VEETEE_DEBUG_BUILD) {
		usbio_printf("[hid] interface %u protocol %u:", instance,
		    tuh_hid_interface_protocol(dev, instance));
		for (uint16_t i = 0; i < len; i++)
			usbio_printf(" %02x", copy[i]);
		usbio_printf("\r\n");
	}
}

#endif /* Pico-PIO-USB */

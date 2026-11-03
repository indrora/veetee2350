/*
 * automation_hid.h — the debug builds' automation HID: report layout.
 *
 * Pure data, shared by the firmware (src/main.cpp moves the reports,
 * lib/input/automation.c reads them, lib/hal/usbio.c writes the log)
 * and mirrored by tools/vthid.py on the Mac. Change one, change both.
 *
 * Debug builds (include/build.h) present a vendor-defined HID interface
 * (usage page 0xFF00, usage 1) beside the line's CDC port, so a script
 * can drive the VeeTee as a person would: type, press keys on a USB
 * keyboard, press BOOTSEL. It replaces the CDC "keyboard" port that
 * stood in for a keyboard until the board had one of its own (devlog
 * 029). Release builds have no such interface.
 *
 * Reports are AUTOMATION_REPORT_SIZE bytes each way, with no HID report
 * IDs (TinyUSB's generic in/out descriptor); byte 0 says what a report
 * is, the rest is zero-padded:
 *
 *   OUT (Mac to VeeTee)
 *     AUTOMATION_KEYBOARD  [1, then an 8-byte HID boot keyboard report]
 *                          as from a USB keyboard on the board's own port,
 *                          through the same decoder (layout, locks,
 *                          auto-repeat), with a keyboard state of its own
 *     AUTOMATION_TYPE      [2, n, then n bytes]  what an xterm sends for
 *                          keys: text, "\x1b[1;5R" for Ctrl+F3 (keys.c)
 *     AUTOMATION_BOOTSEL   [3, down]  a BOOTSEL of its own, OR'd with
 *                          the real one: taps, holds, the factory reset
 *   IN (VeeTee to Mac)
 *     AUTOMATION_LOG       [4, n, then n bytes]  debug text, the stream
 *                          usbio_printf() writes ("[key] F3 -> set-up");
 *                          lines may span reports
 */
#ifndef AUTOMATION_HID_H
#define AUTOMATION_HID_H

#define AUTOMATION_REPORT_SIZE	64
#define AUTOMATION_TEXT_MAX	(AUTOMATION_REPORT_SIZE - 2)

enum automation_report_type {
	AUTOMATION_KEYBOARD = 1,
	AUTOMATION_TYPE = 2,
	AUTOMATION_BOOTSEL = 3,
	AUTOMATION_LOG = 4,
};

#endif /* AUTOMATION_HID_H */

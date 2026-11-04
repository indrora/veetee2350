/*
 * serial.h — the hardware UART on the 40-pin header, as the terminal's
 * communication line (the other choice is the USB CDC port, usbio.h).
 *
 * Wiring (Waveshare RP2350-PiZero header, docs/hat.md; the HAT puts an
 * RS-232 transceiver on these):
 *
 *   UART1 TX   GPIO 4   header pin 8     (the Pi's TXD position)
 *   UART1 RX   GPIO 5   header pin 10    (the Pi's RXD position)
 *   CTS        GPIO 6   header pin 31    in: low = the host may be sent to
 *   RTS        GPIO 7   header pin 26    out: low = the host may send
 *
 * All 3.3 V logic. A USB-serial adapter wires its RX to pin 8 and its TX
 * to pin 10 (crossed), and GND to any ground pin.
 *
 * Receiving and sending both go through rings filled and drained by the
 * UART's interrupt, so the main loop can be busy (an SD card write takes
 * tens of milliseconds) without the 32-byte FIFO overrunning; at 115200
 * baud 32 bytes last under 3 ms. With flow control, RTS is driven by
 * hand from how full the receive ring is: the PL011's own RTS follows
 * only its FIFO, which the interrupt keeps empty, so it would never tell
 * the host to stop. CTS is the PL011's own: it stops transmitting
 * between characters while the host holds CTS high.
 *
 * Core 0 only. Every call is non-blocking.
 */
#ifndef HAL_SERIAL_H
#define HAL_SERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct serial_format {
	uint32_t	baud;
	uint8_t		data_bits;	/* 5 to 8 */
	uint8_t		parity;		/* 0 none, 1 even, 2 odd (settings.h) */
	uint8_t		stop_bits;	/* 1 or 2 */
	bool		rts_cts;
};

/*
 * Start (or restart, with a new format) the UART: claims GPIO 4 and 5,
 * and 6 and 7 with flow control. Bytes received before are dropped.
 */
void	serial_begin(const struct serial_format *format);
/* Stop it and give the pins back (inputs, pulled up). */
void	serial_end(void);

/* How many received bytes are waiting in the ring. */
size_t	serial_available(void);
/* Up to `cap` received bytes into buf; returns the count. */
size_t	serial_read(uint8_t *buf, size_t cap);
/* Queue bytes to send; returns how many fit (the rest are dropped). */
size_t	serial_write(const uint8_t *buf, size_t len);

/*
 * Errors seen since serial_begin: framing (wrong speed or format, or
 * wires crossed the wrong way), parity, receive overruns (bytes the ring
 * had no room for). For the boot banner and debug text.
 */
struct serial_errors {
	uint32_t	framing;
	uint32_t	parity;
	uint32_t	overrun;
};
void	serial_errors(struct serial_errors *out);

#endif /* HAL_SERIAL_H */

/*
 * serial.c — the header's hardware UART as the communication line (see
 * serial.h for wiring and why everything goes through rings).
 *
 * Rings: the interrupt is the only producer of `rx` and the main loop its
 * only consumer, so plain head/tail indices with a memory barrier are
 * enough there (STYLE.md: single writer + volatile + barrier). `tx` is
 * filled by the main loop and drained by the interrupt; starting a send
 * ("kick") touches the FIFO and the interrupt mask, so it runs with
 * interrupts off for those few instructions.
 */
#include "serial.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/uart.h"

#define SERIAL_UART	uart1
#define PIN_TX		4u
#define PIN_RX		5u
#define PIN_CTS		6u
#define PIN_RTS		7u

/*
 * 8 KiB received is 0.7 s at 115200 baud: far longer than the main loop
 * ever stops (an SD write, a card scan). Sizes are powers of two so the
 * free-running indices wrap with a mask.
 */
#define RX_SIZE		8192u
#define TX_SIZE		2048u
/* RTS: tell the host to stop at 3/4 full, to go on again below 1/4. */
#define RX_STOP_AT	(RX_SIZE * 3 / 4)
#define RX_GO_BELOW	(RX_SIZE / 4)

static uint8_t rx_ring[RX_SIZE];
static volatile uint32_t rx_head;	/* written by the interrupt */
static volatile uint32_t rx_tail;	/* written by the main loop */
static uint8_t tx_ring[TX_SIZE];
static volatile uint32_t tx_head;	/* written by the main loop */
static volatile uint32_t tx_tail;	/* written by the interrupt */

static bool running;
static bool rts_cts;
static volatile bool rts_stopped;	/* RTS high: the host is to wait */
static volatile struct serial_errors errors;

/* PL011 data register error bits, beside each received byte. */
#define DR_FE		(1u << 8)	/* framing: no stop bit where expected */
#define DR_PE		(1u << 9)
#define DR_OE		(1u << 11)	/* the FIFO itself overflowed */

static void
set_rts(bool stop)
{
	rts_stopped = stop;
	gpio_put(PIN_RTS, stop);	/* active low: high means "wait" */
}

/* Move FIFO bytes into the ring, and ring bytes into the FIFO. */
static void
serial_irq(void)
{
	uart_hw_t *hw = uart_get_hw(SERIAL_UART);

	while ((hw->fr & UART_UARTFR_RXFE_BITS) == 0) {
		uint32_t dr = hw->dr;
		uint32_t used = rx_head - rx_tail;

		if (dr & DR_FE)
			errors.framing++;
		if (dr & DR_PE)
			errors.parity++;
		if (dr & DR_OE)
			errors.overrun++;
		if (used >= RX_SIZE) {
			errors.overrun++;	/* the ring is full: dropped */
			continue;
		}
		rx_ring[rx_head % RX_SIZE] = (uint8_t)dr;
		__dmb();		/* the byte before the index */
		rx_head++;
		if (rts_cts && !rts_stopped && used + 1 >= RX_STOP_AT)
			set_rts(true);
	}
	while (tx_tail != tx_head && (hw->fr & UART_UARTFR_TXFF_BITS) == 0) {
		hw->dr = tx_ring[tx_tail % TX_SIZE];
		tx_tail++;
	}
	if (tx_tail == tx_head)
		hw_clear_bits(&hw->imsc, UART_UARTIMSC_TXIM_BITS);
}

void
serial_begin(const struct serial_format *format)
{
	static const uart_parity_t parities[3] = {
		UART_PARITY_NONE, UART_PARITY_EVEN, UART_PARITY_ODD,
	};

	if (running)
		serial_end();
	rx_head = rx_tail = 0;
	tx_head = tx_tail = 0;
	errors = (struct serial_errors){0};
	rts_cts = format->rts_cts;

	uart_init(SERIAL_UART, format->baud);
	uart_set_format(SERIAL_UART, format->data_bits, format->stop_bits,
	    parities[format->parity % 3]);
	uart_set_fifo_enabled(SERIAL_UART, true);
	/* CTS by the PL011; RTS by hand (serial.h says why). */
	uart_set_hw_flow(SERIAL_UART, rts_cts, false);
	gpio_set_function(PIN_TX, UART_FUNCSEL_NUM(SERIAL_UART, PIN_TX));
	gpio_set_function(PIN_RX, UART_FUNCSEL_NUM(SERIAL_UART, PIN_RX));
	/* An unconnected RX idles high (a mark), not as a stream of
	 * breaks and framing errors. */
	gpio_pull_up(PIN_RX);
	if (rts_cts) {
		gpio_set_function(PIN_CTS, UART_FUNCSEL_NUM(SERIAL_UART,
		    PIN_CTS));
		gpio_pull_up(PIN_CTS);	/* unconnected: hold off, not send */
		gpio_init(PIN_RTS);
		gpio_set_dir(PIN_RTS, GPIO_OUT);
		set_rts(false);
	}

	irq_set_exclusive_handler(UART1_IRQ, serial_irq);
	/* Receive: FIFO half full, or a quiet spell with bytes waiting. */
	uart_get_hw(SERIAL_UART)->imsc = UART_UARTIMSC_RXIM_BITS |
	    UART_UARTIMSC_RTIM_BITS;
	irq_set_enabled(UART1_IRQ, true);
	running = true;
}

void
serial_end(void)
{
	if (!running)
		return;
	irq_set_enabled(UART1_IRQ, false);
	irq_remove_handler(UART1_IRQ, serial_irq);
	uart_deinit(SERIAL_UART);
	static const unsigned pins[] = {PIN_TX, PIN_RX, PIN_CTS, PIN_RTS};
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		if (!rts_cts && (pins[i] == PIN_CTS || pins[i] == PIN_RTS))
			continue;	/* never ours: leave them alone */
		gpio_init(pins[i]);
		gpio_pull_up(pins[i]);
	}
	running = false;
}

size_t
serial_read(uint8_t *buf, size_t cap)
{
	size_t count = 0;

	if (!running)
		return 0;
	while (count < cap && rx_tail != rx_head) {
		__dmb();	/* the index before the byte it covers */
		buf[count++] = rx_ring[rx_tail % RX_SIZE];
		rx_tail++;
	}
	if (rts_cts && rts_stopped && rx_head - rx_tail < RX_GO_BELOW)
		set_rts(false);
	return count;
}

size_t
serial_write(const uint8_t *buf, size_t len)
{
	size_t count = 0;

	if (!running)
		return 0;
	while (count < len && tx_head - tx_tail < TX_SIZE) {
		tx_ring[tx_head % TX_SIZE] = buf[count++];
		__dmb();
		tx_head++;
	}
	/* Kick: fill the FIFO now and let its interrupt take the rest. */
	uint32_t saved = save_and_disable_interrupts();
	uart_hw_t *hw = uart_get_hw(SERIAL_UART);
	while (tx_tail != tx_head && (hw->fr & UART_UARTFR_TXFF_BITS) == 0) {
		hw->dr = tx_ring[tx_tail % TX_SIZE];
		tx_tail++;
	}
	if (tx_tail != tx_head)
		hw_set_bits(&hw->imsc, UART_UARTIMSC_TXIM_BITS);
	restore_interrupts(saved);
	return count;
}

void
serial_errors(struct serial_errors *out)
{
	out->framing = errors.framing;
	out->parity = errors.parity;
	out->overrun = errors.overrun;
}

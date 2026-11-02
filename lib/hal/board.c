/*
 * board.c — board-level helpers implemented in C (see board.h).
 */
#include "board.h"

#include <malloc.h>

#include "hardware/structs/qmi.h"
#include "hardware/structs/timer.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico.h"

/*
 * Formula from arduino-pico's PSRAM setup, applied to flash: the divider
 * keeps the flash clock at or under 133 MHz, and the sample point
 * (RXDELAY, in half-sysclk cycles) moves one later above 100 MHz. At
 * 252 MHz it gives the values PicoDVI's RP2350 demos use (clkdiv 2,
 * rxdelay 3); at 372 MHz, clkdiv 3 (124 MHz) and rxdelay 4.
 *
 * Interrupts are masked inside so it's safe to call with them on, but
 * callers that already masked them (flashcfg_write) can call it too.
 */
void __no_inline_not_in_flash_func(board_flash_timing)(uint32_t sys_hz)
{
	const uint32_t max_flash_hz = 133000000;
	uint32_t divider = (sys_hz + max_flash_hz - 1) / max_flash_hz;
	if (divider < 2)
		divider = 2;
	uint32_t rxdelay = divider + (sys_hz / divider > 100000000 ? 1 : 0);

	uint32_t saved = save_and_disable_interrupts();
	hw_write_masked(&qmi_hw->m[0].timing,
	    divider << QMI_M0_TIMING_CLKDIV_LSB |
	    rxdelay << QMI_M0_TIMING_RXDELAY_LSB,
	    QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS);
	restore_interrupts(saved);
}

#include "hardware/structs/watchdog.h"

/*
 * scratch[0..3] are free for applications; the SDK's watchdog_reboot uses
 * scratch[4..7].
 */
#define BOOT_MAGIC	0x56544254u	/* "VTBT" */
#define WATCHDOG_MS	8000

/*
 * HardFault: record where it happened, then spin until the watchdog resets
 * us. Overrides the SDK's weak isr_hardfault. scratch[2]/[3] hold PC/LR;
 * scratch[1] bit 31 marks them valid. The exception frame is on whichever
 * stack was active (EXC_RETURN bit 2 says which).
 */
#define FAULT_VALID	0x80000000u

void __attribute__((used)) __not_in_flash_func(hardfault_record)(
    const uint32_t *frame, uint32_t r5)
{
	watchdog_hw->scratch[2] = frame[6];	/* stacked PC */
	watchdog_hw->scratch[3] = frame[5];	/* stacked LR */
	/* Debug aid: r0, r2, r5 of the faulting context. scratch[5..7] are
	 * only used by the SDK for watchdog_reboot-to-PC, which we don't. */
	watchdog_hw->scratch[5] = frame[0];
	watchdog_hw->scratch[6] = frame[2];
	watchdog_hw->scratch[7] = r5;
	watchdog_hw->scratch[1] |= FAULT_VALID;
	for (;;)
		;
}

void __attribute__((naked)) __not_in_flash_func(isr_hardfault)(void)
{
	__asm volatile(
	    "tst lr, #4\n"
	    "ite eq\n"
	    "mrseq r0, msp\n"
	    "mrsne r0, psp\n"
	    "mov r1, r5\n"
	    "b hardfault_record\n");
}

static uint32_t last_fault_pc, last_fault_lr;
static bool last_fault_valid;

bool
board_last_fault(uint32_t *pc, uint32_t *lr)
{
	*pc = last_fault_pc;
	*lr = last_fault_lr;
	return last_fault_valid;
}

unsigned
board_boot_begin(void)
{
	if (watchdog_hw->scratch[0] == BOOT_MAGIC &&
	    (watchdog_hw->scratch[1] & FAULT_VALID)) {
		last_fault_valid = true;
		last_fault_pc = watchdog_hw->scratch[2];
		last_fault_lr = watchdog_hw->scratch[3];
	}
	watchdog_hw->scratch[1] &= ~FAULT_VALID;

	unsigned crashes = 0;

	if (watchdog_enable_caused_reboot() &&
	    watchdog_hw->scratch[0] == BOOT_MAGIC)
		crashes = (watchdog_hw->scratch[1] & ~FAULT_VALID) + 1;
	watchdog_hw->scratch[0] = BOOT_MAGIC;
	watchdog_hw->scratch[1] = crashes;
	watchdog_enable(WATCHDOG_MS, true);
	return crashes;
}

void
board_boot_ok(void)
{
	watchdog_hw->scratch[1] = 0;
}

void
board_watchdog_feed(void)
{
	watchdog_update();
}

void
board_reboot(void)
{
	/* Let the watchdog reset the chip shortly; USB re-enumerates. */
	watchdog_reboot(0, 0, 50);
	for (;;)
		;
}

void __no_inline_not_in_flash_func(board_flash_timing_transition)(
    uint32_t max_hz)
{
	const uint32_t max_flash_hz = 133000000;
	uint32_t divider = (max_hz + max_flash_hz - 1) / max_flash_hz + 1;
	if (divider < 3)
		divider = 3;

	uint32_t saved = save_and_disable_interrupts();
	hw_write_masked(&qmi_hw->m[0].timing,
	    divider << QMI_M0_TIMING_CLKDIV_LSB |
	    3u << QMI_M0_TIMING_RXDELAY_LSB,
	    QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS);
	restore_interrupts(saved);
}

#ifdef VEETEE_FLASH_PROBE
extern uint8_t __flash_binary_end;	/* linker: end of the firmware image */

static uint32_t probe_reference;

/* Checksum the image through the uncached, non-allocating XIP alias, so
 * every word really comes over the QSPI bus with the current timing. */
static uint32_t __no_inline_not_in_flash_func(image_checksum)(void)
{
	const volatile uint32_t *word =
	    (const volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
	uint32_t words = ((uint32_t)&__flash_binary_end - XIP_BASE) / 4;
	uint32_t hash = 0x811c9dc5u;

	for (uint32_t i = 0; i < words; i++)
		hash = ((hash << 5) | (hash >> 27)) ^ word[i];
	return hash;
}

uint32_t board_boot_qmi_timing;

void __no_inline_not_in_flash_func(board_flash_probe_reference)(void)
{
	board_boot_qmi_timing = qmi_hw->m[0].timing;
	uint32_t saved = save_and_disable_interrupts();
	probe_reference = image_checksum();
	restore_interrupts(saved);
}

void __no_inline_not_in_flash_func(board_flash_probe)(uint32_t sys_hz,
    struct board_flash_probe *out)
{
	const uint32_t max_flash_hz = 133000000;
	uint32_t divider = (sys_hz + max_flash_hz - 1) / max_flash_hz;
	if (divider < 2)
		divider = 2;

	out->sys_hz = sys_hz;
	out->divider = (uint8_t)divider;
	uint32_t started = timer0_hw->timerawl;
	uint32_t saved = save_and_disable_interrupts();
	for (uint32_t variant = 0; variant < 2; variant++) {
		out->pass[variant] = 0;
		for (uint32_t rxdelay = 0; rxdelay < 8; rxdelay++) {
			hw_write_masked(&qmi_hw->m[0].timing,
			    (divider + variant) << QMI_M0_TIMING_CLKDIV_LSB |
			    rxdelay << QMI_M0_TIMING_RXDELAY_LSB,
			    QMI_M0_TIMING_CLKDIV_BITS |
			    QMI_M0_TIMING_RXDELAY_BITS);
			if (image_checksum() == probe_reference)
				out->pass[variant] |= (uint8_t)(1u << rxdelay);
		}
	}
	restore_interrupts(saved);
	board_flash_timing(sys_hz);
	out->probe_us = timer0_hw->timerawl - started;
}
#endif

size_t
board_free_heap(void)
{
	extern char __StackLimit, __bss_end__;
	struct mallinfo info = mallinfo();

	return (size_t)(&__StackLimit - &__bss_end__) - info.uordblks;
}

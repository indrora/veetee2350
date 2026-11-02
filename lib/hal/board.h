/*
 * board.h — odds and ends of the Waveshare RP2350-PiZero.
 */
#ifndef HAL_BOARD_H
#define HAL_BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * State of the BOOTSEL button (used as the SET-UP key). Disables
 * interrupts and XIP for ~10 us: only call when video_flash_safe().
 * Implemented in src/main.cpp (Arduino's BOOTSEL object is C++).
 */
bool	board_bootsel_pressed(void);

/*
 * Crash guard. The watchdog (8 s, fed from the main loop) resets a hung or
 * crashed chip. board_boot_begin() starts it and returns how many boots in
 * a row ended that way: watchdog scratch registers survive the reset, and
 * the SDK can tell a watchdog *timeout* from a deliberate watchdog_reboot,
 * power-on or upload (those reset the count). board_boot_ok() clears the
 * count once the system has run healthily for a while.
 */
unsigned board_boot_begin(void);

/*
 * If the previous boot ended in a HardFault, the faulting PC/LR (saved by
 * our isr_hardfault into watchdog scratch registers); else false.
 */
bool	board_last_fault(uint32_t *pc, uint32_t *lr);
void	board_boot_ok(void);

/*
 * Why the chip last reset, from the RP2350's latched flags (POWMAN
 * CHIP_RESET): "power-on", "brown-out", "RUN pin", "watchdog",
 * "debugger", "glitch detector", "software" or "unknown". A brown-out is
 * worth saying out loud: this board's USB host port takes its 5 V
 * straight from the supply, and a keyboard plugged in while running can
 * pull it down far enough (devlog 027).
 */
const char *board_reset_reason(void);

/*
 * Bytes malloc can still hand out: the heap's span (end of .bss up to
 * core 0's stack limit) minus what's allocated, as arduino-pico's
 * rp2040.getFreeHeap() computes it. Fragmentation can make the largest
 * single block smaller.
 */
size_t	board_free_heap(void);
void	board_watchdog_feed(void);

/*
 * Set the QSPI flash clock divider and sample delay for a given sysclk
 * (flash is rated to 133 MHz; at 372 MHz the boot divider of 2 would run
 * it at 186 MHz). Runs from RAM with interrupts off. Must be called before
 * raising sysclk, and again after anything that re-enters XIP with boot
 * defaults -- flash_range_erase/program do (see flashcfg.c).
 */
void	board_flash_timing(uint32_t sys_hz);

/*
 * Flash timing that is safe at every clock a sysclk change passes through
 * on its way to `max_hz`: set_sys_clock_khz (which runs from flash!)
 * briefly parks clk_sys on the 48 MHz USB PLL. The normal timing for
 * 372 MHz (clkdiv 3, rxdelay 4) samples too late at 48 MHz and the next
 * instruction fetch is garbage -- 960x540 hard-faulted inside
 * clock_configure on boot. One divider step slower with rxdelay 3 is
 * inside the read window at 48 MHz and at 252..372 MHz.
 */
void	board_flash_timing_transition(uint32_t max_hz);

#ifdef VEETEE_FLASH_PROBE
/*
 * Debug builds only: find which flash RX delays read correctly at a clock.
 * board_flash_probe_reference() checksums the firmware image (call at the
 * safe boot clock); board_flash_probe() then re-reads it uncached at every
 * RXDELAY 0..7 for divider d and d+1 (d from board_flash_timing's formula)
 * and records pass bitmasks; afterwards the normal timing is restored.
 */
struct board_flash_probe {
	uint32_t	sys_hz;
	uint8_t		divider;
	uint8_t		pass[2];	/* bit n set: RXDELAY n read OK */
	uint32_t	probe_us;	/* how long the sweep took */
	uint32_t	boot_timing;	/* QMI M0 TIMING as booted */
};
void	board_flash_probe_reference(void);
void	board_flash_probe(uint32_t sys_hz, struct board_flash_probe *out);
#endif

/* Restart the chip (e.g. to apply a new video mode). Does not return. */
[[noreturn]] void board_reboot(void);

#endif /* HAL_BOARD_H */

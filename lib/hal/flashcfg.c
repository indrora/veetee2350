/*
 * flashcfg.c — read/write the settings sector.
 *
 * Writing flash makes XIP unavailable. That's safe here because:
 *  - core 0 runs this with interrupts masked, and flash_range_erase/
 *    program themselves execute from RAM;
 *  - core 1 never executes from flash once video_flash_safe() is true
 *    (tools/core1_audit.py checks every build);
 *  - DMA only touches SRAM.
 */
#include "flashcfg.h"

#include <string.h>

#include "board.h"
#include "video.h"

#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico.h"

extern uint8_t _EEPROM_start;	/* from arduino-pico's linker script */

static_assert(FLASHCFG_MAX == FLASH_SECTOR_SIZE);

void
flashcfg_read(void *dst, size_t len)
{
	if (len > FLASHCFG_MAX)
		len = FLASHCFG_MAX;
	memcpy(dst, &_EEPROM_start, len);
}

/*
 * In RAM, not flash: flash_range_erase/program re-enter XIP using the boot
 * defaults, which on RP2350 resets the QMI divider to 2. Above ~266 MHz
 * that overclocks the flash, so the timing must be put back before
 * ANYTHING fetches from flash again -- including returning into this
 * function, were it in flash. (Found when a save at 270 MHz read back
 * garbage and the board restarted in the old video mode.)
 */
bool __no_inline_not_in_flash_func(flashcfg_write)(const void *src, size_t len)
{
	/* flash_range_program wants whole 256-byte pages. */
	static uint8_t page_buffer[1024] __attribute__((aligned(4)));
	uint32_t offset = (uint32_t)&_EEPROM_start - XIP_BASE;

	if (!video_flash_safe() || len > sizeof(page_buffer))
		return false;
	size_t padded = (len + FLASH_PAGE_SIZE - 1) & ~(size_t)(FLASH_PAGE_SIZE - 1);
	memset(page_buffer, 0xff, padded);
	memcpy(page_buffer, src, len);

	uint32_t sys_hz = clock_get_hz(clk_sys);	/* flash code: before */
	uint32_t saved = save_and_disable_interrupts();
	flash_range_erase(offset, FLASH_SECTOR_SIZE);
	flash_range_program(offset, page_buffer, padded);
	board_flash_timing(sys_hz);
	restore_interrupts(saved);

	/* Byte compare by hand: memcmp may live in flash, which is fine now,
	 * but keep the check obviously after the timing fix above. */
	const uint8_t *stored = &_EEPROM_start;
	const uint8_t *wanted = src;
	for (size_t i = 0; i < len; i++)
		if (stored[i] != wanted[i])
			return false;
	return true;
}

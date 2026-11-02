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

#include "video.h"

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

bool
flashcfg_write(const void *src, size_t len)
{
	/* flash_range_program wants whole 256-byte pages. */
	static uint8_t page_buffer[1024] __attribute__((aligned(4)));
	uint32_t offset = (uint32_t)&_EEPROM_start - XIP_BASE;

	if (!video_flash_safe() || len > sizeof(page_buffer))
		return false;
	size_t padded = (len + FLASH_PAGE_SIZE - 1) & ~(size_t)(FLASH_PAGE_SIZE - 1);
	memset(page_buffer, 0xff, padded);
	memcpy(page_buffer, src, len);

	uint32_t saved = save_and_disable_interrupts();
	flash_range_erase(offset, FLASH_SECTOR_SIZE);
	flash_range_program(offset, page_buffer, padded);
	restore_interrupts(saved);
	return memcmp(&_EEPROM_start, src, len) == 0;
}

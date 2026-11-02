/*
 * flashcfg.h — a small record kept in the last 4 KB sector of flash.
 *
 * Uses the sector arduino-pico's linker script reserves for its EEPROM
 * emulation (_EEPROM_start); the EEPROM library itself is not used. Holds
 * the SET-UP settings (lib/setup/settings.h).
 */
#ifndef HAL_FLASHCFG_H
#define HAL_FLASHCFG_H

#include <stdbool.h>
#include <stddef.h>

#define FLASHCFG_MAX	4096

/* Copy the stored record (whatever is there; caller validates). */
void	flashcfg_read(void *dst, size_t len);

/*
 * Erase the sector and write `len` bytes. Takes ~50 ms with core 0's
 * interrupts masked: USB pauses and the picture shows filler lines for a
 * few frames. Refuses (returns false) until video_flash_safe().
 */
[[nodiscard]] bool flashcfg_write(const void *src, size_t len);

#endif /* HAL_FLASHCFG_H */

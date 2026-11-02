/*
 * board.h — odds and ends of the Waveshare RP2350-PiZero.
 */
#ifndef HAL_BOARD_H
#define HAL_BOARD_H

#include <stdbool.h>

/*
 * State of the BOOTSEL button (used as the SET-UP key). Disables
 * interrupts and XIP for ~10 us: only call when video_flash_safe().
 * Implemented in src/main.cpp (Arduino's BOOTSEL object is C++).
 */
bool	board_bootsel_pressed(void);

#endif /* HAL_BOARD_H */

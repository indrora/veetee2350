/*
 * capture_sd.h — record what the host sends to /capture/NNNN.rec on the
 * SD card, as ttyrec (lib/ttyrec), for replay here or with ttyplay.
 */
#ifndef CAPTURE_SD_H
#define CAPTURE_SD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Switch capturing on (a new file on the first bytes) or off (closed). */
void	capture_sd_enable(bool on);
bool	capture_sd_enabled(void);

/* Host bytes, as they're handed to the terminal. */
void	capture_sd_feed(const uint8_t *bytes, size_t len, uint32_t now_ms);

/* Main loop: write out what's buffered, close the file when idle. */
void	capture_sd_poll(uint32_t now_ms);

/*
 * The file being written ("0003.rec"), or "" before the first bytes.
 * After a failure (card pulled or full) capturing stops and
 * capture_sd_error() says why; switching it on again starts afresh.
 */
const char *capture_sd_name(void);
const char *capture_sd_error(void);

#endif /* CAPTURE_SD_H */

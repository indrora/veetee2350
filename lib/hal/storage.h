/*
 * storage.h — the microSD card, as a small C file API.
 *
 * Wiring (Waveshare RP2350-PiZero TF slot, used in SPI mode on SPI1):
 *
 *   GP30  SDIO_CLK / SPI SCK      GP40  SDIO_D0 / SPI MISO (RX)
 *   GP31  SDIO_CMD / SPI MOSI     GP43  SDIO_D3 / SPI CS (plain GPIO)
 *
 * (GP41/42, DAT1/DAT2, are idle in SPI mode.) SPI rather than 4-bit SDIO
 * keeps the card off the PIO blocks (pio0 is the DVI output; the others
 * are wanted for a USB host port) and independent of the per-mode
 * sysclk. There is no card-detect switch -- DAT3 doubles as CS -- so "no
 * card" is simply a failed mount.
 *
 * Implemented in src/storage_sd.cpp on arduino-pico's bundled SD library
 * (SdFat 2.x: FAT12/16/32 and exFAT), which is C++. Everything here is
 * blocking and runs on core 0 in the main loop; the renderer's interrupt
 * keeps the picture going meanwhile. Never call from an interrupt.
 *
 * Paths are absolute ("/fonts/x.psf"). Handles are small integers; a
 * few files can be open at once.
 */
#ifndef HAL_STORAGE_H
#define HAL_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STORAGE_MAX_OPEN	3
#define STORAGE_NAME_MAX	64	/* longest file name we pass around */

enum storage_mode {
	STORAGE_READ,		/* existing file, from the start */
	STORAGE_CREATE,		/* create or truncate, write */
	STORAGE_APPEND,		/* create or append, write */
};

struct storage_info {
	bool		mounted;
	uint8_t		fat_type;	/* 12, 16, 32, or 64 for exFAT */
	uint64_t	bytes;		/* volume size */
};

/*
 * Mount the card if it isn't already (cheap when it is). Returns false
 * with no card, an unreadable card, or no FAT/exFAT volume; see
 * storage_error(). After a card is pulled, calls fail and the next
 * storage_mount() probes again.
 */
bool	storage_mount(void);
void	storage_unmount(void);
bool	storage_info(struct storage_info *info);
/* Human-readable reason for the last failure ("no card", ...). */
const char *storage_error(void);

/* Open `path`; returns a handle >= 0, or -1 (see storage_error()). */
int	storage_open(const char *path, enum storage_mode mode);
/* Bytes moved, 0 at end of file, -1 on error. */
long	storage_read(int handle, void *buf, size_t len);
long	storage_write(int handle, const void *buf, size_t len);
long	storage_size(int handle);
/* Flushes; false if the data may not have reached the card. */
bool	storage_close(int handle);

bool	storage_exists(const char *path);
bool	storage_mkdir(const char *path);	/* true if it exists after */

/*
 * Call `entry` for each file and directory in `dir` (not recursive, in
 * directory order). `entry` returns false to stop early. Returns false
 * if `dir` can't be read.
 */
typedef bool (*storage_entry_fn)(const char *name, uint32_t size,
    bool is_dir, void *ctx);
bool	storage_list(const char *dir, storage_entry_fn entry, void *ctx);

#endif /* HAL_STORAGE_H */

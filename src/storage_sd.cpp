/*
 * storage_sd.cpp — lib/hal/storage.h on arduino-pico's SD library.
 *
 * C++ only because SD/SdFat are C++ (the same reason src/main.cpp holds
 * the USB CDC shims). Each function is a thin wrapper; the policy --
 * what lives where on the card, what to do when it's missing -- is in
 * the C code that calls these.
 */
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

extern "C" {
#include "storage.h"
}

#define PIN_SD_SCK	30
#define PIN_SD_MOSI	31
#define PIN_SD_MISO	40
#define PIN_SD_CS	43

/*
 * 25 MHz is the SPI-mode ceiling for standard SD cards. The SPI block's
 * clock is clk_peri (= sysclk here, 252-372 MHz by video mode) divided
 * by an even prescaler, so the real rate lands a little under that.
 */
#define SD_CLOCK_HZ	25000000u

static bool mounted;
static bool spi_pins_set;
static const char *last_error = "not mounted";
static File files[STORAGE_MAX_OPEN];

static File *
file_for(int handle)
{
	if (handle < 0 || handle >= STORAGE_MAX_OPEN || !files[handle])
		return nullptr;
	return &files[handle];
}

extern "C" bool
storage_mount(void)
{
	if (mounted)
		return true;
	if (!spi_pins_set) {
		/* SPI1 on the TF slot's pins; only valid on the RP2350B. */
		if (!SPI1.setSCK(PIN_SD_SCK) || !SPI1.setTX(PIN_SD_MOSI) ||
		    !SPI1.setRX(PIN_SD_MISO)) {
			last_error = "SD pins rejected by SPI1";
			return false;
		}
		spi_pins_set = true;
	}
	if (!SD.begin(PIN_SD_CS, SD_CLOCK_HZ, SPI1)) {
		SD.end(false);
		last_error = "no card / not FAT";
		return false;
	}
	mounted = true;
	last_error = "";
	return true;
}

extern "C" void
storage_unmount(void)
{
	for (File &file : files)
		if (file)
			file.close();
	if (mounted)
		SD.end(false);
	mounted = false;
	last_error = "not mounted";
}

extern "C" bool
storage_info(struct storage_info *info)
{
	info->mounted = mounted;
	info->fat_type = mounted ? SD.fatType() : 0;
	info->bytes = mounted ? SD.size64() : 0;
	return mounted;
}

extern "C" const char *
storage_error(void)
{
	return last_error;
}

extern "C" int
storage_open(const char *path, enum storage_mode mode)
{
	static const char *const modes[] = {"r", "w", "a"};
	int handle;

	if (!mounted) {
		last_error = "no card";
		return -1;
	}
	for (handle = 0; handle < STORAGE_MAX_OPEN; handle++)
		if (!files[handle])
			break;
	if (handle == STORAGE_MAX_OPEN) {
		last_error = "too many open files";
		return -1;
	}
	files[handle] = SD.open(path, modes[mode]);
	if (!files[handle]) {
		/* Most often: no such file, or the card went away. */
		last_error = mode == STORAGE_READ ? "file not found" :
		    "cannot create file";
		return -1;
	}
	return handle;
}

extern "C" long
storage_read(int handle, void *buf, size_t len)
{
	File *file = file_for(handle);

	if (file == nullptr)
		return -1;
	int got = file->read(static_cast<uint8_t *>(buf), len);
	if (got < 0)
		last_error = "read error";
	return got;
}

extern "C" long
storage_write(int handle, const void *buf, size_t len)
{
	File *file = file_for(handle);

	if (file == nullptr)
		return -1;
	size_t put = file->write(static_cast<const uint8_t *>(buf), len);
	if (put != len) {
		last_error = "write error (card full or removed?)";
		return -1;
	}
	return static_cast<long>(put);
}

extern "C" long
storage_size(int handle)
{
	File *file = file_for(handle);

	return file == nullptr ? -1 : static_cast<long>(file->size());
}

extern "C" bool
storage_close(int handle)
{
	File *file = file_for(handle);

	if (file == nullptr)
		return false;
	file->flush();
	file->close();
	*file = File();
	return true;
}

extern "C" bool
storage_exists(const char *path)
{
	return mounted && SD.exists(path);
}

extern "C" bool
storage_mkdir(const char *path)
{
	if (!mounted)
		return false;
	return SD.exists(path) || SD.mkdir(path);
}

extern "C" bool
storage_list(const char *dir, storage_entry_fn entry, void *ctx)
{
	if (!mounted)
		return false;
	File folder = SD.open(dir);
	if (!folder || !folder.isDirectory()) {
		last_error = "directory not found";
		return false;
	}
	for (File item = folder.openNextFile(); item;
	    item = folder.openNextFile()) {
		bool more = entry(item.name(), static_cast<uint32_t>(item.size()),
		    item.isDirectory(), ctx);
		item.close();
		if (!more)
			break;
	}
	folder.close();
	return true;
}

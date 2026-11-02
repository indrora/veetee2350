/*
 * storage_sd.cpp — lib/hal/storage.h on SdFat (bundled with arduino-pico).
 *
 * C++ only because SdFat is C++ (the same reason src/main.cpp holds the
 * USB CDC shims). Each function is a thin wrapper; the policy -- what
 * lives where on the card, what to do when it's missing -- is in the C
 * code that calls these.
 *
 * Why SdFat directly and not arduino-pico's SD/SDFS wrapper: the
 * wrapper's File::openNextFile() steps a directory iterator and then
 * re-opens the entry *by path*, and every path lookup searches the
 * directory from the top. Listing N files cost O(N^2) directory reads --
 * a card with a few hundred fonts (twice that with the "._" files macOS
 * leaves behind) took longer than the 8 s watchdog. SdFat's
 * FsFile::openNext() opens the next entry where the iterator stands.
 */
#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>

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
 * Nothing else uses SPI1, so SdFat may keep the bus (DEDICATED_SPI),
 * which lets it stream multi-block reads.
 */
#define SD_CLOCK_HZ	25000000u

static SdFs volume;		/* FAT12/16/32 and exFAT */
static bool mounted;
static bool spi_pins_set;
static const char *last_error = "not mounted";
static FsFile files[STORAGE_MAX_OPEN];

static FsFile *
file_for(int handle)
{
	if (handle < 0 || handle >= STORAGE_MAX_OPEN || !files[handle].isOpen())
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
	if (!volume.begin(SdSpiConfig(PIN_SD_CS, DEDICATED_SPI, SD_CLOCK_HZ,
	    &SPI1))) {
		volume.end();
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
	for (FsFile &file : files)
		if (file.isOpen())
			file.close();
	if (mounted)
		volume.end();
	mounted = false;
	last_error = "not mounted";
}

extern "C" bool
storage_info(struct storage_info *info)
{
	info->mounted = mounted;
	info->fat_type = mounted ? volume.fatType() : 0;
	info->bytes = mounted ? static_cast<uint64_t>(volume.clusterCount()) *
	    volume.bytesPerCluster() : 0;
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
	static const oflag_t flags[] = {
		[STORAGE_READ] = O_RDONLY,
		[STORAGE_CREATE] = O_WRONLY | O_CREAT | O_TRUNC,
		[STORAGE_APPEND] = O_WRONLY | O_CREAT | O_APPEND,
	};
	int handle;

	if (!mounted) {
		last_error = "no card";
		return -1;
	}
	for (handle = 0; handle < STORAGE_MAX_OPEN; handle++)
		if (!files[handle].isOpen())
			break;
	if (handle == STORAGE_MAX_OPEN) {
		last_error = "too many open files";
		return -1;
	}
	if (!files[handle].open(&volume, path, flags[mode])) {
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
	FsFile *file = file_for(handle);

	if (file == nullptr)
		return -1;
	int got = file->read(buf, len);
	if (got < 0)
		last_error = "read error";
	return got;
}

extern "C" long
storage_write(int handle, const void *buf, size_t len)
{
	FsFile *file = file_for(handle);

	if (file == nullptr)
		return -1;
	size_t put = file->write(buf, len);
	if (put != len) {
		last_error = "write error (card full or removed?)";
		return -1;
	}
	return static_cast<long>(put);
}

extern "C" long
storage_size(int handle)
{
	FsFile *file = file_for(handle);

	return file == nullptr ? -1 : static_cast<long>(file->fileSize());
}

extern "C" bool
storage_close(int handle)
{
	FsFile *file = file_for(handle);

	if (file == nullptr)
		return false;
	return file->close();	/* flushes; false if that failed */
}

extern "C" bool
storage_exists(const char *path)
{
	return mounted && volume.exists(path);
}

extern "C" bool
storage_mkdir(const char *path)
{
	if (!mounted)
		return false;
	return volume.exists(path) || volume.mkdir(path);
}

extern "C" bool
storage_list(const char *dir, size_t peek, storage_entry_fn entry,
    void *ctx)
{
	uint8_t head[STORAGE_PEEK_MAX];
	char name[STORAGE_NAME_MAX + 1];
	FsFile folder, item;

	if (!mounted)
		return false;
	if (peek > sizeof(head))
		peek = sizeof(head);
	if (!folder.open(&volume, dir, O_RDONLY) || !folder.isDir()) {
		last_error = "directory not found";
		return false;
	}
	while (item.openNext(&folder, O_RDONLY)) {
		bool is_dir = item.isDir();
		int got = 0;

		/* Longer names than we can hold aren't ours: skip them. */
		if (item.getName(name, sizeof(name)) == 0) {
			item.close();
			continue;
		}
		if (peek > 0 && !is_dir)
			got = item.read(head, peek);
		uint32_t size = static_cast<uint32_t>(item.fileSize());
		item.close();
		if (!entry(name, size, is_dir, head, got > 0 ?
		    static_cast<size_t>(got) : 0, ctx))
			break;
	}
	folder.close();
	return true;
}

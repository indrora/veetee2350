/*
 * card.c — fonts and colour schemes from the SD card. See card.h.
 */
#include "card.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "board.h"
#include "settings.h"
#include "storage.h"

#define PSF2_MAGIC	0x864ab572u
#define PSF2_HEADER	32

static struct setup_card_font *fonts;
static char (*schemes)[SETTINGS_SCHEME_MAX];
static unsigned font_count, scheme_count;

/* Two slots for card fonts: see card_font(). */
static struct font_builtin slots[2];
static char slot_names[2][SETTINGS_FONT_MAX];

static uint32_t
read_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
	    (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

/*
 * `name` ends in `suffix` (any case) and the stem fits `max` - 1 chars:
 * copy the stem to `stem`.
 */
static bool
stem_of(const char *name, const char *suffix, char *stem, size_t max)
{
	size_t len = strlen(name), tail = strlen(suffix);

	if (name[0] == '.' || len <= tail || len - tail >= max ||
	    strcasecmp(name + len - tail, suffix) != 0)
		return false;	/* also skips macOS "._x.psf" droppings */
	memcpy(stem, name, len - tail);
	stem[len - tail] = '\0';
	return true;
}

/* Read the whole of `path` (at most `max` bytes) into a new buffer. */
static uint8_t *
read_file(const char *path, size_t max, size_t *len, char *error,
    size_t error_len)
{
	int handle = storage_open(path, STORAGE_READ);
	uint8_t *data;
	long size, got;

	if (handle < 0) {
		snprintf(error, error_len, "%s", storage_error());
		return nullptr;
	}
	size = storage_size(handle);
	if (size <= 0 || (size_t)size > max) {
		storage_close(handle);
		snprintf(error, error_len, size <= 0 ? "empty file" :
		    "too big (limit %u KB)", (unsigned)(max / 1024));
		return nullptr;
	}
	data = malloc((size_t)size);
	if (data == nullptr) {
		storage_close(handle);
		snprintf(error, error_len, "not enough memory");
		return nullptr;
	}
	got = storage_read(handle, data, (size_t)size);
	storage_close(handle);
	if (got != size) {
		free(data);
		snprintf(error, error_len, "read error");
		return nullptr;
	}
	*len = (size_t)size;
	return data;
}

/* --- catalogue -------------------------------------------------------- */

/*
 * Scanning reads 32 bytes of every font for its cell size, which on a
 * card holding a few hundred fonts takes seconds. So the result is
 * cached in CACHE_PATH, keyed by a fingerprint of /fonts and /schemes:
 * FNV-1a over every entry's name and size, in directory order. Working
 * out the fingerprint only walks the two directories (no file reads);
 * if it matches the cache, the lists come straight from the cache.
 * Adding, removing, renaming or replacing a file changes it.
 *
 * The cache is a plain dump, versioned by its magic:
 *   magic[8] | fonts fingerprint | schemes fingerprint | font count |
 *   scheme count (u32, little-endian, as the RP2350 stores them) |
 *   fonts (struct setup_card_font each) | schemes (name[32] each)
 */
#define CACHE_DIR	"/veetee"
#define CACHE_PATH	"/veetee/catalog.bin"
#define CACHE_MAGIC	"VTCAT01"	/* bump if the layout changes */
#define PROGRESS_EVERY	32		/* entries between progress lines */

struct cache_header {
	char		magic[8];
	uint32_t	fonts_print;
	uint32_t	schemes_print;
	uint32_t	font_count;
	uint32_t	scheme_count;
};

struct scan {
	card_progress_fn progress;
	void		*ctx;
	const char	*what;		/* "/fonts" or "/schemes" */
	unsigned	 seen;
	uint32_t	 print;		/* fingerprint being built */
};

static void
report(struct scan *scan, const char *format, unsigned a, unsigned b)
{
	char text[64];

	if (scan->progress == nullptr)
		return;
	snprintf(text, sizeof(text), format, a, b);
	scan->progress(text, scan->ctx);
}

/* Every PROGRESS_EVERY entries: "Reading /fonts: 64". */
static void
tick(struct scan *scan)
{
	char text[64];

	board_watchdog_feed();
	if (++scan->seen % PROGRESS_EVERY != 0 || scan->progress == nullptr)
		return;
	snprintf(text, sizeof(text), "Reading %s: %u", scan->what, scan->seen);
	scan->progress(text, scan->ctx);
}

static bool
fingerprint_entry(const char *name, uint32_t size, bool is_dir,
    const uint8_t *head, size_t head_len, void *ctx)
{
	struct scan *scan = ctx;

	(void)head;
	(void)head_len;
	(void)is_dir;
	tick(scan);
	for (const char *c = name; *c != '\0'; c++)
		scan->print = (scan->print ^ (uint8_t)*c) * 16777619u;
	for (int shift = 0; shift < 32; shift += 8)
		scan->print = (scan->print ^ ((size >> shift) & 0xff)) *
		    16777619u;
	return true;
}

static uint32_t
fingerprint(struct scan *scan, const char *dir)
{
	scan->what = dir;
	scan->seen = 0;
	scan->print = 2166136261u;
	if (!storage_list(dir, 0, fingerprint_entry, scan))
		return 0;	/* no such directory */
	return scan->print;
}

static bool
add_font(const char *name, uint32_t size, bool is_dir, const uint8_t *head,
    size_t head_len, void *ctx)
{
	struct setup_card_font *font = &fonts[font_count];

	tick(ctx);
	if (is_dir || size < PSF2_HEADER || head_len < PSF2_HEADER ||
	    !stem_of(name, ".psf", font->name, sizeof(font->name)))
		return true;
	/* Only list what could be a font; card_font() validates fully. */
	if (read_le32(head) == PSF2_MAGIC && read_le32(head + 24) <= 255 &&
	    read_le32(head + 28) <= 255) {
		font->height = (uint8_t)read_le32(head + 24);
		font->width = (uint8_t)read_le32(head + 28);
		font_count++;
	}
	return font_count < CARD_FONTS_MAX;
}

static bool
add_scheme(const char *name, uint32_t size, bool is_dir, const uint8_t *head,
    size_t head_len, void *ctx)
{
	(void)head;
	(void)head_len;
	tick(ctx);
	if (is_dir || size == 0 || size > CARD_SCHEME_MAX_BYTES)
		return true;
	if (stem_of(name, ".yaml", schemes[scheme_count], SETTINGS_SCHEME_MAX) ||
	    stem_of(name, ".yml", schemes[scheme_count], SETTINGS_SCHEME_MAX))
		scheme_count++;
	return scheme_count < CARD_SCHEMES_MAX;
}

static int
compare_fonts(const void *a, const void *b)
{
	return strcasecmp(((const struct setup_card_font *)a)->name,
	    ((const struct setup_card_font *)b)->name);
}

static int
compare_schemes(const void *a, const void *b)
{
	return strcasecmp(a, b);
}

/* Read exactly `len` bytes, or fail. */
static bool
read_all(int handle, void *buf, size_t len)
{
	return storage_read(handle, buf, len) == (long)len;
}

static bool
load_cache(const struct cache_header *want)
{
	struct cache_header have;
	int handle = storage_open(CACHE_PATH, STORAGE_READ);
	bool ok;

	if (handle < 0)
		return false;
	ok = read_all(handle, &have, sizeof(have)) &&
	    memcmp(have.magic, want->magic, sizeof(have.magic)) == 0 &&
	    have.fonts_print == want->fonts_print &&
	    have.schemes_print == want->schemes_print &&
	    have.font_count <= CARD_FONTS_MAX &&
	    have.scheme_count <= CARD_SCHEMES_MAX &&
	    read_all(handle, fonts, have.font_count * sizeof(*fonts)) &&
	    read_all(handle, schemes, have.scheme_count * sizeof(*schemes));
	storage_close(handle);
	if (ok) {
		font_count = have.font_count;
		scheme_count = have.scheme_count;
		/* Names came off the card: make sure they're terminated. */
		for (unsigned i = 0; i < font_count; i++)
			fonts[i].name[sizeof(fonts[i].name) - 1] = '\0';
		for (unsigned i = 0; i < scheme_count; i++)
			schemes[i][SETTINGS_SCHEME_MAX - 1] = '\0';
	}
	return ok;
}

/* Best effort: a write-protected or full card just means no cache. */
static void
save_cache(const struct cache_header *header)
{
	struct cache_header out = *header;
	int handle;

	out.font_count = font_count;
	out.scheme_count = scheme_count;
	if (!storage_mkdir(CACHE_DIR))
		return;
	handle = storage_open(CACHE_PATH, STORAGE_CREATE);
	if (handle < 0)
		return;
	/*
	 * A short write leaves a cache whose counts don't match its length;
	 * load_cache() then fails its reads and the next SET-UP rescans.
	 */
	(void)(storage_write(handle, &out, sizeof(out)) == (long)sizeof(out) &&
	    storage_write(handle, fonts, font_count * sizeof(*fonts)) ==
	    (long)(font_count * sizeof(*fonts)) &&
	    storage_write(handle, schemes, scheme_count * sizeof(*schemes)) ==
	    (long)(scheme_count * sizeof(*schemes)));
	(void)storage_close(handle);
}

bool
card_scan(struct setup_catalogue *catalogue, card_progress_fn progress,
    void *ctx)
{
	struct scan scan = {.progress = progress, .ctx = ctx};
	struct cache_header header = {.magic = CACHE_MAGIC};
	bool cached;

	memset(catalogue, 0, sizeof(*catalogue));
	card_release();
	if (!storage_mount())
		return false;
	fonts = calloc(CARD_FONTS_MAX, sizeof(*fonts));
	schemes = calloc(CARD_SCHEMES_MAX, sizeof(*schemes));
	if (fonts == nullptr || schemes == nullptr) {
		card_release();
		report(&scan, "Card: not enough memory to list it", 0, 0);
		return false;
	}

	report(&scan, "Checking /fonts and /schemes...", 0, 0);
	header.fonts_print = fingerprint(&scan, "/fonts");
	header.schemes_print = fingerprint(&scan, "/schemes");
	cached = load_cache(&header);
	if (!cached) {
		scan.seen = 0;
		scan.what = "/fonts";
		(void)storage_list("/fonts", PSF2_HEADER, add_font, &scan);
		scan.seen = 0;
		scan.what = "/schemes";
		(void)storage_list("/schemes", 0, add_scheme, &scan);
		qsort(fonts, font_count, sizeof(*fonts), compare_fonts);
		qsort(schemes, scheme_count, sizeof(*schemes), compare_schemes);
		report(&scan, "Saving the list for next time...", 0, 0);
		save_cache(&header);
	}
	catalogue->font_count = font_count;
	catalogue->fonts = fonts;
	catalogue->scheme_count = scheme_count;
	catalogue->schemes = (const char (*)[SETTINGS_SCHEME_MAX])schemes;
	report(&scan, cached ? "Card: %u fonts, %u schemes (from cache)" :
	    "Card: %u fonts, %u schemes (scanned)", font_count, scheme_count);
	return true;
}

void
card_release(void)
{
	free(fonts);
	free(schemes);
	fonts = nullptr;
	schemes = nullptr;
	font_count = scheme_count = 0;
}

/* --- loading ---------------------------------------------------------- */

const struct font_builtin *
card_font(const char *name, const struct font_builtin *active, char *error,
    size_t len)
{
	char path[24 + SETTINGS_FONT_MAX];
	struct font trial;
	size_t size;
	uint8_t *data;

	/* Already loaded (e.g. SET-UP re-applied the same settings)? */
	for (unsigned i = 0; i < 2; i++)
		if (slots[i].data != nullptr && strcmp(slot_names[i], name) == 0)
			return &slots[i];
	if (!storage_mount()) {
		snprintf(error, len, "%s", storage_error());
		return nullptr;
	}
	snprintf(path, sizeof(path), "/fonts/%s.psf", name);
	data = read_file(path, CARD_FONT_MAX_BYTES, &size, error, len);
	if (data == nullptr)
		return nullptr;

	/* Parse once now, so a bad file gets a reason, not a blank screen. */
	switch (font_load_psf2(&trial, data, (unsigned)size)) {
	case FONT_OK:
		font_free(&trial);
		break;
	case FONT_ERR_SIZE:
		free(data);
		snprintf(error, len, "cell size the renderer can't draw");
		return nullptr;
	case FONT_ERR_NOMEM:
		free(data);
		snprintf(error, len, "not enough memory");
		return nullptr;
	default:
		free(data);
		snprintf(error, len, "not a PSF2 font (or damaged)");
		return nullptr;
	}

	/* Reuse the slot the terminal isn't drawing with. */
	struct font_builtin *slot = &slots[active == &slots[0] ? 1 : 0];
	unsigned index = (unsigned)(slot - slots);
	free((void *)slot->data);
	snprintf(slot_names[index], sizeof(slot_names[index]), "%s", name);
	slot->name = slot_names[index];
	slot->data = data;
	slot->len = (unsigned)size;
	return slot;
}

bool
card_scheme(const char *name, struct base16_terminal *colours, char *error,
    size_t len)
{
	char path[24 + SETTINGS_SCHEME_MAX];
	struct base16 scheme;
	const char *reason;
	size_t size;
	char *text;

	if (!storage_mount()) {
		snprintf(error, len, "%s", storage_error());
		return false;
	}
	snprintf(path, sizeof(path), "/schemes/%s.yaml", name);
	if (!storage_exists(path))
		snprintf(path, sizeof(path), "/schemes/%s.yml", name);
	text = (char *)read_file(path, CARD_SCHEME_MAX_BYTES, &size, error,
	    len);
	if (text == nullptr)
		return false;
	bool ok = base16_parse(text, size, &scheme, &reason);
	free(text);
	if (!ok) {
		snprintf(error, len, "%s", reason);
		return false;
	}
	base16_to_terminal(&scheme, colours);
	return true;
}

/*
 * card.h — what the VeeTee reads from its SD card (app-private).
 *
 *   /fonts/<name>.psf      PSF2 fonts (tools/ttf2psf.py makes them)
 *   /schemes/<name>.yaml   base16 colour schemes (tinted-theming)
 *
 * The policy layer over lib/hal/storage.h: where things live, size
 * limits, validation, and turning failures into one-line reasons for
 * the Configuration Error screen.
 */
#ifndef CARD_H
#define CARD_H

#include <stdbool.h>
#include <stddef.h>

#include "base16.h"
#include "font.h"
#include "setup.h"

#define CARD_FONT_MAX_BYTES	(64u * 1024u)	/* stays in RAM while used */
#define CARD_SCHEME_MAX_BYTES	(8u * 1024u)
/*
 * Caveat emptor: only the first 64 fonts and 64 schemes found (in
 * directory order, i.e. roughly the order they were copied) are listed;
 * the rest are ignored. tools/sd_card.py writes at most 64 of each.
 */
#define CARD_FONTS_MAX		64
#define CARD_SCHEMES_MAX	64

/*
 * List the card's fonts and schemes for SET-UP (sorted by name), from
 * the cache in /veetee/catalog.bin when the folders haven't changed. The
 * arrays live until card_release(). Without a card the lists are empty
 * and false is returned. `progress` (may be null) is told what's
 * happening as it goes ("Reading /fonts: 64").
 */
typedef void (*card_progress_fn)(const char *text, void *ctx);
bool	card_scan(struct setup_catalogue *catalogue,
	    card_progress_fn progress, void *ctx);
void	card_release(void);

/*
 * Load /fonts/<name>.psf. The returned font stays valid (its file data
 * in RAM) until a different card font is loaded twice later -- two slots
 * alternate, so the terminal's current font is never freed under it.
 * Pass the terminal's current font as `active`. On failure returns
 * nullptr and a reason in `error`.
 */
const struct font_builtin *card_font(const char *name,
	    const struct font_builtin *active, char *error, size_t len);

/* Load /schemes/<name>.yaml as terminal colours. */
bool	card_scheme(const char *name, struct base16_terminal *colours,
	    char *error, size_t len);

#endif /* CARD_H */

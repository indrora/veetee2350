/*
 * printer_sd.h — the VT printer port, printing to /print on the SD card.
 */
#ifndef PRINTER_SD_H
#define PRINTER_SD_H

#include <stdbool.h>

#include "term.h"

/* Switch the printer on (attach it to `term`) or off. */
void	printer_sd_enable(struct term *term, bool on);
/* From the main loop: close an auto-print file that's gone quiet. */
void	printer_sd_poll(void);
/* The current print file ("/print/0003.txt"), or "" before the first job. */
const char *printer_sd_path(void);

#endif /* PRINTER_SD_H */

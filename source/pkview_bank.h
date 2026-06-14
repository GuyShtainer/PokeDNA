#ifndef PKVIEW_BANK_H
#define PKVIEW_BANK_H

#include <stdbool.h>

/* The external bank, reworked into a parallel set of 16 NAMED, WALLPAPERED boxes
 * (30 slots each) persisted on the SD card — one file per box plus a small metadata
 * file — and rendered by the very same box screen as the in-save PC (pkview_box).
 * Mons move between the bank and the save through the universal copy/paste
 * clipboard, so the bank behaves exactly like another set of PC boxes.
 *
 * On first use it migrates any existing flat /pokeviewer/bank/*.pk3 files into the
 * new box files (non-destructively — the .pk3 files are left in place).
 *
 * Shows the bank screen; returns when the user backs out. Omega-only for writes;
 * read-only carts can browse + copy but not edit. */
bool pkview_bank_show(void);

#endif /* PKVIEW_BANK_H */

#ifndef PDNA_BANK_H
#define PDNA_BANK_H

#include <stdbool.h>

/* The external bank, reworked into a parallel set of 16 NAMED, WALLPAPERED boxes
 * (30 slots each) persisted on the SD card — one file per box plus a small metadata
 * file — and rendered by the very same box screen as the in-save PC (pdna_box).
 * Mons move between the bank and the save through the universal copy/paste
 * clipboard, so the bank behaves exactly like another set of PC boxes.
 *
 * On first use it migrates any existing flat /PokeDNA/bank/*.pk3 files into the
 * new box files (non-destructively — the .pk3 files are left in place).
 *
 * Shows the bank screen; returns when the user backs out. Omega-only for writes;
 * read-only carts can browse + copy but not edit. */
int pdna_bank_show(void);   /* returns the box exit code (5 = dropped off the bottom -> back to PC) */

/* Bank->PC carry is a deferred MOVE: record the bank source to delete, then apply the
 * deletions at the save phase (after the PC commit) or drop them on discard. */
void pdna_bank_defer_delete(int box, int slot);
void pdna_bank_flush_deletions(void);   /* apply pending deletions (call AFTER the PC is written) */
void pdna_bank_clear_deletions(void);   /* drop pending deletions (discard / fresh save) */

#endif /* PDNA_BANK_H */

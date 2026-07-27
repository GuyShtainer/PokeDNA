#ifndef PDNA_BANK_H
#define PDNA_BANK_H

#include <stdbool.h>
#include <stdint.h>
#include "gen3_mon.h"     /* PkMon (pdna_bank_hide_pending) */

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

/* Bank->PC (or Bank->party) carry is a deferred MOVE: record the bank source AND the carried
 * mon's 80-byte record, then apply the deletions at the save phase (after the PC commit) or
 * drop them on discard. The record is matched at flush time so a re-arrange of the bank can't
 * delete the wrong mon (a mismatch is skipped, leaving a harmless duplicate). */
void pdna_bank_defer_delete(int box, int slot, const uint8_t* rec80);
bool pdna_bank_defer_full(void);        /* deletion queue full -> refuse further Bank->PC MOVEs */
bool pdna_bank_defer_room(int n);       /* room for n more deferred deletions (a whole chunk)? */
void pdna_bank_defer_pop(int n);        /* undo the last n queued deletions (a failed/immediate move) */

/* A mon carried Bank->PC is deleted from the bank only at the save phase, but it should LOOK gone
 * right away. hide_pending blanks those slots in a DECODED box (display only — never the persisted
 * buffer); slot_pending reports that a slot still physically holds a moving-out mon, so nothing may
 * overwrite it before the PC destination is saved. */
void pdna_bank_hide_pending(int box, PkMon g[30]);
bool pdna_bank_slot_pending(int box, int slot);

/* Clear identity-matched slots in a bank box and persist it (verified). Refuses (returns false,
 * file untouched) if the box's page-in read was incomplete or nothing matched. Cross-box MOVE only,
 * and ONLY after the destination box is committed. */
bool pdna_bank_clear_slots(int box, const uint8_t* slots, const uint8_t (*recs80)[80], int n);
void pdna_bank_flush_deletions(void);   /* apply pending deletions (call AFTER the PC is written) */
void pdna_bank_clear_deletions(void);   /* drop pending deletions (discard / fresh save) */

#endif /* PDNA_BANK_H */

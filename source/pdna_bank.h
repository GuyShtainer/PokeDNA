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
int  pdna_bank_flush_deletions(void);   /* apply pending deletions (call AFTER the PC is written);
                                          * BACKLOG #150 S150-8 decision 10: returns the count still
                                          * queued after a failed box_save() (0 = all flushed) */
void pdna_bank_clear_deletions(void);   /* drop pending deletions (discard / fresh save) */

/* BACKLOG #163: true while `box` is the one box_save() most recently refused to write --
 * cleared the moment that SAME box saves clean. pdna_box.c's Bank header painter polls
 * this to draw a persistent "BOX NOT SAVED" banner (box_save runs with no grid on screen,
 * review G4, so it cannot draw its own). At most one box at a time: a different box's own
 * unsaved marker was already true before this one and is unaffected. */
bool pdna_bank_box_unsaved(int box);

/* BACKLOG #150 S150-4 decision 2: allocate + persist (BEFORE returning) the next
 * bank_serial a native cell's bc_pack() needs. 0 = refuse the lift (a meta write
 * failure); otherwise non-zero and unique across every prior call that persisted. */
uint32_t pdna_bank_next_serial(void);

/* BACKLOG #223: after a bank.meta .bak rollback (BACKLOG #219) the recovered serial
 * counter can sit at or below serials already stored in the boxes -- every UP landing
 * then allocates a serial the collision scan refuses, burning one per retry forever.
 * Caller passes bank_serial_max() (source/bank_collision.h) over all 16 boxes; if the
 * live counter is not already strictly ahead of it, this raises it to stored_max + 1
 * and persists through meta_save() (verified, same idiom as pdna_bank_next_serial())
 * before returning. Returns true on a real resync (and logs "bank: serial resynced
 * N -> M"); false when nothing needed resyncing or the meta write failed (counter
 * reverted -- the caller's own collision refusal still applies, exactly as before
 * this existed). */
bool pdna_bank_serial_resync(uint32_t stored_max);

/* BACKLOG #150 S150-12 decision 8: sets the box pdna_bank_show()'s NEXT call opens on
 * (out-of-range clamps to 0); consumed and reset to 0 by that one call. Used by the
 * read-only mount's exit offer so YES opens the Bank on the box the last copy landed
 * in, cursor at cell 0 (there is no "resume this exact cell" mechanism). */
void pdna_bank_start_box_set(int box);

/* BACKLOG #150 S150-4 decision 3: the one-shot immutable pre-#150 backup gate. Call
 * BEFORE writing a native cell into the Bank; false => refuse the lift (nothing was
 * moved). O(1) once /PokeDNA/bank/backup-v1/DONE exists. */
bool pdna_bank_prepare_native(void);

/* BACKLOG #168a review D3: the Bank's box count, mirrored from pdna_bank.c's own
 * BANK_BOXES (an internal #define, not exported) so a call site like drop_held_up's
 * 16-box ident32 scan (source/pdna_box.c) never carries its own unpinned `16`
 * literal -- rewriting that literal to 1 was caught by nothing until this. The
 * _Static_assert in pdna_bank.c keeps the two in lockstep. */
#define PDNA_BANK_BOXES 16

/* BACKLOG #150 S150-11 decision 19: read box `box`'s 2400-byte raw records without
 * opening the box screen (flushes the currently-loaded box first, via the same
 * b163 verdict path banksrc_records() uses). NULL on a page-in failure or an
 * out-of-range box; the returned pointer aliases the one shared bank buffer and is
 * only valid until the next call that pages a box in. */
const uint8_t* pdna_bank_peek_box(int box);

/* BACKLOG #168: true only right after a meta_load() whose PRIMARY bank.meta parsed
 * clean this session (never after a .bak fallback or a default reset). Lets
 * drop_held()'s UP-branch ident32 collision scan (source/pdna_box.c) skip its 15
 * extra box reads when the serial a native cell's ident32 is built from is known
 * fresh -- see the doc comment on the static it mirrors in pdna_bank.c. */
bool pdna_bank_serial_trusted(void);

/* BACKLOG #150 S150-11 decision 8c/19: write a fresh native cell into an all-zero
 * slot (RESTORE TO BANK's write path -- a genuinely new Bank write, no merge).
 * Omega-only (app_can_edit() first); refuses on a non-zero target slot, an
 * out-of-range box/slot, or a failed save (the buffer's copy of the slot is
 * re-zeroed on that path so a retry starts from the same state). */
bool pdna_bank_put_cell(int box, int slot, const uint8_t cell80[80]);

#endif /* PDNA_BANK_H */

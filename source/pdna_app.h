#ifndef PDNA_APP_H
#define PDNA_APP_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_mon.h"      /* PkMon (app_bank_hide_pending) */

/* Shared app glue so the party list and box grid can open the editor and persist
 * safely. Implemented in pdna_main.c (which owns the loaded save + path). */

/* Writes are EZ-Flash-Omega-only. */
bool app_can_edit(void);

/* Flush the RAM log to SD immediately (rmbl-paused). For anomaly evidence that must
 * survive a power-off; main-loop-synchronous callers only. */
void app_log_flush(void);

/* How an edited in-RAM `block` is persisted. Each kind of block (PC storage,
 * SaveBlock1, SaveBlock2, a bank box file) supplies its own verified-write
 * function; the mon menu / editors mutate `block` in RAM then call this to save.
 * Returns true iff a write happened. */
typedef bool (*AppCommitFn)(void);

/* Gen-3-PC-style action menu shown on A: SUMMARY / ITEM / MOVES / COPY / PASTE /
 * DUPLICATE / RELEASE on Omega (an empty slot offers PASTE only); straight to the
 * read-only summary on Everdrive. `block` is the pc-layout buffer the slot lives in
 * and `box`/`slot` locate the record within it (box = -1, slot = party index for
 * party callers). `commit` persists `block`. `is_bank` swaps the EXPORT action for
 * "TO GAME" (inject into the loaded save). Returns true iff a write happened. */
bool app_mon_menu(uint8_t* rec, bool is_party, bool is_bank, AppCommitFn commit, uint8_t* block, int box, int slot);

/* Bank "Copy to game": inject a stored 80-byte box record into the loaded save's
 * first free PC box slot (and commit). Returns true iff written. Omega-only. */
bool app_inject_to_game(const uint8_t* rec80);

/* The A-menu's MOVE action sets a one-shot flag; the box grid consumes it to
 * enter "pick up + reposition" mode. Returns true once per MOVE pick. */
bool app_take_move_request(void);

/* The A-menu's DUPLICATE action (box/bank only) sets a one-shot flag; the box grid
 * consumes it to copy the selected mon into a free slot and pick that copy up in the
 * glove, so the user positions it. Returns true once per DUPLICATE pick. */
bool app_take_dup_request(void);

/* A Day-Care withdraw-to-PC parks the mon in a free PC slot and sets a pending pickup;
 * the box grid consumes it on entry to open that box carrying the mon in the glove.
 * Returns true once and fills *box/*slot. */
bool app_take_pickup(int* box, int* slot);

/* Entry-cursor hint for the next box grid: 0 default, 1 = start on the top tabs,
 * 2 = start at the bottom row (used by the PC<->Bank up/down hand-off). */
void app_box_start_set(int s);
int  app_box_start_take(void);
void app_note_pc_box(int b);              /* PC box screen reports its current box so the app remembers it */
/* Party overlay popped from the box screen's PARTY tab (Gen-4/5-style "move to/from party").
 * PLACE mode (held != NULL): carrying a box mon -> A on a slot ADDS/SWAPS it into the party
 * (orig_box/orig_slot/orig_bank = the held mon's origin; can_swap = it has a clean PC origin
 * to receive a swapped-out member). Returns 1 (placed -> caller ends the carry) or 0.
 * GRAB mode (held == NULL): empty-handed -> A on a party mon picks it UP to move to a box:
 * fills grab80 (80-byte box form) + *grab_slot (party index) and returns 2; 0 = closed. */
int  app_party_overlay(const uint8_t* held, int orig_box, int orig_slot, bool orig_bank,
                       bool can_swap, uint8_t grab80[80], int* grab_slot, bool allow_move_to_box);
/* Remove party slot `idx` (gap-free) as a DEFERRED move (party -> box): stage SB1 + mark PC
 * dirty so it folds into the one exit save, and refresh the cached party. Called by the box
 * grid only when a carried party mon is successfully DROPPED into a box (lift-don't-clear). */
void app_party_remove_at(int idx);
/* Bank->PC / Bank->party carry: record the bank source slot AND the carried 80-byte record to
 * delete at the save phase (true move; matched by record so a re-arrange can't delete the wrong mon). */
void app_bank_defer_delete(int box, int slot, const uint8_t* rec80);
bool app_bank_defer_full(void);   /* 64-move queue full -> refuse the move (a silent drop would DUP) */
bool app_bank_defer_room(int n);  /* room for a whole chunk of n deferred deletions? (Bank->PC multi-move) */
void app_bank_defer_pop(int n);   /* undo the last n queued deletions (revert a Bank->PC move on write failure) */
void app_bank_flush_deletions(void);  /* delete the queued Bank sources NOW — call ONLY after the PC destination is verified on disk */

/* A mon carried Bank->PC is deleted from the bank only at the save, but must LOOK gone at once.
 * hide_pending blanks those slots in a DECODED box (display only); slot_pending says a slot still
 * physically holds a moving-out mon (its only on-card copy) so nothing may overwrite it yet.
 * clear_slots does the cross-box MOVE's source clear+commit, refusing on an incomplete page-in. */
void app_bank_hide_pending(int box, PkMon g[30]);
bool app_bank_slot_pending(int box, int slot);
bool app_bank_clear_slots(int box, const uint8_t* slots, const uint8_t (*recs80)[80], int n);

/* Commit the loaded save's SaveBlock2 (section 0 — the trainer block) or
 * SaveBlock1 (sections 1..4 — where money lives) after an in-place edit of the
 * shared g_sb2 / g_sb1 buffers. Same verified-write+backup path as the editors.
 * Returns true iff a write happened. Used by the editable trainer card. */
bool app_commit_sb2(void);
bool app_commit_sb1(void);
bool app_commit_sb12(void);               /* SB2 + SB1 (sections 0..4) in one verified write */
bool app_commit_pc(void);                 /* PC storage (sections 5..13): box name/wallpaper */

/* Deferred-save for PC box MOVES. Repositioning mons in move-mode mutates g_pc in
 * RAM but does NOT write immediately (no per-drop "Saving" dialog); it marks the PC
 * dirty instead. The single "save the Pokemon you moved?" prompt fires when the
 * user leaves the open save (B out of the box/party screen). All OTHER PC edits
 * still commit immediately — and any such commit clears the dirty flag, since the
 * verified write flushes the whole g_pc (pending moves included). */
void app_mark_pc_dirty(void);
bool app_pc_dirty(void);

/* PC->Bank MOVE support: clear a PC box slot (release from the save) after the destination
 * bank box has been verified on SD, matched by the mon's 8-byte identity so a bystander is
 * never zeroed. Marks the PC dirty (committed at the one exit save). Omega-only in effect
 * (the caller gates on can_edit + only reaches this after a successful bank write). */
void app_pc_release_slot(int box, int slot, const uint8_t* id8);

/* Emerald "Walda" secret wallpaper: the graphic shown by box wallpaper 16. Pattern
 * is 0..15 (sWaldaWallpapers index) in SaveBlock1. app_walda_pattern returns -1 on
 * non-Emerald; app_set_walda edits g_sb1 in place (commit via app_commit_sb1). */
int  app_walda_pattern(void);
bool app_set_walda(uint8_t pattern);

/* Shared framed yes/no confirm (A = yes, B = no). */
bool app_confirm(const char* title, const char* l1);

/* Per-place "moving sprites" toggles (Settings, persisted in config.cfg). Each
 * screen's idle bob/wiggle is gated on its own flag, so the user can calm one place
 * without killing the rest. Box/Party/Dex/Daycare default ON; the summary portrait
 * wiggle defaults OFF. Read by pdna_box / pdna_main (party + daycare) / pdna_pick
 * (dex) / pdna_summary. */
enum { ANIM_BOX,        /* PC + bank box-icon 2-frame bob   */
       ANIM_PARTY,      /* party list sprite bob            */
       ANIM_DEX,        /* Pokedex caught-cell bob          */
       ANIM_DAYCARE,    /* daycare scene mon bob            */
       ANIM_SUMMARY,    /* summary-card portrait wiggle     */
       ANIM_COUNT };
bool app_anim_enabled(int kind);   /* true iff animations are on for ANIM_<kind> */

#endif /* PDNA_APP_H */

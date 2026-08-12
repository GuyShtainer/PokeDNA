#ifndef PDNA_APP_H
#define PDNA_APP_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_mon.h"      /* PkMon (app_bank_hide_pending) */
#include "gen3_trainer.h"  /* PkGame (app_rom_path)         */

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

/* ---- read-only BoxSource gate ------------------------------------------------
 * Editability has TWO axes and app_mon_menu used to know only one of them:
 *   - the CART (app_can_edit: writes are EZ-Flash-Omega-only), and
 *   - the SOURCE (BoxSource.can_edit: a mounted Gen-1/2 save is read-only no matter
 *     what cart it is running on — the conversion is one-way and the GB file is
 *     never written).
 * On an Omega, a read-only source therefore got the FULL menu: PASTE / RELEASE /
 * DUPLICATE / CREATE would mutate the source's RAM buffer, its commit() would
 * no-op, and the screen would show a change that never happened. pdna_box already
 * gates its own destructive paths on src->can_edit(); app_mon_menu could not,
 * because it is handed a record and a commit function, not the source.
 *
 * So a source that is read-only *as a source* registers itself for the duration of
 * its pdna_box() run. While registered:
 *   - app_mon_menu offers only VIEW / LEGALITY / COPY (+ the reason a specific
 *     record cannot even be copied), and
 *   - the singleton bank's deferred-delete bookkeeping (app_bank_hide_pending /
 *     app_bank_slot_pending) is skipped, because a foreign source's boxes are not
 *     the bank's boxes and a queued Bank->PC deletion must never hide one of their
 *     Pokemon.
 * Nothing registers by default, so every existing call site behaves exactly as
 * before. `why_locked(rec80)` returns NULL when that record may be copied, else a
 * short reason to show; `note` is a short line shown under the menu title (e.g.
 * "Converted copy"). Both may be NULL. */
void app_src_readonly_set(const char* (*why_locked)(const uint8_t* rec80), const char* note);
void app_src_readonly_clear(void);
bool app_src_readonly(void);

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

/* ---- borrowed EWRAM arena (the map screen) ---------------------------------
 * EWRAM has only ~6 KB genuinely free, and the map viewer needs ~33 KB for a
 * decompressed tileset. Rather than adding a buffer that would not link, the map
 * screen BORROWS `g_pc` — the reassembled PC storage. That buffer is the right
 * donor for three reasons: it is a single CONTIGUOUS 35,712-byte block (the other
 * candidates are scattered across translation units and cannot form one arena), it
 * is *derived* from g_save so it can be rebuilt byte-exactly on release, and the
 * map screen has no use for PC boxes.
 *
 * Deliberately NOT donors: g_save / g_sb1 / g_sb2 — the map screen must read g_sb1
 * for the player's position and write through g_save to commit a teleport, so
 * aliasing those is exactly the hard-rule-3 failure mode.
 *
 * app_arena_acquire() FAILS (returns NULL) when the PC is dirty, because unsaved box
 * moves live only in g_pc and would be destroyed. Callers must handle NULL by telling
 * the user to save first — never by proceeding.
 * app_arena_release() re-derives g_pc from g_save. */
#define APP_ARENA_BYTES (9 * 3968)          /* == G3_PC_BYTES, 35712 */
uint8_t* app_arena_acquire(uint32_t need);  /* NULL: too big, already held, or PC dirty */
void     app_arena_release(void);
bool     app_arena_held(void);

/* Path of the Pokemon ROM the map screen reads map data from, remembered per game
 * (RS / Emerald / FRLG) because their map data differs and each needs its own ROM.
 * Returns "" when none has been picked yet. Set persists via config.cfg. */
/* ---- descriptions: the user's ROM first, the embedded table only as a fallback ----
 * Item/move/ability description TEXT is the games' own prose, and shipping it verbatim
 * is the largest remaining legal liability in the binary (741 strings). These three
 * read it from the registered/fused ROM when one is available, and fall back to
 * whatever the build has otherwise — so a ROM-gated build can drop the embedded copy
 * entirely without any screen losing its layout.
 *
 * The returned pointer is valid until the NEXT call of the same function (each keeps
 * one small static buffer). Copy it if you need to hold it. Never NULL. */
const char* app_item_desc(uint16_t item_id);
const char* app_move_desc(uint16_t move_id);
const char* app_ability_desc(uint16_t ability_id);

const char* app_rom_path(PkGame game);
void        app_rom_path_set(PkGame game, const char* path);
/* True once the user has registered ANY of their own game ROMs. The gate for extras
 * that need real art to mean anything (Day-Care yard visitors). */
bool        app_any_rom_registered(void);

#endif /* PDNA_APP_H */

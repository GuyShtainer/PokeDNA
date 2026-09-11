#ifndef PDNA_PICK_H
#define PDNA_PICK_H

#include <stdint.h>
#include <stdbool.h>

/* Rich pickers for the editor. Each returns the chosen id, or 0xFFFF if the user
 * cancelled (B). `current` pre-selects the starting entry.
 *
 * pick_species: HGSS-style icon grid with live search (SELECT -> keyboard),
 *   filter (L: All / Gen1-3 / Legendary / by-Type) and sort (R: No. / A-Z).
 * pick_move:    list with the move's type chip, power/accuracy/PP, description,
 *   a type filter (L) and search (SELECT).
 * pick_item / pick_nature: searchable lists. */
uint16_t pick_species(uint16_t current_internal);
/* Restricts the NEXT pick_species() call's list to National Dex 1..max_dex
 * (0 = unrestricted, the default every existing caller sees) -- the Gen-1/2
 * create flow's own use, so a Gen-1 session is never offered a Gen-2+
 * species (BACKLOG #50 UX-parity, source/pdna_pick.c's own header comment
 * on g_species_max_dex has the full design). Set it right before the call
 * and clear it (pass 0) right after -- it is a file-static and will
 * otherwise leak into the next, unrelated pick_species() caller. */
void     pick_species_set_max_dex(uint16_t max_dex);
uint16_t pick_move(uint16_t current_move);
uint16_t pick_item(uint16_t current_item);
/* Restricts the NEXT pick_item() call to ids 1..max_id shown as "#n" (no real
 * name/description/icon -- 0 = unrestricted, the default every existing
 * caller sees). The Gen-1/2 held-item field's own use (UX-parity audit, Guy
 * 2026-09-07); see source/pdna_pick.c's header comment on g_item_max_id for
 * why this is not merely a ceiling the way pick_species_set_max_dex() is.
 * Set it right before the call and clear it (pass 0) right after. */
void     pick_item_set_gen1_2_max(uint16_t max_id);
uint8_t  pick_nature(uint8_t current_nature);
int      pick_unown_form(int current_form);     /* 0..27 = A..?, -1 cancel */

/* Poke Ball picker. Gen 3 keeps the ball in four bits, and the twelve balls are item ids
 * 1..12, so this shows the item name + icon + blurb for each. Returns the chosen ball
 * (1..12), or `current` on cancel — the field can never hold anything else. */
uint8_t pick_ball(uint8_t current);

/* Met-location picker: a searchable, sortable list over gen3_places.c.
 *   metgame — the record's origin byte; it SCOPES the list (a FireRed record is not
 *             offered Hoenn), and 0 / Colo-XD / garbage means "don't narrow".
 *   region0 — a G3_RGN_* to open scoped to, or <0 for all regions.
 * Returns the chosen location id, or 0xFFFF on cancel. */
uint16_t pick_metloc(uint16_t current, uint8_t metgame, int region0);

/* Region chooser (Hoenn / Kanto / Sevii Isles / Special), showing how many places each
 * offers THIS origin game. Returns a G3_RGN_*, or -1 on cancel. */
int pick_region(int current, uint8_t metgame);

/* Ability picker. Gen-3 stores only a 1-bit ability SLOT, so the choices are the
 * species' two abilities (shown by name + description). Returns the chosen slot
 * (0 or 1), or `current` on cancel. */
uint8_t  pick_ability(uint16_t species_internal, uint8_t current_slot);

/* ---- Pokedex viewer / editor ---------------------------------------------
 * HGSS-style Pokedex built on the same species grid + filters. The dex flags
 * live in the loaded save (pdna_main owns the buffers), so the screen reaches
 * them through these callbacks. State: 0 = unseen, 1 = seen, 2 = caught. */
typedef int  (*DexGetState)(int nat);          /* National no. (1..386) -> 0/1/2 */
typedef void (*DexSetState)(int nat, int state);
/* National Dex unlock hooks: reading/writing the magic+var+flag trio so the in-game
 * dex actually shows #152..386. getnat returns whether national is currently live;
 * setnat turns it on/off. The DEX:ALL menu shows a toggle and auto-enables on Catch ALL.
 * Both may be NULL (then the toggle is hidden). */
typedef bool (*DexGetNat)(void);
typedef void (*DexSetNat)(bool on);

/* Run the Pokedex screen. Three views (Grid / List / by-Type), the species-grid
 * filters (Gen/type/legendary) + a caught/seen/unseen status filter + name search,
 * and per-state sprite rendering (greyscale unseen, colour seen, colour+bob caught
 * with a Poke-Ball marker). When `can_edit`, A cycles a species unseen->seen->caught
 * and a "Mark all" bulk op is offered. Returns true iff any dex state changed (so
 * the caller can offer to save). */
bool pdna_dex_screen(DexGetState get, DexSetState set,
                     DexGetNat getnat, DexSetNat setnat, bool can_edit);

/* ---- pick_rows: the generic searchable/sortable row-list engine (BACKLOG #107) ---
 * list_pick's own loop (source/pdna_pick.c), extracted so any leaf picker screen can
 * get the same chrome (dirty-row repaint, L/R paging, SELECT search, START A-Z sort)
 * without a second copy of it. Full contract in pdna_pick.c's header comment on
 * pick_rows -- read it before calling this from a new screen, especially the ROW
 * REPAINT RULE (`row` owns its whole rect) and the ctx-not-statics convention.
 *
 * `n`      -- how many underlying items (0..n-1) exist; `row`/`search_key` are keyed
 *   (when PR_SORTABLE is set, search_key's return must fit pr_build's 32-B stable-copy buffer in
 *   pdna_pick.c -- a longer key still sorts, just on a truncated prefix; b107 review A3)
 *             by that same index, never by a filtered display position.
 * `current`-- which underlying id to preselect (its list POSITION after any default
 *             filter/sort, same as list_pick's `current`).
 * `row`    -- draws item `i` at screen y, `sel` = currently highlighted.
 * `ctx`    -- opaque, handed back to `row`/`search_key` unchanged; hold per-call
 *             state here (a local struct's address), not a new file-static.
 * `search_key` -- NULL disables search AND sort; else the ci_contains() search
 *             target and (PR_SORTABLE) the A-Z sort key.
 * `opts`   -- PR_SORTABLE, and exactly one of PR_ROWH9 (FILT: 9 px pitch, 15 vis,
 *             full-page L/R) / PR_ROWH26 (24x24-icon rows: 26 px pitch, 5 vis, 5
 *             page) / neither (plain 8 px pitch, 16 vis, 10 page -- list_pick's own
 *             non-icon default).
 * Returns the chosen id (0..n-1), or -1 on B. */
enum { PR_SORTABLE = 1, PR_ROWH9 = 2, PR_ROWH26 = 4 };
int pick_rows(const char* title, int n, int current,
             void (*row)(int i, int y, bool sel, void* ctx), void* ctx,
             const char* (*search_key)(int i, void* ctx), int opts);
/* Species cap for the shared dex screen above (BACKLOG #87): species with a
 * National no. past `max_dex` are hidden from every view, excluded from the
 * seen/caught header counts, and skipped by the bulk Catch/See/Wipe-ALL loops.
 * Default (and the value on 0/out-of-range input) is the full 386 — Gen-3 callers
 * must call this with 386 on EVERY entry (never rely on a previous reset) so a
 * prior Gen-1/2 visit's 151/251 cap cannot leak into a Gen-3 session. */
void pdna_dex_set_max(int max_dex);

#endif /* PDNA_PICK_H */

#ifndef PDNA_PICK_H
#define PDNA_PICK_H

#include <stdint.h>
#include <stdbool.h>

#include "gb_bag.h"   /* GbBagPocket -- pick_item_set_gen1_2_cat's parameter    */

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
 * Set it right before the call and clear it (pass 0) right after.
 *
 * BACKLOG #195: a thin wrapper over pick_item_set_gen1_2(0, GBF_G_RED,
 * max_id) -- kept so the held-item callers (pdna_gbedit.c, pdna_gen12.c)
 * that want the OLD "#n" raw-byte behaviour (no name table: a held item is
 * any raw byte 0..255, no legality gate) need no change. */
void     pick_item_set_gen1_2_max(uint16_t max_id);
/* BACKLOG #195: like pick_item_set_gen1_2_max(), but `gen` (GBIN_GEN1/
 * GBIN_GEN2, gb_item_names.h) also turns on REAL names (gb_item_label) and a
 * pocket CATEGORY filter (gbb_pocket_of) in the restricted picker -- see
 * pdna_pick.c's header comment on g_item_max_id/g_item_gen for the full
 * behaviour matrix. `gen` 0 is the old raw "#n" mode (identical to
 * pick_item_set_gen1_2_max()); GBIN_GEN1/GBIN_GEN2 are the Gen-1/2 bag/pack
 * ADD ITEM sites' own use.
 *
 * Review D2: `game` is the ACTUAL GbGame (GBF_G_RED/YELLOW for Gen-1 callers,
 * the session's own GBF_G_GS/GBF_G_CRYSTAL for Gen-2 callers), not merely
 * the generation -- gbb_pocket_of() answers differently for Gold/Silver vs
 * Crystal at four ids (gb_bag.h's own header comment has the derivation), so
 * this picker needs the real game to filter correctly, same as gbb_insert()
 * and every other gb_bag.h entry point already takes GbGame, not a "which
 * generation" shorthand.
 *
 * Set right before the call, clear (0, GBF_G_RED, 0) right after -- same
 * one-shot-per-call contract as every other picker restrictor here. */
void     pick_item_set_gen1_2(int gen, GbGame game, uint16_t max_id);
/* BACKLOG #195: primes the NEXT pick_item() call's STARTING category filter
 * when gen1_2 mode is active (ignored otherwise) -- GBB_POCKET_ITEMS/KEY/
 * BALLS/TMHM opens pre-filtered to that pocket's category; GBB_POCKET_COUNT
 * (or any value pick_item()'s own gen doesn't offer as a category, e.g.
 * GBB_POCKET_PC) opens on "All". Auto-consumed (reset to GBB_POCKET_COUNT)
 * the instant pick_item() reads it, so -- unlike the ceiling above -- a
 * caller does NOT need to clear it after the call. */
void     pick_item_set_gen1_2_cat(GbBagPocket pocket0);
/* BACKLOG #189: restricts the NEXT pick_move() call's list to ids 1..max_id (0 =
 * unrestricted, the default every existing caller sees). Mirrors
 * pick_item_set_gen1_2_max() exactly: file-scope static, consulted in build_moves(),
 * set right before the call and cleared (pass 0) right after so it never leaks into
 * the next, unrelated pick_move() caller. Unlike the item ceiling, move names/data
 * ARE real at every id shown (pk_move_name/pk_move_power/etc. already cover the
 * whole NMOVE range) -- this ceiling exists to hide moves the mon's OWN generation
 * cannot learn (gb_max_move(gen): 165 for Gen 1, 251 for Gen 2), not because the
 * data would be wrong, so nothing else about the row/detail rendering changes. */
void     pick_move_set_gen_max(uint16_t max_id);
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

/* BACKLOG #124: process-wide optional cell-art override, same style as pdna_box.c's
 * own pdna_box_xfer_set() (a file-static pointer a caller installs around its own
 * call, never a new pdna_dex_screen() parameter -- adding one would touch every
 * caller's signature, including the Gen-3 one, for a feature the Gen-3 dex never
 * uses). Set by a caller that can paint a species' 32x32 dex cell from another art
 * source (the GB session's own ROM sprites) instead of the icon store / mon_icon_for
 * ladder dex_cell_grid() uses today. `dex` is the NATIONAL dex number (1..251),
 * matching pdna_origin_art.h's PdnaGbArtSource.icon() convention -- NOT the internal
 * species id g_list stores. `x,y,w,h` is the cell rect dex_cell_grid() would have
 * painted (currently always 32x32 -- passed rather than hardcoded so a future view
 * geometry change does not silently mismatch this contract). Returns true iff it
 * painted the whole cell (icon + any of its own chrome); false means "could not serve
 * this species right now" and dex_cell_grid() falls back to its existing path exactly
 * as if no override were installed. NULL (the default) restores today's behaviour.
 * dex_cell_grid() only calls this for a species already at seen/caught state (never
 * for an unseen one, matching the "reveal nothing new" posture the artless grey icon
 * already keeps for state 0) and never from the per-tick bob-animation refresh (that
 * path is a per-frame repaint; a GB ROM fetch must never sit on one -- see
 * pdna_box.c's era_cell_draw's own comment on the same rule) -- a page served by the
 * override simply does not bob. The installer must clear this (NULL) on every exit
 * path before pdna_dex_screen() returns control past it, including on error/STOP; a
 * stale pointer left installed would be called for the NEXT screen that opens
 * pdna_dex_screen() (the ordinary Gen-3 one) with a `ctx` that may no longer be
 * valid.
 *
 * `serves_page` (review A5 follow-up, cross-review finding): a PAGE-LEVEL answer to
 * "will this override actually serve ANY cell on this visit", supplied by the
 * INSTALLER, not re-derived here. dex_declare_page()'s icon-store-plan skip and the
 * bob-animation loop's per-cell skip both need this same yes/no question, and it is
 * NOT simply "is a Gen-2 ROM registered": pdna_origin_art_have(PDNA_GEN2) is a
 * boot-sticky global (Settings can register a Gen-1 AND a Gen-2 ROM independently,
 * gb_art_source.c's own s_reg_have array), so a Gen-1 (Red) dex visit with a
 * Gen-2 ROM ALSO registered would have `have(PDNA_GEN2)` answer true even though
 * gbdex_cell_art() refuses every cell (its own gate also checks `s->gen ==
 * GB_GEN2`) -- computing the page-level skip from have(GEN2) alone silently
 * defeated the icon-store plan AND stopped the bob animation for a session that
 * would never once call the GB path. The installer already knows both halves of
 * its own gate (session gen + have(GEN2)) at install time, so it passes the
 * ANSWER instead of the codebase re-deriving (and risking re-diverging) it in two
 * more places. Ignored/false when `fn` is NULL. */
typedef bool (*PdnaDexCellArtFn)(uint16_t dex, int x, int y, int w, int h, void* ctx);
void pdna_dex_set_cell_art(PdnaDexCellArtFn fn, void* ctx, bool serves_page);

/* BACKLOG #208: an optional hook fired once per FULL repaint of the grid, BEFORE the
 * `vis` dex_cell_grid() calls that follow it -- a cold entry, a view/filter change,
 * or a one-row scroll step, exactly the same events dex_declare_page() itself reacts
 * to (see that function's own comment), never the bob-animation tick (a page the
 * cell-art override serves never bobs at all -- pdna_dex_screen's own per-cell
 * dex_cell_art_page_served() guard). Same installer contract as
 * pdna_dex_set_cell_art() above (installed/cleared around the SAME call, NULL is the
 * default, never a pdna_dex_screen() parameter): gives the GB cell-art override's
 * owner (pdna_gbdex.c) a page boundary to declare its own per-page art cache against
 * and to roll up a fetch/hit tally per page, without pdna_pick.c (the Gen-3 dex's
 * own shared screen) knowing anything about a cache that exists only on the GB side.
 * Called only when the page being declared is the GRID view (list view draws no art
 * at all -- dex_declare_page()'s own rule).
 *
 * PDNA_DELTA-only, entirely (declaration AND the call site in pdna_pick.c) -- this
 * hook exists ONLY to drive BACKLOG #208's measurement (a fetch/hit tally per page,
 * printed through log_line), which is itself PDNA_DELTA-only per that item's own
 * design. Gating the whole mechanism out of the artless/normal/sd variants (not just
 * the counters it would otherwise drive) means those variants compile no indirect
 * call through a pointer nothing in them ever assigns -- tools/stack_budget.py's
 * whole-graph sweep would otherwise have to be told, in EVERY variant, that a
 * pointer only PDNA_DELTA ever writes is unreachable in the others; leaving the
 * whole feature out of their translation units is the simpler, harder-to-drift
 * way to say the same thing. The GB art CACHE itself (pdna_gbdex.c's
 * gbdex_cell_art_gen1/gen2) is unaffected -- it never calls through this hook. */
#ifdef PDNA_DELTA
typedef void (*PdnaDexPageFn)(void);
void pdna_dex_set_page_begin(PdnaDexPageFn fn);
#endif

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

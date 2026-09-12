#ifndef DEX_CELL_ART_RULE_H
#define DEX_CELL_ART_RULE_H
#include <stdbool.h>

/*
 * dex_cell_art_rule -- BACKLOG #124's selection rule for one Gen-1/2 Pokedex grid
 * cell: given what this visit knows, which art source should paint it?
 *
 * Pure C (no tonc/FatFs/GBA headers), no globals, no I/O -- tests/host_dexcellart_
 * test.c compiles and runs this exact file on the PC and exercises the whole 2x2x2
 * truth table. source/pdna_gbdex.c's gbdex_cell_art() (pdna_pick.h's PdnaDexCellArtFn
 * callback) is the one caller: it calls this FIRST to decide whether to even attempt
 * the GB ROM fetch, so the test covers the real rule the cartridge runs, not a
 * paraphrase of it.
 */

typedef enum {
  DEX_CELL_ART_GB    = 0,  /* draw the GB ROM's own icon (pdna_origin_art_icon())    */
  DEX_CELL_ART_STORE = 1,  /* fall back to the icon store / mon_icon_for ladder       */
  DEX_CELL_ART_NONE  = 2   /* neither source can serve this cell right now            */
} DexCellArtSource;

/*
 * gb_session -- this dex visit is a Gen-2 GB session (never Gen 1: Gen 1 has no
 *   per-species menu icons at all, gb_art_source.c's own rule; never Gen 3: the
 *   ordinary Gen-3 dex screen never installs the override that feeds this at all,
 *   so gb_session is always false for it).
 * gb_have -- a Gen-2 ROM is actually registered and servable right now
 *   (pdna_origin_art_have(PDNA_GEN2) -- cheap, no I/O, no SD read).
 * store_ok -- the icon-store / mon_icon_for fallback ladder can serve this species.
 *   Always true from the real caller today (dex_cell_grid()'s own unchanged path is
 *   always attempted when this rule does not pick DEX_CELL_ART_GB) -- carried as an
 *   explicit input, not hardcoded, so the rule states the whole contract and a future
 *   caller with a genuinely unservable store (e.g. an out-of-range species) gets the
 *   honest DEX_CELL_ART_NONE answer instead of a silent GB refusal with no fallback.
 *
 * Pure function: same inputs always give the same answer, no side effects, safe to
 * call every frame.
 */
DexCellArtSource dex_cell_art_source(bool gb_session, bool gb_have, bool store_ok);

/*
 * Review A5 cross-review finding: the PAGE-level question dex_declare_page()'s
 * icon-store-plan skip and the bob-animation loop both need ("will the override
 * serve ANY cell on this visit") collapses to exactly the same two inputs
 * gbdex_cell_art()'s own per-cell gate uses (species is per-cell, not per-page, so
 * it drops out) -- `gb_session && gb_have`. Pinned here, next to dex_cell_art_source(),
 * so the per-cell and per-page rules are provably consistent (tests/host_dexcellart_
 * test.c exercises both against the same table) instead of two hand-written copies
 * of "gen == GB_GEN2 && have(GEN2)" that could quietly re-diverge. pdna_gbdex.c's
 * gbdex_serves_dex() calls this directly; pdna_pick.h's pdna_dex_set_cell_art()
 * stores the ANSWER (computed once, at install time), never this function itself --
 * pdna_pick.c must not re-derive it from a live pdna_origin_art_have() read (that
 * global is boot-sticky and independent per era, which is the whole bug this rule
 * exists to prevent a repeat of).
 */
bool dex_cell_art_serves_page(bool gb_session, bool gb_have);

#endif /* DEX_CELL_ART_RULE_H */

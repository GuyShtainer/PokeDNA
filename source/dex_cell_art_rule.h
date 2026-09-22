#ifndef DEX_CELL_ART_RULE_H
#define DEX_CELL_ART_RULE_H
#include <stdbool.h>

/*
 * dex_cell_art_rule -- BACKLOG #124's selection rule for one Gen-1/2 Pokedex grid
 * cell: given what this visit knows, which art source should paint it?
 *
 * Pure C (no tonc/FatFs/GBA headers), no globals, no I/O -- tests/host_dexcellart_
 * test.c compiles and runs this exact file on the PC and exercises the whole truth
 * table. source/pdna_gbdex.c's gbdex_cell_art() (pdna_pick.h's PdnaDexCellArtFn
 * callback) is the one caller: it calls this FIRST to decide whether to even attempt
 * the GB ROM fetch, so the test covers the real rule the cartridge runs, not a
 * paraphrase of it.
 *
 * BACKLOG #196: extended from BACKLOG #124's Gen-2-only bool `gb_session` to a
 * three-way `session_gen` (0 = no GB session / Gen 3, GB_GEN1 = 1, GB_GEN2 = 2,
 * gb_edit.h's own numbering, deliberately reused rather than re-declared here so
 * the two files can never silently disagree on what "1" and "2" mean) -- Gen 1 now
 * has its own GB rung too (pdna_origin_art_portrait_by_dex(), the front-pic twin of
 * Gen 2's pdna_origin_art_icon(), see pdna_origin_art.h's own comment for why that
 * needed a NEW entry point rather than reusing pdna_origin_art_portrait()). The
 * safety property BACKLOG #124 built this rule for is unchanged and still the whole
 * point: `gb_have` must be the caller's answer for *this session's own generation*
 * (pdna_origin_art_have(PDNA_GEN1) for a GB_GEN1 session, PDNA_GEN2 for a GB_GEN2
 * one) -- never "have(GEN1) || have(GEN2)" -- so a Gen-1 session can never borrow a
 * separately-registered Gen-2 ROM's art (or vice versa) for a same-numbered Kanto
 * species just because SOME GB ROM happens to be registered.
 */

typedef enum {
  DEX_CELL_ART_GB    = 0,  /* draw the GB ROM's own picture (icon for Gen 2, front
                            * sprite for Gen 1 -- pdna_origin_art_icon() /
                            * pdna_origin_art_portrait_by_dex())                    */
  DEX_CELL_ART_STORE = 1,  /* fall back to the icon store / mon_icon_for ladder       */
  DEX_CELL_ART_NONE  = 2   /* neither source can serve this cell right now            */
} DexCellArtSource;

/*
 * session_gen -- this dex visit's own generation: 0 for "no GB session" (a native
 *   Gen-3 dex, or ctx == NULL), 1 for GB_GEN1, 2 for GB_GEN2 (gb_edit.h's numbering).
 *   Any other value is treated as "no GB session" (defensive -- the real caller only
 *   ever passes 0, 1 or 2).
 * gb_have -- a ROM for THIS SESSION'S OWN generation is registered and servable
 *   right now (pdna_origin_art_have(PDNA_GEN1) or PDNA_GEN2 as `session_gen` says --
 *   cheap, no I/O, no SD read). The caller computes this per-generation answer; this
 *   rule never asks "which generation" on its own, on purpose (see the file header).
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
DexCellArtSource dex_cell_art_source(int session_gen, bool gb_have, bool store_ok);

/*
 * Review A5 cross-review finding: the PAGE-level question dex_declare_page()'s
 * icon-store-plan skip and the bob-animation loop both need ("will the override
 * serve ANY cell on this visit") collapses to exactly the same two inputs
 * gbdex_cell_art()'s own per-cell gate uses (species is per-cell, not per-page, so
 * it drops out) -- `(session_gen == 1 || session_gen == 2) && gb_have`. Pinned here,
 * next to dex_cell_art_source(), so the per-cell and per-page rules are provably
 * consistent (tests/host_dexcellart_test.c exercises both against the same table)
 * instead of two hand-written copies of "gen == GB_GEN1/GB_GEN2 && have(that gen)"
 * that could quietly re-diverge. pdna_gbdex.c's gbdex_serves_dex() calls this
 * directly; pdna_pick.h's pdna_dex_set_cell_art() stores the ANSWER (computed once,
 * at install time), never this function itself -- pdna_pick.c must not re-derive it
 * from a live pdna_origin_art_have() read (that global is boot-sticky and
 * independent per era, which is the whole bug this rule exists to prevent a repeat
 * of).
 */
bool dex_cell_art_serves_page(int session_gen, bool gb_have);

#endif /* DEX_CELL_ART_RULE_H */

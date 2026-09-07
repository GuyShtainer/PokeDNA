#ifndef NAV_AVAIL_H
#define NAV_AVAIL_H

/* nav_avail -- BACKLOG #58 (Guy, 2026-09-07): "Please use the same start button menu
 * in all games, even when a feature is not built yet or when it's not supported (like
 * Pokeblocks), but when the user tries to press it, say 'coming soon' or 'not
 * supported in Gen 2 games'."
 *
 * Before this, a Game Boy (Gen 1/2) session's START menu (pdna_gen12.c's
 * gb_nav_from_start) showed a DIFFERENT, shrunken menu -- most rows dimmed by a
 * bitmask (NAV_ALL_AVAILABLE / app_nav_menu's avail_mask, pdna_app.h), one generic
 * "GEN 3 ONLY / Gen 3 saves only, for now." message for all of them. Guy wants the
 * SAME menu, every row lit exactly like a Gen-3 save's, and pressing an unavailable
 * row to say WHY: not built yet (COMING SOON) vs. does not exist in this game/gen
 * (NOT IN GAME) -- two different facts that deserve two different messages.
 *
 * This module is the pure-C RULE TABLE behind that: for one nav row (an NV_* id from
 * pdna_layout.h's PDNA_NAV_ITEMS) and one save kind (an SE_KIND_* id from
 * sprite_era.h), what should pressing A do? No tonc, no FatFs, no GBA headers -- only
 * <stdint.h>/<string.h> would be needed and this file needs neither -- so
 * tests/host_nav_avail_test.c compiles and runs it on the PC, following this
 * codebase's pure-C-core convention (sprite_era.c, gen3_frontier.c, pdna_places.c).
 *
 * ---- WHY EVERY GEN-3 SAVE (RS/EM/FRLG) ANSWERS NAV_OK FOR EVERY ROW -----------------
 *
 * The brief for this item assumed several Gen-3 rows would need gating (Pokeblocks/
 * Secret Bases/Frontier/Mirage/Clock fix/Battle Records, each absent from at least one
 * of RS/Emerald/FireRed-LeafGreen) -- but checking what each of those six screens
 * ALREADY does per game (source/nav_avail.c's own header comment cites the exact
 * lines) found every one of them already honest: either it shows real, working,
 * per-game content (pdna_frontier.c redirects Ruby/Sapphire to the real Battle Tower
 * screen instead of refusing anything), or it already refuses with its own clear
 * message on exactly the game(s) that lack the feature (pdna_pokeblock/pdna_secretbase/
 * pdna_mirage/pdna_clock/pdna_battle_record in pdna_main.c). Gating any of those a
 * SECOND time here would either duplicate a message that already exists or, for
 * Frontier on Ruby/Sapphire, silently take away a screen that works. So nav_avail()
 * defers to the existing screens for every Gen-3 kind: this table only has an opinion
 * about a raw Game Boy save (GEN1/GEN2), where gb_nav_from_start (pdna_gen12.c) does
 * NOT have a per-game-checked screen behind most rows at all -- BACKLOG #49/#52 track
 * building Party/Bank/Daycare/Pokedex/Bag/Flags/Fly/Map/GB-import for a raw Game Boy
 * save; until one ships, its row belongs here, in exactly one place, saying so. */

typedef enum {
  NAV_OK = 0,        /* dispatch normally -- the row's real screen (or, for a Gen-3
                       * kind, the row's EXISTING per-game-correct behaviour) runs   */
  NAV_COMING_SOON,    /* the feature is real (works for some other save) but this
                       * session's code path does not wire it up yet                */
  NAV_NOT_IN_GAME     /* this game/generation never had the feature at all          */
} NavAvail;

/* `nv_item` is an NV_* id from pdna_layout.h's PDNA_NAV_ITEMS X-macro (0..NV_COUNT-1).
 * An out-of-range value answers NAV_OK -- the safest default: this table only ever
 * SUBTRACTS availability from a row it explicitly recognises, never invents a refusal
 * for one it does not.
 *
 * `save_kind` is an SE_KIND_* value from sprite_era.h (SE_KIND_RS=0, SE_KIND_EM=1,
 * SE_KIND_FRLG=2, SE_KIND_GEN1=3, SE_KIND_GEN2=4) -- a plain `int` here, not
 * `SeSaveKind`, so THIS header stays decoupled from sprite_era.h, the same split
 * sprite_era.h/.c itself keeps from gen3_trainer.h's PkGame (its own file header
 * explains why: pure-C cores including only what their PUBLIC signature needs).
 * nav_avail.c privately includes sprite_era.h for the matching SE_KIND_* case labels
 * and a _Static_assert on their numbering, exactly as sprite_era.c does for PkGame.
 * A save_kind that is not SE_KIND_GEN1/SE_KIND_GEN2 (a Gen-3 kind, or any
 * out-of-range value) answers NAV_OK for every row -- see the file header above. */
NavAvail nav_avail(int nv_item, int save_kind);

/* A short, honest, ALWAYS non-empty reason, <= 30 characters (msg_wait's 184px
 * proportional clamp at a 30-char cap; tests/host_nav_avail_test.c asserts the
 * length on every pair this function can be asked about). Defined for EVERY
 * (nv_item, save_kind) pair, including every NAV_OK one -- app_nav_refuse
 * (pdna_main.c) only ever calls this for the two refusal states, so an OK pair's
 * reason is never shown to a player, but the contract stays total so nothing that
 * calls this can read back a NULL or empty string. */
const char* nav_avail_why(int nv_item, int save_kind);

#endif /* NAV_AVAIL_H */

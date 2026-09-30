#include "nav_avail.h"

#include "pdna_layout.h"    /* NV_* row ids + NV_COUNT -- pure C, no .c file to link  */
#include "sprite_era.h"     /* SE_KIND_* -- ONLY for the case labels + the assert below;
                              * nav_avail.h itself stays decoupled (plain ints), the
                              * same split sprite_era.h/.c keeps from gen3_trainer.h. */
#include "xfer_gate.h"      /* BACKLOG #239: xfer_direct_allowed() -- the one flag that
                              * makes NV_GB say COMING_SOON on a Gen-3 kind too, see
                              * nav_avail()'s own comment below. */

/* If sprite_era.h's SeSaveKind numbering ever changes, GB_TABLE's two columns below
 * silently answer for the wrong kind -- catch that at compile time instead, the same
 * pattern sprite_era.c uses for PkGame. */
_Static_assert(SE_KIND_GEN1 == 3 && SE_KIND_GEN2 == 4,
               "nav_avail's GB_TABLE column order assumes this exact SeSaveKind numbering");

/* ---- the Game Boy (Gen 1 / Gen 2) rule table ----------------------------------------
 *
 * nav_avail() answers NAV_OK immediately for every Gen-3 kind (RS/EM/FRLG) without
 * consulting this table -- VERIFIED against what each candidate screen already does,
 * not assumed (BACKLOG #58 delivery notes, 2026-09-07):
 *
 *   pdna_pokeblock()      pdna_main.c ~5326  pk_pokeblock_offset(g_game)==0 (FRLG only)
 *                          -> "NO POKEBLOCKS" / "This game lacks contests."
 *   pdna_secretbase()     pdna_main.c ~6543  gen3_secret_base_offset()==0 (FRLG only)
 *                          -> "SECRET BASES" / "FireRed/LeafGreen has no Secret Bases."
 *   pdna_mirage()         pdna_main.c ~6745  `if (g_game == PK_FRLG)` ->
 *                          "FireRed / LeafGreen have no Mirage Island."
 *   pdna_clock()          pdna_main.c ~6858  `if (g_game == PK_FRLG)` ->
 *                          "FireRed / LeafGreen have no real-time clock..."
 *   pdna_battle_record()  pdna_main.c ~8004  `if (g_game != PK_EMERALD)` ->
 *                          "Only Emerald stores a Battle Record."
 *   pdna_frontier()       pdna_frontier.c:364-369  `!g3f_supported(game)` refuses FRLG
 *                          on its own; `game == PK_RS` REDIRECTS to rs_tower() -- a
 *                          real, working screen, not a refusal
 *   event_tickets()       pdna_main.c ~5469  three per-game tables (Emerald 4, FRLG 2,
 *                          RS 1) -- every game has at least one, nothing to gate
 *
 * Gating any of those a second time in pdna_main.c's nav switch would either duplicate
 * an already-honest message (five of the six) or silently remove a working screen
 * (Frontier on RS). So none of them are gated -- the table below exists only for a raw
 * Game Boy save, whose gb_nav_from_start (pdna_gen12.c) has no per-game-checked screen
 * behind most rows at all. */

typedef struct { NavAvail state; const char* why; } NavCell;

/* Column 0 = SE_KIND_GEN1, column 1 = SE_KIND_GEN2. Every NV_* row gets an explicit
 * entry -- unlike the Gen-3 short-circuit above, "missing from this table" would be a
 * bug here, not a safe default: every row of a Game Boy session's menu must say
 * something honest when it cannot do the real thing. */
static const NavCell GB_TABLE[NV_COUNT][2] = {
  /* Review D1: party VIEW/EDIT already works in a Game Boy session -- the party is the
   * grid's last box (pdna_gen12.c last_box_is_party) -- so this row must not claim the
   * capability is missing; it points at where it lives instead. */
  [NV_PARTY]     = { { NAV_COMING_SOON, "The party is the last box." },
                     { NAV_COMING_SOON, "The party is the last box." } },
  [NV_BANK]      = { { NAV_OK, "OK" },
                     { NAV_OK, "OK" } },
  /* BACKLOG #85: gb_daycare.c (the pure core) is merged on main; this UI slice wires
   * it up on BOTH kinds -- Gen 1 gets the one-slot, level-up-only version (no
   * breeding, no compatibility line -- the format's own limit, not a missing
   * feature), Gen 2 gets both slots + compatibility + the egg. */
  [NV_DAYCARE]   = { { NAV_OK, "OK" }, { NAV_OK, "OK" } },
  [NV_TRAINER]   = { { NAV_OK, "OK" }, { NAV_OK, "OK" } },
  /* BACKLOG #86/#108: Gen 2's own Clock fix screen (pdna_gbclock.c) is wired --
   * gb_nav_from_start's own NV_CLOCK/SE_KIND_GEN2 branch. Gen 1 has no clock at all
   * (gb_clock.h: gbc_read() reports present=false for every Gen-1 save), so its own
   * cell stays NAV_NOT_IN_GAME. */
  [NV_CLOCK]     = { { NAV_NOT_IN_GAME, "Gen 1 games have no clock." },
                     { NAV_OK, "OK" } },
  [NV_MIRAGE]    = { { NAV_NOT_IN_GAME, "Gen 1 games have no Mirage." },
                     { NAV_NOT_IN_GAME, "Gen 2 games have no Mirage." } },
  /* BACKLOG #87: gb_dex.c (the pure core) + pdna_gbdex.c (the screen) are wired on
   * gb_nav_from_start's own NV_DEX branch -- both generations get the shared
   * pdna_dex_screen() UNCHANGED under a species cap (151 Gen 1, 251 Gen 2), Gen 2
   * additionally offers the Unown-forms toggle list from its own entry chooser. */
  [NV_DEX]       = { { NAV_OK, "OK" }, { NAV_OK, "OK" } },
  /* U4 (BACKLOG #67): Red/Yellow's own Item bag + PC store are wired
   * (pdna_gbbag.c) -- Gen 1. U5: Gold/Silver/Crystal's own Pack + PC store
   * are wired too (pdna_gbpack.c, design sec 1.4) -- Gen 2. */
  [NV_BAG]       = { { NAV_OK, "OK" },
                     { NAV_OK, "OK" } },
  /* BACKLOG #88: the Flags & counters screen (pdna_gbflags.c) is wired on BOTH
   * kinds -- gb_nav_from_start's own NV_DATA branch, same "needs a live GbSession
   * to write through" gate every other real screen on this menu uses. */
  [NV_DATA]      = { { NAV_OK, "OK" },
                     { NAV_OK, "OK" } },
  [NV_SECRET]    = { { NAV_NOT_IN_GAME, "Gen 1 games have no Bases." },
                     { NAV_NOT_IN_GAME, "Gen 2 games have no Bases." } },
  [NV_POKEBLOCK] = { { NAV_NOT_IN_GAME, "Gen 1 games have no Blocks." },
                     { NAV_NOT_IN_GAME, "Gen 2 games have no Blocks." } },
  [NV_EVENTS]    = { { NAV_NOT_IN_GAME, "Gen 1 games have no tickets." },
                     { NAV_NOT_IN_GAME, "Gen 2 games have no tickets." } },
  /* BACKLOG #89: the "Records" row hosts a NEW screen on a Game Boy save -- the Hall
   * of Fame (source/pdna_gbhof.c, source/gb_hof.h) -- not the Gen-3 Frontier battle
   * record this row opens on RS/EM/FRLG (pdna_battle_record(), which never runs on a
   * Game Boy save at all: gb_nav_from_start dispatches its OWN NV_BATTLEREC branch).
   * Both generations record a Hall of Fame, so this is NAV_OK on both kinds now,
   * unlike every other Hoenn/Frontier-shaped row in this table. */
  [NV_BATTLEREC] = { { NAV_OK, "OK" }, { NAV_OK, "OK" } },
  [NV_FRONTIER]  = { { NAV_NOT_IN_GAME, "Gen 1 games have no Frontier." },
                     { NAV_NOT_IN_GAME, "Gen 2 games have no Frontier." } },
  /* BACKLOG #90: gb_fly.h's bitfield core + pdna_gbfly.c's screen are wired for
   * both generations now. */
  [NV_FLY]       = { { NAV_OK, "OK" }, { NAV_OK, "OK" } },
  [NV_CONTEST]   = { { NAV_NOT_IN_GAME, "Gen 1 games have no Contests." },
                     { NAV_NOT_IN_GAME, "Gen 2 games have no Contests." } },
  /* BACKLOG #91 M1/M1-G2: both generations' read-only current-map views are
   * wired now (pdna_gbmap_gen1()/pdna_gbmap_gen2(), gb_nav_from_start's
   * NV_MAP branches). */
  [NV_MAP]       = { { NAV_OK, "OK" },
                     { NAV_OK, "OK" } },
  /* BACKLOG #239 review: this row was a concurrent second-save mount (removed --
   * see nv_gb_blocked() below) that let a COPY in the Game Boy save leak straight
   * into a live Gen-3 save with no Bank in the middle. "Open the Bank instead." lied
   * about an empty Bank (nothing was ever deposited, since the GB save was never
   * opened); the honest procedure is deposit-first. */
  [NV_GB]        = { { NAV_BANK_ONLY, "Deposit it to the Bank first." },
                     { NAV_BANK_ONLY, "Deposit it to the Bank first." } },
  /* BACKLOG #150 S150-11 decision 13: the TRANSFERS screen only reads a Gen-3 PC
   * (app_gen3_pc_live()) -- a raw Game Boy session has none, so it's the same
   * "open it from the other side" honesty as NV_GB above, not NAV_OK. */
  [NV_XFER]      = { { NAV_COMING_SOON, "Open it from a Gen-3 save." },
                     { NAV_COMING_SOON, "Open it from a Gen-3 save." } },
  /* #234 slice 3 is Gen-3 only (design D8); slice 4 brings the same core to a Game Boy save. */
  [NV_HISTORY]   = { { NAV_COMING_SOON, "Game Boy history comes later." },
                     { NAV_COMING_SOON, "Game Boy history comes later." } },
  [NV_SETTINGS]  = { { NAV_OK, "OK" }, { NAV_OK, "OK" } },
  [NV_BACK]      = { { NAV_OK, "OK" }, { NAV_OK, "OK" } },
};

/* Shared by nav_avail/nav_avail_why: -1 means "not a Game Boy kind, or nv_item is out
 * of range" -- both callers fall back to their own safe default in that case. */
static int gb_col(int nv_item, int save_kind) {
  if ((unsigned)nv_item >= (unsigned)NV_COUNT) return -1;
  if (save_kind == SE_KIND_GEN1) return 0;
  if (save_kind == SE_KIND_GEN2) return 1;
  return -1;
}

/* BACKLOG #239: the ONE exception to "every Gen-3 kind answers NAV_OK for every row"
 * (this file's own header comment above). NV_GB is how a Gen-3 save's own nav menu
 * mounts a SECOND (Game Boy) save as a read-only box source while the Gen-3 save stays
 * resident -- exactly the "direct save-to-save / cart-to-save" concurrency BACKLOG #239
 * closes; both of pdna_main.c's NV_GB call sites (pdna_gen12_show / the PDNA_DELTA
 * pdna_gen12_show_fused arm) sit behind this SAME nav_avail() check, so gating it here
 * is the one place that makes both unreachable. xfer_direct_allowed() is a plain
 * function (xfer_gate.h), not a table row, because a Gen-3 kind's own GB_TABLE column
 * does not exist (gb_col() returns -1 for RS/EM/FRLG) -- adding a real column would
 * mean re-deriving col for a kind this table was never meant to index. Checked BEFORE
 * gb_col(): a GEN1/GEN2 save_kind already gets NAV_COMING_SOON off GB_TABLE regardless
 * (unaffected either way), so this early return only ever changes the Gen-3 answer. */
static bool nv_gb_blocked(int nv_item) {
  return nv_item == (int)NV_GB && !xfer_direct_allowed();
}

NavAvail nav_avail(int nv_item, int save_kind) {
  if (nv_gb_blocked(nv_item)) return NAV_BANK_ONLY;
  int col = gb_col(nv_item, save_kind);
  return (col < 0) ? NAV_OK : GB_TABLE[nv_item][col].state;
}

const char* nav_avail_why(int nv_item, int save_kind) {
  /* Same wording the Game Boy table already uses for this exact row (GB_TABLE's own
   * [NV_GB] entries above) -- one string, not a second copy that could drift. */
  if (nv_gb_blocked(nv_item)) return "Deposit it to the Bank first.";
  int col = gb_col(nv_item, save_kind);
  return (col < 0) ? "OK" : GB_TABLE[nv_item][col].why;
}

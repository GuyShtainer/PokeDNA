#include "nav_avail.h"

#include "pdna_layout.h"    /* NV_* row ids + NV_COUNT -- pure C, no .c file to link  */
#include "sprite_era.h"     /* SE_KIND_* -- ONLY for the case labels + the assert below;
                              * nav_avail.h itself stays decoupled (plain ints), the
                              * same split sprite_era.h/.c keeps from gen3_trainer.h. */

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
  [NV_PARTY]     = { { NAV_COMING_SOON, "Party editing is coming soon." },
                     { NAV_COMING_SOON, "Party editing is coming soon." } },
  [NV_BANK]      = { { NAV_COMING_SOON, "The Bank is coming soon." },
                     { NAV_COMING_SOON, "The Bank is coming soon." } },
  [NV_DAYCARE]   = { { NAV_COMING_SOON, "The Daycare is coming soon." },
                     { NAV_COMING_SOON, "The Daycare is coming soon." } },
  [NV_TRAINER]   = { { NAV_OK, "OK" }, { NAV_OK, "OK" } },
  [NV_CLOCK]     = { { NAV_NOT_IN_GAME, "Gen 1 games have no clock." },
                     { NAV_COMING_SOON, "Gen 2 games have a clock too." } },
  [NV_MIRAGE]    = { { NAV_NOT_IN_GAME, "Gen 1 games have no Mirage." },
                     { NAV_NOT_IN_GAME, "Gen 2 games have no Mirage." } },
  [NV_DEX]       = { { NAV_COMING_SOON, "The Pokedex is coming soon." },
                     { NAV_COMING_SOON, "The Pokedex is coming soon." } },
  [NV_BAG]       = { { NAV_COMING_SOON, "The Bag is coming soon." },
                     { NAV_COMING_SOON, "The Bag is coming soon." } },
  [NV_DATA]      = { { NAV_COMING_SOON, "Flags/counters coming soon." },
                     { NAV_COMING_SOON, "Flags/counters coming soon." } },
  [NV_SECRET]    = { { NAV_NOT_IN_GAME, "Gen 1 games have no Bases." },
                     { NAV_NOT_IN_GAME, "Gen 2 games have no Bases." } },
  [NV_POKEBLOCK] = { { NAV_NOT_IN_GAME, "Gen 1 games have no Blocks." },
                     { NAV_NOT_IN_GAME, "Gen 2 games have no Blocks." } },
  [NV_EVENTS]    = { { NAV_NOT_IN_GAME, "Gen 1 games have no tickets." },
                     { NAV_NOT_IN_GAME, "Gen 2 games have no tickets." } },
  [NV_BATTLEREC] = { { NAV_NOT_IN_GAME, "Gen 1 games have no Records." },
                     { NAV_NOT_IN_GAME, "Gen 2 games have no Records." } },
  [NV_FRONTIER]  = { { NAV_NOT_IN_GAME, "Gen 1 games have no Frontier." },
                     { NAV_NOT_IN_GAME, "Gen 2 games have no Frontier." } },
  [NV_FLY]       = { { NAV_COMING_SOON, "Fly flags are coming soon." },
                     { NAV_COMING_SOON, "Fly flags are coming soon." } },
  [NV_MAP]       = { { NAV_COMING_SOON, "The map view is coming soon." },
                     { NAV_COMING_SOON, "The map view is coming soon." } },
  [NV_GB]        = { { NAV_COMING_SOON, "Comes with the Bank feature." },
                     { NAV_COMING_SOON, "Comes with the Bank feature." } },
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

NavAvail nav_avail(int nv_item, int save_kind) {
  int col = gb_col(nv_item, save_kind);
  return (col < 0) ? NAV_OK : GB_TABLE[nv_item][col].state;
}

const char* nav_avail_why(int nv_item, int save_kind) {
  int col = gb_col(nv_item, save_kind);
  return (col < 0) ? "OK" : GB_TABLE[nv_item][col].why;
}

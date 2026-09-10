/* Host test for source/nav_avail.{c,h} -- BACKLOG #58 (Guy, 2026-09-07): one identical
 * START menu in every game, honest per-row messages when a row is not built yet
 * (COMING SOON) or does not exist in this game/generation (NOT IN GAME).
 *
 *   cc -std=c11 -O2 -Wall -Wextra -I source tests/host_nav_avail_test.c \
 *      source/nav_avail.c -o /tmp/hnat && /tmp/hnat
 *
 * Covers: every (nv_item, save_kind) pair over the full NV_COUNT x SE_KIND_N grid
 * returns a defined NavAvail state and a non-empty, <=30-char reason (msg_wait's
 * 184px proportional clamp at a 30-char cap); every Gen-3 kind (RS/EM/FRLG) answers
 * NAV_OK for every row -- the VERIFIED result of checking each candidate screen's
 * existing per-game behaviour, spelled out in source/nav_avail.c's own header, not an
 * unchecked default; Settings/Back answer NAV_OK for every kind; the Game Boy table's
 * named cases from the brief (Clock fix's Gen1-vs-Gen2 SPLIT, a COMING_SOON row, a
 * NOT_IN_GAME row); and out-of-range nv_item/save_kind degrade to NAV_OK with a
 * non-empty reason rather than reading off the end of the table. */
#include <stdio.h>
#include <string.h>

#include "nav_avail.h"
#include "pdna_layout.h"
#include "sprite_era.h"

static int checks = 0, fails = 0;
#define CHECK(c, msg) do { checks++; if (!(c)) { printf("  !! FAIL: %s\n", msg); fails++; } } while (0)

/* Every kind this table is ever asked about, GB kinds last (mirrors sprite_era.h's own
 * SE_KIND_N ordering: RS, EM, FRLG, GEN1, GEN2). */
static const int ALL_KINDS[] = { SE_KIND_RS, SE_KIND_EM, SE_KIND_FRLG, SE_KIND_GEN1, SE_KIND_GEN2 };
#define N_KINDS (int)(sizeof ALL_KINDS / sizeof ALL_KINDS[0])

/* ---- (A) every (item, kind) pair: a defined state + a non-empty, <=30-char reason -- */
static void test_every_pair_defined(void) {
  for (int item = 0; item < NV_COUNT; item++) {
    for (int k = 0; k < N_KINDS; k++) {
      NavAvail a = nav_avail(item, ALL_KINDS[k]);
      CHECK(a == NAV_OK || a == NAV_COMING_SOON || a == NAV_NOT_IN_GAME,
            "nav_avail: state is one of the three defined values");
      const char* why = nav_avail_why(item, ALL_KINDS[k]);
      CHECK(why != 0, "nav_avail_why: never NULL");
      if (why) {
        CHECK(why[0] != '\0', "nav_avail_why: never empty");
        CHECK(strlen(why) <= 30, "nav_avail_why: <= 30 chars (msg_wait's proportional clamp)");
      }
    }
  }
  printf("(A) every (item, kind) pair defined, %d items x %d kinds\n", NV_COUNT, N_KINDS);
}

/* ---- (B) every Gen-3 kind answers NAV_OK for every row ------------------------------
 *
 * VERIFIED, not assumed: source/nav_avail.c's file header cites the exact per-game
 * check inside pdna_pokeblock/pdna_secretbase/pdna_mirage/pdna_clock/
 * pdna_battle_record/pdna_frontier/event_tickets that already makes every one of
 * those honest on its own -- so the "explicit gate list" for a Gen-3 kind is empty,
 * and this loop has nothing to except out. A future row that DOES need gating for a
 * Gen-3 kind changes this test right here, on purpose -- not silently. */
static void test_gen3_kinds_all_ok(void) {
  const int g3[] = { SE_KIND_RS, SE_KIND_EM, SE_KIND_FRLG };
  for (int k = 0; k < 3; k++)
    for (int item = 0; item < NV_COUNT; item++)
      CHECK(nav_avail(item, g3[k]) == NAV_OK,
            "Gen-3 kind (RS/EM/FRLG): every row is NAV_OK (see nav_avail.c's header)");
  printf("(B) RS/EM/FRLG: every one of %d rows is NAV_OK\n", NV_COUNT);
}

/* ---- (C) Settings / Back: NAV_OK everywhere, every kind ------------------------------ */
static void test_settings_back_ok_everywhere(void) {
  for (int k = 0; k < N_KINDS; k++) {
    CHECK(nav_avail(NV_SETTINGS, ALL_KINDS[k]) == NAV_OK, "Settings: NAV_OK for every kind");
    CHECK(nav_avail(NV_BACK, ALL_KINDS[k]) == NAV_OK, "Back: NAV_OK for every kind");
  }
  printf("(C) Settings/Back: NAV_OK across all %d kinds\n", N_KINDS);
}

/* ---- (D) Trainer: OK on a Game Boy save too (gb_info_page already serves it) -------- */
static void test_trainer_ok_on_gb(void) {
  CHECK(nav_avail(NV_TRAINER, SE_KIND_GEN1) == NAV_OK, "Trainer: NAV_OK on a Gen 1 save");
  CHECK(nav_avail(NV_TRAINER, SE_KIND_GEN2) == NAV_OK, "Trainer: NAV_OK on a Gen 2 save");
  printf("(D) Trainer: NAV_OK on both Game Boy kinds\n");
}

/* ---- (E) Clock fix: the one row whose STATE (not just its wording) differs between
 * Gen 1 (no RTC ever existed) and Gen 2 (Gold/Silver/Crystal have one; PokeDNA just
 * has not wired the fix up for a raw Game Boy save yet). ---------------------------- */
static void test_clock_fix_splits_gen1_gen2(void) {
  CHECK(nav_avail(NV_CLOCK, SE_KIND_GEN1) == NAV_NOT_IN_GAME, "Clock fix: Gen 1 has no RTC at all");
  CHECK(nav_avail(NV_CLOCK, SE_KIND_GEN2) == NAV_COMING_SOON, "Clock fix: Gen 2 has an RTC, just not wired up yet");
  const char* w1 = nav_avail_why(NV_CLOCK, SE_KIND_GEN1);
  const char* w2 = nav_avail_why(NV_CLOCK, SE_KIND_GEN2);
  CHECK(strcmp(w1, w2) != 0, "Clock fix: Gen 1 and Gen 2 reasons are worded differently");
  printf("(E) Clock fix: NOT_IN_GAME on Gen 1, COMING_SOON on Gen 2\n");
}

/* ---- (F) a representative COMING_SOON row and a representative NOT_IN_GAME row,
 * both Game Boy kinds, matching the brief's own worked examples verbatim. ------------ */
static void test_representative_rows(void) {
  /* U4 (BACKLOG #67): Red/Yellow's own Item bag is wired -- Gen 1 is NAV_OK.
   * U5: Gold/Silver/Crystal's own Pack (design sec 1.4) is wired too -- Gen 2
   * is NAV_OK now as well. */
  CHECK(nav_avail(NV_BAG, SE_KIND_GEN1) == NAV_OK, "Bag: wired up on Gen 1 (U4, BACKLOG #67)");
  CHECK(nav_avail(NV_BAG, SE_KIND_GEN2) == NAV_OK, "Pack: wired up on Gen 2 too (U5, BACKLOG #67)");
  CHECK(nav_avail(NV_POKEBLOCK, SE_KIND_GEN1) == NAV_NOT_IN_GAME, "Blocks: Gen 1 never had Pokeblocks");
  CHECK(nav_avail(NV_POKEBLOCK, SE_KIND_GEN2) == NAV_NOT_IN_GAME, "Blocks: Gen 2 never had Pokeblocks");
  /* Per-gen wording, not one shared string copy-pasted across both kinds. */
  CHECK(strcmp(nav_avail_why(NV_POKEBLOCK, SE_KIND_GEN1), nav_avail_why(NV_POKEBLOCK, SE_KIND_GEN2)) != 0,
        "Blocks: Gen 1 and Gen 2 reasons name their own generation");
  CHECK(nav_avail(NV_GB, SE_KIND_GEN1) == NAV_COMING_SOON, "GB import row on a Gen 1 save: coming with the Bank");
  /* BACKLOG #91 M1: Map splits like Bag once did -- Gen 1's read-only current-map
   * view is wired (NAV_OK); Gen 2's own map is a later slice (COMING_SOON). */
  CHECK(nav_avail(NV_MAP, SE_KIND_GEN1) == NAV_OK, "Map: wired up on Gen 1 (M1, BACKLOG #91)");
  CHECK(nav_avail(NV_MAP, SE_KIND_GEN2) == NAV_COMING_SOON, "Map: Gen 2's own map is a later slice");
  printf("(F) representative COMING_SOON / NOT_IN_GAME rows match the brief\n");
}

/* ---- (G) every one of the 19 PDNA_NAV_ITEMS rows is covered by the GB table -------- */
static void test_every_row_covered(void) {
  /* A row this table forgot would fall back to the -1/"not found" path inside
   * nav_avail.c's gb_col() and answer NAV_OK -- indistinguishable from a genuinely
   * built row by return value alone, so cross-check against the brief's own 19-item
   * classification instead: every row is EITHER coming-soon-or-ok (never refused as
   * NOT_IN_GAME) except the six Hoenn/Frontier-shaped features and half of Clock fix. */
  static const int not_in_game_gen1[] = {
    NV_CLOCK, NV_MIRAGE, NV_SECRET, NV_POKEBLOCK, NV_EVENTS, NV_BATTLEREC, NV_FRONTIER,
    NV_CONTEST   /* BACKLOG #60: RSE-only, same shape as the other 7 */
  };
  for (int i = 0; i < (int)(sizeof not_in_game_gen1 / sizeof not_in_game_gen1[0]); i++)
    CHECK(nav_avail(not_in_game_gen1[i], SE_KIND_GEN1) == NAV_NOT_IN_GAME,
          "Gen 1: every Hoenn/Frontier-shaped row (+Clock/Contest) is NOT_IN_GAME");

  /* U5: NV_BAG is now NAV_OK on BOTH kinds (Gen 1's own Item bag, U4; Gen 2's
   * own Pack, U5) -- checked separately below, alongside the OK-on-both rows.
   * BACKLOG #91 M1: NV_MAP moved OUT of this list -- it now splits per-gen
   * like NV_CLOCK does (checked in its own assertion below), not COMING_SOON
   * on both any more. */
  static const int coming_soon_both[] = {
    NV_PARTY, NV_BANK, NV_DAYCARE, NV_DEX, NV_DATA, NV_FLY, NV_GB
  };
  for (int i = 0; i < (int)(sizeof coming_soon_both / sizeof coming_soon_both[0]); i++) {
    CHECK(nav_avail(coming_soon_both[i], SE_KIND_GEN1) == NAV_COMING_SOON,
          "Gen 1: every not-yet-wired row is COMING_SOON");
    CHECK(nav_avail(coming_soon_both[i], SE_KIND_GEN2) == NAV_COMING_SOON,
          "Gen 2: every not-yet-wired row is COMING_SOON");
  }

  static const int ok_both[] = { NV_TRAINER, NV_SETTINGS, NV_BACK, NV_BAG };
  for (int i = 0; i < (int)(sizeof ok_both / sizeof ok_both[0]); i++) {
    CHECK(nav_avail(ok_both[i], SE_KIND_GEN1) == NAV_OK, "Gen 1: Trainer/Settings/Back/Bag are NAV_OK");
    CHECK(nav_avail(ok_both[i], SE_KIND_GEN2) == NAV_OK, "Gen 2: Trainer/Settings/Back/Pack are NAV_OK");
  }

  /* NV_MAP: OK on Gen 1, COMING_SOON on Gen 2 -- same per-gen-split shape as
   * NV_CLOCK (checked in test_clock_fix_splits_gen1_gen2 / test_representative_rows
   * above), counted here as its own single row rather than folded into either
   * all-COMING_SOON or all-OK list. */
  CHECK(nav_avail(NV_MAP, SE_KIND_GEN1) == NAV_OK, "Gen 1: Map is NAV_OK (M1, BACKLOG #91)");
  CHECK(nav_avail(NV_MAP, SE_KIND_GEN2) == NAV_COMING_SOON, "Gen 2: Map is still COMING_SOON");

  /* 8 NOT_IN_GAME (gen1-list, Clock counted once, + BACKLOG #60's NV_CONTEST) + 7
   * COMING_SOON-both + 4 OK-both (Trainer/Settings/Back/Bag -- U5 made the Bag OK on
   * Gen 2 too) + Clock's own Gen-2 COMING_SOON (already counted above via a separate
   * assertion) + Map's own per-gen split (this assertion) == 20 rows -- the
   * classification is EXHAUSTIVE, not a sample, so a row silently added to
   * PDNA_NAV_ITEMS without a matching GB_TABLE entry cannot hide behind rows this
   * test never asked about. */
  CHECK(8 + 7 + 4 + 1 == NV_COUNT, "row classification accounts for all 20 PDNA_NAV_ITEMS");
  printf("(G) every PDNA_NAV_ITEMS row is classified (8 NOT_IN_GAME + 7 COMING_SOON-both + "
        "4 OK-both + 1 Map-split == %d)\n", NV_COUNT);
}

/* ---- (H) defensive: out-of-range nv_item / save_kind never misbehaves -------------- */
static void test_out_of_range(void) {
  CHECK(nav_avail(-1, SE_KIND_GEN1) == NAV_OK, "negative nv_item: degrades to NAV_OK");
  CHECK(nav_avail(NV_COUNT, SE_KIND_GEN1) == NAV_OK, "nv_item == NV_COUNT: degrades to NAV_OK");
  CHECK(nav_avail(NV_COUNT + 99, SE_KIND_GEN2) == NAV_OK, "wildly out-of-range nv_item: NAV_OK");
  CHECK(nav_avail(NV_PARTY, 99) == NAV_OK, "out-of-range save_kind: treated like a Gen-3 kind (NAV_OK)");
  CHECK(nav_avail(NV_PARTY, -1) == NAV_OK, "negative save_kind: treated like a Gen-3 kind (NAV_OK)");
  const char* w = nav_avail_why(-1, -1);
  CHECK(w != 0 && w[0] != '\0', "out-of-range pair: nav_avail_why still non-empty");
  printf("(H) out-of-range nv_item/save_kind: NAV_OK, non-empty reason, no crash\n");
}

int main(void) {
  test_every_pair_defined();
  test_gen3_kinds_all_ok();
  test_settings_back_ok_everywhere();
  test_trainer_ok_on_gb();
  test_clock_fix_splits_gen1_gen2();
  test_representative_rows();
  test_every_row_covered();
  test_out_of_range();

  printf("\n%d checks, %d FAILED\n", checks, fails);
  return fails ? 1 : 0;
}

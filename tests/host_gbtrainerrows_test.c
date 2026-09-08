/* Host test for source/gbtr_rows.c -- the Gen-1/2 trainer card's pure row-
 * visibility model (BACKLOG #49 P1b/P1c), same shape as host_flagsfold_test.c.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gbtrainerrows_test.c \
 *      source/gbtr_rows.c -o /tmp/hgbtr && /tmp/hgbtr
 *
 * pdna_gbtrainer.c's front/back cursor loops call these exact functions
 * (gbtr_build_front_rows / gbtr_build_back_rows) -- there is no second copy of
 * "which rows exist for this game, on which face" to drift out of sync with
 * what ships; only the cursor/edit state and the card-art draw stay in
 * pdna_gbtrainer.c, which is GBA UI and not host-compilable. gbtr_rows.h only
 * needs GbTrainer's shape (gb_trainer.h, a plain struct with no function
 * bodies), so this test links against neither gb_trainer.c nor any GB save
 * parser. */
#include <stdio.h>
#include <string.h>

#include "gbtr_rows.h"

static int fails = 0, checks = 0;

static void expect_int(const char* what, int got, int want) {
  checks++;
  if (got != want) { fails++; printf("FAIL %s: got %d want %d\n", what, got, want); }
}

static bool contains(const int* rows, int n, int kind) {
  for (int i = 0; i < n; i++) if (rows[i] == kind) return true;
  return false;
}

static void expect_absent(const char* what, const int* rows, int n, int kind) {
  checks++;
  if (contains(rows, n, kind)) { fails++; printf("FAIL %s: row unexpectedly present\n", what); }
}
static void expect_present(const char* what, const int* rows, int n, int kind) {
  checks++;
  if (!contains(rows, n, kind)) { fails++; printf("FAIL %s: row unexpectedly absent\n", what); }
}

int main(void) {
  int rows[GBTR_ROW_MAX];

  /* NULL guards: touches nothing, returns 0. */
  expect_int("front null t", gbtr_build_front_rows(NULL, rows), 0);
  expect_int("back null t",  gbtr_build_back_rows(NULL, rows), 0);
  GbTrainer any; memset(&any, 0, sizeof any);
  expect_int("front null rows", gbtr_build_front_rows(&any, NULL), 0);
  expect_int("back null rows",  gbtr_build_back_rows(&any, NULL), 0);

  /* FRONT: constant across every game -- NAME, ID, MONEY, TIME, BADGES, in that
   * CARDF_* cursor order, whether or not any Gen-2 flag is set. */
  int nf = gbtr_build_front_rows(&any, rows);
  expect_int("front count", nf, 5);
  expect_int("front[0]=NAME",   rows[0], GBTR_NAME);
  expect_int("front[1]=ID",     rows[1], GBTR_ID);
  expect_int("front[2]=MONEY",  rows[2], GBTR_MONEY);
  expect_int("front[3]=TIME",   rows[3], GBTR_TIME);
  expect_int("front[4]=BADGES", rows[4], GBTR_BADGES);

  /* BACK, Gen 1 (Red/Blue/Yellow): no mom, no gender, no mother -- just
   * COINS + RIVAL, well inside the 6-row back-page budget. */
  GbTrainer g1; memset(&g1, 0, sizeof g1);
  g1.has_mom = false; g1.has_gender = false; g1.has_mother = false;
  int n1 = gbtr_build_back_rows(&g1, rows);
  expect_int("g1 back count", n1, 2);
  expect_int("g1 back[0]=COINS", rows[0], GBTR_COINS);
  expect_int("g1 back[1]=RIVAL", rows[1], GBTR_RIVAL);
  expect_absent("g1: no mom/mother/gender/Kanto rows", rows, n1, GBTR_MOMMONEY);
  expect_absent("g1: no Kanto badges", rows, n1, GBTR_KANTOBADGES);
  expect_absent("g1: no gender", rows, n1, GBTR_GENDER);

  /* BACK, Gold/Silver: mom + mother, no gender -- COINS, MOM$, MOM SAVE, KANTO
   * BADGES, RIVAL, MOTHER: exactly 6, the full back-page budget, Kanto badges
   * present (has_gender is false so nothing displaces them). */
  GbTrainer gs; memset(&gs, 0, sizeof gs);
  gs.has_mom = true; gs.has_gender = false; gs.has_mother = true;
  int ngs = gbtr_build_back_rows(&gs, rows);
  expect_int("gs back count", ngs, 6);
  expect_present("gs: mom money", rows, ngs, GBTR_MOMMONEY);
  expect_present("gs: mom save",  rows, ngs, GBTR_MOMSAVE);
  expect_present("gs: Kanto badges", rows, ngs, GBTR_KANTOBADGES);
  expect_present("gs: rival", rows, ngs, GBTR_RIVAL);
  expect_present("gs: mother", rows, ngs, GBTR_MOTHER);
  expect_absent("gs: no gender row", rows, ngs, GBTR_GENDER);

  /* BACK, Crystal: mom + mother + gender -- GENDER wins the one row Emerald's
   * 6-row back page cannot also give to Kanto badges (gbtr_build_back_rows'
   * own comment explains the tradeoff); still exactly 6 rows, no overflow. */
  GbTrainer cr; memset(&cr, 0, sizeof cr);
  cr.has_mom = true; cr.has_gender = true; cr.has_mother = true;
  int ncr = gbtr_build_back_rows(&cr, rows);
  expect_int("crystal back count", ncr, 6);
  expect_present("crystal: gender", rows, ncr, GBTR_GENDER);
  expect_absent("crystal: Kanto badges displaced by gender", rows, ncr, GBTR_KANTOBADGES);
  expect_present("crystal: mom money", rows, ncr, GBTR_MOMMONEY);
  expect_present("crystal: mother", rows, ncr, GBTR_MOTHER);
  expect_int("crystal back[0]=COINS", rows[0], GBTR_COINS);

  printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "OK", checks, fails);
  return fails ? 1 : 0;
}

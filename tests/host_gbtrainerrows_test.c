/* Host test for source/gbtr_rows.c -- the Gen-1/2 trainer card's pure row-
 * visibility model (BACKLOG #49 P1b), same shape as host_flagsfold_test.c.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gbtrainerrows_test.c \
 *      source/gbtr_rows.c -o /tmp/hgbtr && /tmp/hgbtr
 *
 * pdna_gbtrainer.c's row loop calls this exact function (gbtr_build_rows) --
 * there is no second copy of "which rows exist for this game" to drift out of
 * sync with what ships; only the cursor/edit state stays in pdna_gbtrainer.c,
 * which is GBA UI and not host-compilable. gbtr_rows.h only needs GbTrainer's
 * shape (gb_trainer.h, a plain struct with no function bodies), so this test
 * links against neither gb_trainer.c nor any GB save parser. */
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

int main(void) {
  int rows[GBTR_ROW_MAX];

  /* NULL guards: touches nothing, returns 0. */
  expect_int("null t", gbtr_build_rows(NULL, rows), 0);
  GbTrainer any; memset(&any, 0, sizeof any);
  expect_int("null rows", gbtr_build_rows(&any, NULL), 0);

  /* Gen 1 (Red/Blue/Yellow): no mom, no gender, no mother. */
  GbTrainer g1; memset(&g1, 0, sizeof g1);
  g1.has_mom = false; g1.has_gender = false; g1.has_mother = false;
  int n1 = gbtr_build_rows(&g1, rows);
  expect_int("g1 count", n1, 8);
  expect_int("g1[0]=NAME",  rows[0], GBTR_NAME);
  expect_int("g1[1]=ID",    rows[1], GBTR_ID);
  expect_int("g1[2]=MONEY", rows[2], GBTR_MONEY);
  expect_int("g1[3]=COINS", rows[3], GBTR_COINS);
  expect_int("g1[4]=BADGES", rows[4], GBTR_BADGES);
  expect_int("g1[5]=TIME",   rows[5], GBTR_TIME);
  expect_int("g1[6]=DEX",    rows[6], GBTR_DEX);
  expect_int("g1[7]=RIVAL",  rows[7], GBTR_RIVAL);
  checks++; if (contains(rows, n1, GBTR_MOMMONEY) || contains(rows, n1, GBTR_MOMSAVE) ||
                contains(rows, n1, GBTR_GENDER)   || contains(rows, n1, GBTR_MOTHER)) {
    fails++; printf("FAIL g1: a Gen-2-only row leaked into Gen 1's list\n");
  }

  /* Gold/Silver: mom + no gender, no mother-view-gate difference (has_mother still
   * gates independently -- Gold/Silver DO have a mother's-name field per the design
   * doc, so has_mother is true here to prove the row appears when the flag says so). */
  GbTrainer gs; memset(&gs, 0, sizeof gs);
  gs.has_mom = true; gs.has_gender = false; gs.has_mother = true;
  int ngs = gbtr_build_rows(&gs, rows);
  expect_int("gs count", ngs, 11);
  checks++; if (!contains(rows, ngs, GBTR_MOMMONEY) || !contains(rows, ngs, GBTR_MOMSAVE)) {
    fails++; printf("FAIL gs: mom rows missing when has_mom is true\n");
  }
  checks++; if (contains(rows, ngs, GBTR_GENDER)) {
    fails++; printf("FAIL gs: GENDER row present when has_gender is false\n");
  }
  checks++; if (!contains(rows, ngs, GBTR_MOTHER)) {
    fails++; printf("FAIL gs: MOTHER row missing when has_mother is true\n");
  }

  /* Crystal: mom + gender + mother, every optional row present, and NAME/ID/MONEY/
   * COINS still come first in the Gen-3 order. */
  GbTrainer cr; memset(&cr, 0, sizeof cr);
  cr.has_mom = true; cr.has_gender = true; cr.has_mother = true;
  int ncr = gbtr_build_rows(&cr, rows);
  expect_int("crystal count", ncr, 12);
  expect_int("crystal[0]=NAME",  rows[0], GBTR_NAME);
  expect_int("crystal[1]=ID",    rows[1], GBTR_ID);
  expect_int("crystal[2]=MONEY", rows[2], GBTR_MONEY);
  expect_int("crystal[3]=COINS", rows[3], GBTR_COINS);
  checks++; if (!contains(rows, ncr, GBTR_GENDER)) {
    fails++; printf("FAIL crystal: GENDER row missing when has_gender is true\n");
  }

  printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "OK", checks, fails);
  return fails ? 1 : 0;
}

#ifndef GBTR_ROWS_H
#define GBTR_ROWS_H

#include "gb_trainer.h"   /* GbTrainer */

/* Pure-C row-visibility model for the Gen-1/2 trainer card (BACKLOG #49 P1b),
 * host-testable the same way flags_fold.h is: pdna_gbtrainer.c (GBA UI, tonc-
 * dependent) calls this exact function, so there is no second copy of "which rows
 * exist for this game" to drift out of sync with what ships.
 *
 * Row order (docs/GEN12-PARITY-DESIGN.md 4.1 P1; P1b review D9 -- this is NOT the
 * Gen-3 card's own order, which is NAME, SEX, ID, SID, MONEY, TIME, BADGE, STARS
 * and has no COINS/MOM/DEX/RIVAL/MOTHER at all): editable fields first, then
 * view-only ones -- NAME, ID, MONEY, COINS, [MOM'S MONEY, MOM SAVE] (Gen 2 only),
 * BADGES, TIME, [GENDER] (Crystal only, view-only), DEX, RIVAL, [MOTHER]
 * (Gen 2 only). */
enum {
  GBTR_NAME, GBTR_ID, GBTR_MONEY, GBTR_COINS, GBTR_MOMMONEY, GBTR_MOMSAVE,
  GBTR_BADGES, GBTR_TIME, GBTR_GENDER, GBTR_DEX, GBTR_RIVAL, GBTR_MOTHER,
  GBTR_ROW_MAX
};

/* Fills `rows` (must hold >= GBTR_ROW_MAX ints) with the row-kind list for this
 * trainer's game, in on-screen order, and returns the count. NULL `t`/`rows`
 * returns 0 and touches nothing. */
int gbtr_build_rows(const GbTrainer* t, int rows[GBTR_ROW_MAX]);

#endif /* GBTR_ROWS_H */

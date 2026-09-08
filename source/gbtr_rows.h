#ifndef GBTR_ROWS_H
#define GBTR_ROWS_H

#include "gb_trainer.h"   /* GbTrainer */

/* Pure-C row-visibility model for the Gen-1/2 trainer card (BACKLOG #49 P1b/P1c),
 * host-testable the same way flags_fold.h is: pdna_gbtrainer.c (GBA UI, tonc-
 * dependent) calls this exact function, so there is no second copy of "which rows
 * exist for this game" to drift out of sync with what ships.
 *
 * P1c (UX-parity: the card now LOOKS like the Gen-3 card, source/pdna_trainer.c,
 * front + back over CARD_LAYOUTS[PK_EMERALD]/CARD_BACK_LAYOUTS[PK_EMERALD]) splits
 * the row list in two:
 *
 *   FRONT (on the card art, CARDF_* cursor order): NAME, ID No., MONEY, PLAY TIME,
 *   BADGES. Constant across every GB game -- unlike P1b's flat list, none of these
 *   five are ever omitted (money_ok/coins_ok false just shows "?", same as before;
 *   it does not remove the row). gbtr_build_front_rows() still takes `t` and
 *   returns a count for call-site symmetry with the back list below, and so a
 *   future front field that DOES vary by game has one obvious place to add the
 *   gate.
 *
 *   BACK (CARD_BACK_LAYOUTS[PK_EMERALD] has exactly 6 row slots -- nback == 6):
 *   COINS always; MOM'S MONEY + MOM SAVE MODE if has_mom (Gen 2); RIVAL always;
 *   MOTHER if has_mother (Gen 2); and EITHER the Kanto badge count (Gen 2,
 *   Gold/Silver) OR GENDER (Gen 2, Crystal only) -- never both, see
 *   gbtr_build_back_rows()'s own comment for why that is the one row this list
 *   drops rather than overflowing Emerald's 6-row back page. Gen 1 (no mom/mother/
 *   gender/Kanto badges) is just COINS + RIVAL, 2 of the 6 slots. */
enum {
  GBTR_NAME, GBTR_ID, GBTR_MONEY, GBTR_COINS, GBTR_MOMMONEY, GBTR_MOMSAVE,
  GBTR_BADGES, GBTR_KANTOBADGES, GBTR_TIME, GBTR_GENDER, GBTR_DEX, GBTR_RIVAL,
  GBTR_MOTHER,
  GBTR_ROW_MAX
};

/* Front row list (CARDF_* cursor order): always {NAME, ID, MONEY, TIME, BADGES},
 * 5 rows, for every GB game. `rows` must hold >= GBTR_ROW_MAX ints. NULL `t`/`rows`
 * returns 0 and touches nothing. */
int gbtr_build_front_rows(const GbTrainer* t, int rows[GBTR_ROW_MAX]);

/* Back row list (card_back_row_paint order, top to bottom): see the file comment
 * above for the exact per-game membership and the Kanto-badges-vs-GENDER tradeoff.
 * Never exceeds 6 (CARD_BACK_LAYOUTS[PK_EMERALD].nrows). `rows` must hold >=
 * GBTR_ROW_MAX ints. NULL `t`/`rows` returns 0 and touches nothing. */
int gbtr_build_back_rows(const GbTrainer* t, int rows[GBTR_ROW_MAX]);

/* P1b's original FLAT list (NAME, ID, MONEY, COINS, [MOM$, MOM SAVE], BADGES,
 * TIME, [GENDER], DEX, RIVAL, [MOTHER]), unchanged -- this is now ONLY the
 * artless-build fallback page (pdna_gbtrainer.c falls back to it exactly the
 * way pdna_trainer()'s own tcard_render plain page is Gen-3's fallback, when
 * card_bg(PK_EMERALD, ...).blob == 0: no ROM open that can serve Emerald's
 * card chrome). Never includes GBTR_KANTOBADGES (the flat BADGES row already
 * shows the combined Johto+Kanto count, same as P1b always did). */
int gbtr_build_rows(const GbTrainer* t, int rows[GBTR_ROW_MAX]);

#endif /* GBTR_ROWS_H */

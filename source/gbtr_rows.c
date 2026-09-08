#include "gbtr_rows.h"

int gbtr_build_front_rows(const GbTrainer* t, int rows[GBTR_ROW_MAX]) {
  if (!t || !rows) return 0;
  int n = 0;
  rows[n++] = GBTR_NAME;
  rows[n++] = GBTR_ID;
  rows[n++] = GBTR_MONEY;
  rows[n++] = GBTR_TIME;
  rows[n++] = GBTR_BADGES;
  return n;
}

int gbtr_build_back_rows(const GbTrainer* t, int rows[GBTR_ROW_MAX]) {
  if (!t || !rows) return 0;
  int n = 0;
  rows[n++] = GBTR_COINS;
  if (t->has_mom) { rows[n++] = GBTR_MOMMONEY; rows[n++] = GBTR_MOMSAVE; }
  /* Kanto badges (Gen 2, all of Gold/Silver/Crystal have them) vs. GENDER
   * (Crystal only): together with COINS/MOM$/MOMSAVE/RIVAL/MOTHER that would be
   * 7 rows on Crystal, one past Emerald's 6-row back page (card_bg.h
   * CARD_BACK_LAYOUTS[PK_EMERALD].nrows == 6) -- something has to give. GENDER
   * wins on Crystal specifically because it has NO other display location on
   * this screen (unlike Kanto badges, which stay fully visible AND editable via
   * the same 8/16-row sub-editor the front BADGES field already opens -- see
   * pdna_gbtrainer.c's gbtr_badges_editor, unchanged from P1b) and it is the one
   * fact P1c's brief calls out as Crystal-exclusive. Gold/Silver (has_gender ==
   * false) keep the Kanto row instead since they have the row budget to spare it. */
  if (t->has_mom && !t->has_gender) rows[n++] = GBTR_KANTOBADGES;
  rows[n++] = GBTR_RIVAL;
  if (t->has_mother) rows[n++] = GBTR_MOTHER;
  if (t->has_gender) rows[n++] = GBTR_GENDER;
  return n;
}

int gbtr_build_rows(const GbTrainer* t, int rows[GBTR_ROW_MAX]) {
  if (!t || !rows) return 0;
  int n = 0;
  rows[n++] = GBTR_NAME;
  rows[n++] = GBTR_ID;
  rows[n++] = GBTR_MONEY;
  rows[n++] = GBTR_COINS;
  if (t->has_mom)    { rows[n++] = GBTR_MOMMONEY; rows[n++] = GBTR_MOMSAVE; }
  rows[n++] = GBTR_BADGES;
  rows[n++] = GBTR_TIME;
  if (t->has_gender) rows[n++] = GBTR_GENDER;
  rows[n++] = GBTR_DEX;
  rows[n++] = GBTR_RIVAL;
  if (t->has_mother) rows[n++] = GBTR_MOTHER;
  return n;
}

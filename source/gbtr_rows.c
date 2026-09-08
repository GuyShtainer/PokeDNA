#include "gbtr_rows.h"

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

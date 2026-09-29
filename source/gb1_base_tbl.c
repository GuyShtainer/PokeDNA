/* gb1_base_tbl.c -- see gb1_base_tbl.h. Pure C, no I/O. */
#include "gb1_base_tbl.h"

bool gb1_base_table_present(void) {
  uint8_t row[GB1_BASE_ROW_LEN];
  return gb1_base_gen_row(1, row);
}

bool gb1_base_table_fill(uint16_t dex, GbNewMonSrc* src) {
  uint8_t row[GB1_BASE_ROW_LEN];
  if (!src || !gb1_base_gen_row(dex, row)) return false;
  for (int i = 0; i < GB_NSTATS; i++) src->base[i] = row[i];
  src->type1 = row[5];
  src->type2 = row[6];
  src->growth = row[7];
  return true;
}

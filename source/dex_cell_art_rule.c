/*
 * dex_cell_art_rule.c -- see the header for the full contract. Deliberately tiny and
 * table-shaped: this is the whole rule, not a summary of a bigger one hiding in the
 * caller.
 */
#include "dex_cell_art_rule.h"

DexCellArtSource dex_cell_art_source(bool gb_session, bool gb_have, bool store_ok) {
  if (gb_session && gb_have) return DEX_CELL_ART_GB;
  if (store_ok) return DEX_CELL_ART_STORE;
  return DEX_CELL_ART_NONE;
}

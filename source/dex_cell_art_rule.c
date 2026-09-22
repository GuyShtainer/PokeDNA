/*
 * dex_cell_art_rule.c -- see the header for the full contract. Deliberately tiny and
 * table-shaped: this is the whole rule, not a summary of a bigger one hiding in the
 * caller.
 */
#include "dex_cell_art_rule.h"

static bool is_gb_gen(int session_gen) { return session_gen == 1 || session_gen == 2; }

DexCellArtSource dex_cell_art_source(int session_gen, bool gb_have, bool store_ok) {
  if (is_gb_gen(session_gen) && gb_have) return DEX_CELL_ART_GB;
  if (store_ok) return DEX_CELL_ART_STORE;
  return DEX_CELL_ART_NONE;
}

bool dex_cell_art_serves_page(int session_gen, bool gb_have) {
  return is_gb_gen(session_gen) && gb_have;
}

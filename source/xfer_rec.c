#include "xfer_rec.h"
#include "gb_edit.h"   /* gb_max_species/gb_max_move, GB_GEN1 -- already pure C */

/* Same constants gb_sidecar.c's gbsc_key() uses (source/gb_sidecar.h:68-70). */
uint64_t xr_key_g3(const uint8_t rec80[80]) {
  uint64_t h = 14695981039346656037ULL;   /* FNV-1a-64 offset basis */
  for (int i = 0; i < 8; i++) {
    h ^= (uint64_t)rec80[i];
    h *= 1099511628211ULL;                /* FNV-1a-64 prime */
  }
  return h;
}

/* BACKLOG #150 S150-8 decision 5. met_game: 1 Sapphire, 2 Ruby, 3 Emerald, 4 FireRed,
 * 5 LeafGreen (pdna_main.c's own app_met_game() spelling). */
uint8_t xr_game_item_mask(uint8_t met_game) {
  switch (met_game) {
    case 1: case 2: return 0x01u;   /* RS   */
    case 3:         return 0x02u;   /* Emerald */
    case 4: case 5: return 0x04u;   /* FRLG */
    default:        return 0u;
  }
}

/* BACKLOG #150 S150-8 decision 14/D-Q7. */
int xr_time_capsule_block(uint8_t src_gen, uint8_t dst_gen, uint16_t species_dex,
                          const uint16_t moves[4], uint16_t* bad) {
  if (dst_gen != GB_GEN1) return 0;    /* Gen 1 -> Gen 2 (or anything else): always allowed */
  (void)src_gen;                       /* only the DESTINATION's floor matters here */
  if (species_dex == 0 || species_dex > gb_max_species(GB_GEN1)) {
    if (bad) *bad = species_dex;
    return 1;
  }
  if (moves) {
    for (int i = 0; i < 4; i++) {
      if (moves[i] != 0 && moves[i] > gb_max_move(GB_GEN1)) {
        if (bad) *bad = moves[i];
        return 2;
      }
    }
  }
  return 0;
}

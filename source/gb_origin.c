/* BACKLOG #266 -- see gb_origin.h. Pure C, const data only. */
#include "gb_origin.h"

typedef struct { uint16_t dex; uint8_t level; } GbOriginRow;

/* Gen 1 (national dex numbers). Levels from the games' own map object data. */
static const GbOriginRow k_gen1[] = {
  { 144, 50 },   /* Articuno  -- Seafoam Islands B4F */
  { 145, 50 },   /* Zapdos    -- Power Plant         */
  { 146, 50 },   /* Moltres   -- Victory Road 2F     */
  { 150, 70 },   /* Mewtwo    -- Cerulean Cave B1F   */
};
#define K_GEN1_N ((int)(sizeof k_gen1 / sizeof k_gen1[0]))

uint8_t gb_origin_static_level(uint8_t gen, uint16_t dex) {
  if (gen != 1u || dex == 0u) return 0;
  for (int i = 0; i < K_GEN1_N; i++)
    if (k_gen1[i].dex == dex) return k_gen1[i].level;
  return 0;
}

uint8_t gb_origin_level_floor(uint8_t gen, uint16_t dex, uint8_t floor) {
  uint8_t st = gb_origin_static_level(gen, dex);
  return (st > floor) ? st : floor;
}

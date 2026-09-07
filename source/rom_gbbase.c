/* Gen-1 base stats out of the user's own Game Boy cartridge -- see rom_gbbase.h
 * for the design, the decomp citations and why Gen 2 is deliberately absent.
 * Pure C: no tonc, no FatFs, no GBA headers, no statics. */
#include "rom_gbbase.h"

/* pokered BASE_DATA_SIZE (constants/pokemon_data_constants.asm) -- the same
 * 28-byte stride rom_gbsprite.c's g1_row() reads. */
#define G1B_ROW      28u
#define G1B_SPECIES 151u   /* highest Gen-1 national dex number == Mew */

/* Field offsets inside one 28-byte row. See rom_gbbase.h's ROW LAYOUT comment
 * for the citations; only the fields this module serves are named here. */
#define G1B_OFF_DEX     0u
#define G1B_OFF_STATS   1u   /* HP, Atk, Def, Spd, Spc -- 5 bytes             */
#define G1B_OFF_TYPE1   6u
#define G1B_OFF_TYPE2   7u
#define G1B_OFF_CATCH   8u
#define G1B_OFF_START  15u   /* four level-1-learnset move bytes             */
#define G1B_OFF_GROWTH 19u

bool rom_gbbase_gen1(const RomGbSprite* gs, GbReadFn read, void* ctx, uint16_t dex,
                     RomGb1Species* out) {
  if (!gs || !read || !out) return false;
  if (!gs->ok || gs->gen != GB_ROM_GEN1) return false;
  if (dex < 1 || dex > G1B_SPECIES) return false;

  uint32_t off = (dex == G1B_SPECIES && gs->mew_stats != 0u)
                   ? gs->mew_stats
                   : gs->base_stats + (uint32_t)(dex - 1u) * G1B_ROW;

  uint8_t row[G1B_ROW];
  if (!read(ctx, off, row, sizeof row)) return false;
  if (row[G1B_OFF_DEX] != (uint8_t)dex) return false;   /* self-check */

  out->base.base[GB_HP]  = row[G1B_OFF_STATS + 0u];
  out->base.base[GB_ATK] = row[G1B_OFF_STATS + 1u];
  out->base.base[GB_DEF] = row[G1B_OFF_STATS + 2u];
  out->base.base[GB_SPE] = row[G1B_OFF_STATS + 3u];
  out->base.base[GB_SPC] = row[G1B_OFF_STATS + 4u];
  out->base.type1 = row[G1B_OFF_TYPE1];
  out->base.type2 = row[G1B_OFF_TYPE2];
  out->catch_rate = row[G1B_OFF_CATCH];
  out->growth     = row[G1B_OFF_GROWTH];
  out->start[0] = row[G1B_OFF_START + 0u];
  out->start[1] = row[G1B_OFF_START + 1u];
  out->start[2] = row[G1B_OFF_START + 2u];
  out->start[3] = row[G1B_OFF_START + 3u];
  return true;
}

/* Field offsets inside one 32-byte Gen-2 BaseData row. See rom_gbbase.h's ROW
 * LAYOUT comment: PICSIZE/BETA are the two fields rom_gbsprite.c's own
 * g2_bd_verify already demands a shape for on every ROM it opens; the rest
 * (dex, stats, growth) are new to this module. */
#define G2B_ROW         32u
#define G2B_SPECIES    251u
#define G2B_OFF_DEX      0u
#define G2B_OFF_STATS    1u   /* HP, Atk, Def, Spd, SpA, SpD -- 6 bytes       */
#define G2B_OFF_PICSIZE 17u
#define G2B_OFF_BETA    18u
#define G2B_OFF_GROWTH  22u

bool rom_gbbase_gen2(const RomGbSprite* gs, GbReadFn read, void* ctx, uint16_t dex,
                     RomGb2Species* out) {
  if (!gs || !read || !out) return false;
  if (!gs->ok || gs->gen != GB_ROM_GEN2) return false;
  if (dex < 1 || dex > G2B_SPECIES) return false;

  uint32_t off = gs->base_data + (uint32_t)(dex - 1u) * G2B_ROW;
  uint8_t row[G2B_ROW];
  if (!read(ctx, off, row, sizeof row)) return false;
  if (row[G2B_OFF_DEX] != (uint8_t)dex) return false;                /* self-check */
  uint8_t sz = (uint8_t)(row[G2B_OFF_PICSIZE] & 0x0Fu);
  if (sz < 4u || sz > 7u) return false;                               /* == g2_bd_verify */
  if (row[G2B_OFF_BETA] || row[G2B_OFF_BETA + 1u]
   || row[G2B_OFF_BETA + 2u] || row[G2B_OFF_BETA + 3u]) return false; /* == g2_bd_verify */

  for (int i = 0; i < 6; i++) out->base[i] = row[G2B_OFF_STATS + (uint32_t)i];
  out->growth = row[G2B_OFF_GROWTH];
  return true;
}

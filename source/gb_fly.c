#include "gb_fly.h"

GbGame gbfy_game(const GbSession* s) {
  if (!s) return GBF_G_RED;
  if (s->gen == GB_GEN1) return GBF_G_RED;   /* Yellow == Red/Blue layout */
  return (s->g2w.sv.version == G2_VER_CRYSTAL) ? GBF_G_CRYSTAL : GBF_G_GS;
}

static GbField fly_field(GbGame g) {
  return gbf_off(g, GBF_FLY_FLAGS) ? GBF_FLY_FLAGS : GBF_FLY_FLAGS_G2;
}

int gbfy_count(GbGame game) {
  GbField f = fly_field(game);
  if (!gbf_off(game, f)) return 0;
  /* P1a review D7: this used to return gbf_len()*8 -- the field's own BIT WIDTH (16 for
   * Gen 1, 32 for Gen 2), not how many of those bits are real destinations. The real
   * counts, per each game's own decomp constant:
   *   Gen 1 (Red/Blue/Yellow) -- pokered constants/map_constants.asm: NUM_CITY_MAPS =
   *     11 (PALLET_TOWN..SAFFRON_CITY, the Town Map's own town list); wTownVisitedFlag
   *     is 2 B / 16 bits wide, but only the first 11 are real (bits 11-15 are unused).
   *   Gen 2 (GS/Crystal) -- pokecrystal constants/map_data_constants.asm: NUM_SPAWNS =
   *     28 (SPAWN_HOME..SPAWN_FAST_SHIP); wVisitedSpawns is 4 B / 32 bits wide, but only
   *     the first 28 are real. */
  return (game == GBF_G_GS || game == GBF_G_CRYSTAL) ? 28 : 11;
}

bool gbfy_read(const GbSession* s, uint8_t* bits, int cap) {
  if (!s || !bits || cap < 0 || !s->open) return false;
  GbGame g = gbfy_game(s);
  GbField f = fly_field(g);
  uint32_t off = gbf_off(g, f);
  uint16_t len = gbf_len(g, f);
  if (!off || !len || (uint16_t)cap < len) return false;
  GbSession* ncs = (GbSession*)(const void*)s;
  return gbs_read_field(ncs, off, bits, len) == GBS_OK;
}

bool gbfy_get(const GbSession* s, int index) {
  if (!s || !s->open || index < 0) return false;
  GbGame g = gbfy_game(s);
  if (index >= gbfy_count(g)) return false;
  uint8_t bits[4];   /* widest fly field this table has (Gen 2, 4 B) */
  if (!gbfy_read(s, bits, (int)sizeof bits)) return false;
  int byte = index / 8, bit = index % 8;
  return (bits[byte] & (1u << bit)) != 0;
}

GbsStatus gbfy_set(GbSession* s, int index, bool visited) {
  if (!s || !s->open || index < 0) return GBS_ERR_ARG;
  GbGame g = gbfy_game(s);
  if (index >= gbfy_count(g)) return GBS_ERR_ARG;
  GbField f = fly_field(g);
  uint32_t off = gbf_off(g, f);
  if (!off) return GBS_ERR_ARG;

  int byte = index / 8, bit = index % 8;
  uint8_t cur;
  if (gbs_read_field(s, off + (uint32_t)byte, &cur, 1) != GBS_OK) return GBS_ERR_ARG;

  uint8_t next = visited ? (uint8_t)(cur | (1u << bit)) : (uint8_t)(cur & ~(1u << bit));
  if (next == cur) return GBS_OK;   /* untouched is untouched */

  GbsStatus st = gbs_write_field(s, off + (uint32_t)byte, &next, 1);
  if (st != GBS_OK) return st;
  return gbs_finish(s);
}

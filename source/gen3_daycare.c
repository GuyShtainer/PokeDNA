/* GENERATED egg-group table + Gen-3 Day-Care compatibility (pure C, host-testable).
 * Egg groups extracted from reference/pokeemerald_data/.../base_stats.h, packed one
 * byte per internal species id (low nibble = eggGroup1, high nibble = eggGroup2;
 * 15 = UNDISCOVERED). pk_daycare_compat replicates pokeemerald
 * GetDaycareCompatibilityScore (the Day-Care man's verdict). */
#include "gen3_daycare.h"
#include "data_tables.h"   /* growth rate, exp curve, base PP */
#include "learnsets2.h"    /* level-up lists */
#include <string.h>

#define EGG_UNDISCOVERED 15
#define DC_SPECIES_DITTO 132

static const uint8_t s_egg[413] = {  /* [internal id] = g1 | g2<<4 */
  0xFF,0x71,0x71,0x71,0xE1,0xE1,0xE1,0x21,0x21,0x21,0x33,0x33,0x33,0x33,0x33,0x33,
  0x44,0x44,0x44,0x55,0x55,0x44,0x44,0xE5,0xE5,0x65,0x65,0x55,0x55,0x51,0xFF,0xFF,
  0x51,0x51,0x51,0x66,0x66,0x55,0x55,0x66,0x66,0x44,0x44,0x77,0x77,0x77,0x73,0x73,
  0x33,0x33,0x55,0x55,0x55,0x55,0x52,0x52,0x55,0x55,0x55,0x55,0x22,0x22,0x22,0x88,
  0x88,0x88,0x88,0x88,0x88,0x77,0x77,0x77,0x99,0x99,0xAA,0xAA,0xAA,0x55,0x55,0x21,
  0x21,0xAA,0xAA,0x54,0x44,0x44,0x52,0x52,0xBB,0xBB,0x99,0x99,0xBB,0xBB,0xBB,0xAA,
  0x88,0x88,0x99,0x99,0xAA,0xAA,0x77,0x77,0x11,0x11,0x88,0x88,0x11,0xBB,0xBB,0x51,
  0x51,0x66,0x77,0x11,0xE2,0xE2,0xCC,0xCC,0x99,0x99,0x88,0x33,0x88,0x88,0x88,0x33,
  0x55,0xEC,0xEC,0x21,0xDD,0x55,0x55,0x55,0x55,0xAA,0x92,0x92,0x92,0x92,0x44,0x11,
  0xFF,0xFF,0xFF,0xE2,0xE2,0xE2,0xFF,0xFF,0x71,0x71,0x71,0x55,0x55,0x55,0x21,0x21,
  0x21,0x55,0x55,0x44,0x44,0x33,0x33,0x33,0x33,0x44,0xCC,0xCC,0xFF,0xFF,0xFF,0xFF,
  0x64,0x44,0x44,0x51,0x51,0x51,0x77,0x62,0x62,0xAA,0x22,0x76,0x76,0x76,0x55,0x77,
  0x77,0x33,0x52,0x52,0x55,0x55,0x44,0x21,0xBB,0xFF,0xBB,0x55,0x33,0x33,0x55,0x33,
  0xAA,0x65,0x65,0xCC,0x33,0x33,0x33,0x55,0x55,0x55,0xBB,0xBB,0x55,0x55,0x92,0xC2,
  0xC2,0x52,0x22,0x44,0x55,0x55,0xE2,0x55,0x55,0xAA,0x55,0x55,0xFF,0x88,0xFF,0xFF,
  0xFF,0x55,0x66,0xFF,0xFF,0xFF,0x11,0x11,0x11,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
  0xFF,0xFF,0xFF,0xFF,0xFF,0xE1,0xE1,0xE1,0x55,0x55,0x55,0x21,0x21,0x21,0x55,0x55,
  0x55,0x55,0x33,0x33,0x33,0x33,0x33,0x72,0x72,0x72,0x75,0x75,0x75,0x33,0x33,0xAA,
  0x44,0x44,0x76,0x76,0x85,0x42,0x42,0x32,0x32,0xC5,0xC5,0x65,0x65,0x55,0xAA,0xAA,
  0xAA,0x55,0x88,0xCC,0xCC,0xCC,0x92,0x92,0xE2,0xE2,0xCC,0xCC,0x33,0x33,0x33,0x88,
  0x88,0x55,0x55,0x55,0x55,0x52,0x52,0x52,0x87,0x87,0xA6,0xA6,0xAA,0xAA,0xFF,0x55,
  0x55,0x66,0x66,0x65,0x88,0x88,0xE4,0xE4,0xFF,0xBB,0xBB,0x76,0x55,0x55,0x55,0xBB,
  0xBB,0x71,0x51,0x51,0x51,0x22,0x22,0x22,0x55,0xBB,0xBB,0xE5,0x55,0xC2,0x11,0x11,
  0x11,0xB6,0x83,0x83,0x99,0x99,0x99,0x99,0xBB,0xBB,0xBB,0xEE,0xEE,0xEE,0xAA,0xAA,
  0xAA,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xBB,0xFF,
};

#define EGG_COUNT (int)(sizeof s_egg / sizeof s_egg[0])

uint8_t pk_egg_group(uint16_t internal, int which) {
  if (internal >= EGG_COUNT) return EGG_UNDISCOVERED;
  uint8_t b = s_egg[internal];
  return which ? (uint8_t)((b >> 4) & 0xF) : (uint8_t)(b & 0xF);
}

DcCompat pk_daycare_compat(uint16_t spA, uint32_t otA, uint16_t spB, uint32_t otB) {
  if (!spA || !spB) return DC_INCOMPATIBLE;
  uint8_t a0 = pk_egg_group(spA, 0), a1 = pk_egg_group(spA, 1);
  uint8_t b0 = pk_egg_group(spB, 0), b1 = pk_egg_group(spB, 1);
  if (a0 == EGG_UNDISCOVERED || b0 == EGG_UNDISCOVERED) return DC_INCOMPATIBLE;
  int dA = (spA == DC_SPECIES_DITTO), dB = (spB == DC_SPECIES_DITTO);
  if (dA && dB) return DC_INCOMPATIBLE;                 /* two Ditto */
  if (dA || dB) return (otA == otB) ? DC_LOW : DC_MED;  /* one Ditto */
  if (spA == spB) return (otA == otB) ? DC_MED : DC_HIGH;
  if (a0 == b0 || a0 == b1 || a1 == b0 || a1 == b1)     /* share an egg group */
    return (otA == otB) ? DC_LOW : DC_MED;
  return DC_INCOMPATIBLE;
}

const char* pk_daycare_compat_msg(DcCompat c) {
  switch (c) {
    case DC_HIGH: return "They get along very well!";      /* <=29 chars: fits the day-care panel */
    case DC_MED:  return "The two get along.";
    case DC_LOW:  return "They don't like each other.";
    default:      return "They'd rather be elsewhere.";
  }
}

const char* pk_daycare_yard_note(int boarders, int visitors) {
  if (visitors > 0) return "Others are just visiting.";
  return (boarders >= 2) ? "Both Day-Care slots are full."
                         : "The Day-Care holds 2 Pokemon.";
}

bool pk_daycare_can_breed(uint16_t spA, uint8_t genA, uint16_t spB, uint8_t genB) {
  if (!spA || !spB) return false;
  if (pk_egg_group(spA, 0) == EGG_UNDISCOVERED || pk_egg_group(spB, 0) == EGG_UNDISCOVERED) return false;
  int dA = (spA == DC_SPECIES_DITTO), dB = (spB == DC_SPECIES_DITTO);
  if (dA && dB) return false;
  if (dA || dB) return true;                            /* Ditto + breedable non-Ditto */
  return (genA == 0 && genB == 1) || (genA == 1 && genB == 0);   /* one M + one F */
}

/* ---- take-out growth (BACKLOG #373) ------------------------------------------------ */

static uint16_t dcg_rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t dcg_rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* One learn attempt (retail MonTryLearningNewMove + DeleteFirstMoveAndGiveMoveToMon):
 * already known -> nothing; free slot -> fill it; else slots 1..3 move to 0..2 WITH their
 * PP and PP-Ups and the new move takes slot 3 with base PP and no PP-Ups. */
static void dcg_learn(DcGrow* g, uint16_t mv, uint8_t lvl) {
  for (int k = 0; k < 4; k++) if (g->moves_after[k] == mv) return;
  uint16_t replaced = 0;
  int slot = -1;
  for (int k = 0; k < 4; k++) if (g->moves_after[k] == 0) { slot = k; break; }
  if (slot < 0) {
    replaced = g->moves_after[0];
    for (int k = 0; k < 3; k++) {
      g->moves_after[k] = g->moves_after[k + 1];
      g->pp_after[k]    = g->pp_after[k + 1];
      g->ppups_after[k] = g->ppups_after[k + 1];
    }
    slot = 3;
  }
  g->moves_after[slot] = mv;
  g->pp_after[slot]    = pk_move_pp(mv);
  g->ppups_after[slot] = 0;
  if (g->n_learn < DC_LEARN_MAX) {
    g->learn[g->n_learn].newmove  = mv;
    g->learn[g->n_learn].replaced = replaced;
    g->learn[g->n_learn].at_level = lvl;
  }
  g->n_learn++;
  g->overflow = g->n_learn > DC_LEARN_MAX;
}

bool gen3_dc_preview(PkGame learnset_game, const uint8_t* rec, bool is_party,
                     uint32_t steps, DcGrow* out) {
  if (!rec || !out) return false;
  EditMon e;
  gen3_edit_load(rec, is_party, &e);
  uint16_t species = dcg_rd16(e.sub[0] + 0);
  if (species == 0 || (e.raw[0x13] & 0x04)) return false;      /* empty / egg: never grows */

  memset(out, 0, sizeof(*out));
  uint8_t gr = pk_species_growth(species);
  out->exp_before = dcg_rd32(e.sub[0] + 4);
  out->lv_before  = pk_level_from_exp(gr, out->exp_before);
  out->exp_after  = out->exp_before;
  out->lv_after   = out->lv_before;
  for (int k = 0; k < 4; k++) {
    out->moves_after[k] = dcg_rd16(e.sub[1] + k * 2);
    out->pp_after[k]    = e.sub[1][8 + k];
    out->ppups_after[k] = em_get_ppups(&e, k);
  }
  if (out->lv_before >= 100 || steps == 0) return true;        /* retail's != MAX_LEVEL guard */

  uint32_t cap = pk_exp_for_level(gr, 100);
  uint64_t sum = (uint64_t)out->exp_before + steps;
  out->exp_after = sum > cap ? cap : (uint32_t)sum;
  out->lv_after  = pk_level_from_exp(gr, out->exp_after);

  const uint16_t* list;
  int n = lg2_levelup_list(learnset_game, species, &list);
  for (int L = out->lv_before + 1; L <= out->lv_after; L++) {
    for (int i = 0; i < n; i++) {
      if (LG2_LV_LEVEL(list[i]) != L) continue;
      uint16_t mv = LG2_LV_MOVE(list[i]);
      if (mv != 0) dcg_learn(out, mv, (uint8_t)L);
    }
  }
  return true;
}

void gen3_dc_apply(EditMon* e, const DcGrow* g, bool with_moves) {
  if (!e || !g) return;
  em_set_exp(e, g->exp_after);
  if (!with_moves) return;
  for (int k = 0; k < 4; k++) {      /* em_set_move resets PP + clears PP-Ups: set those AFTER.
                                      * PP-Ups BEFORE the raw PP: em_set_ppups also bumps current
                                      * PP by what the bump buys, so the exact PP goes last. */
    em_set_move(e, k, g->moves_after[k]);
    em_set_ppups(e, k, g->ppups_after[k]);
    em_set_pp(e, k, g->pp_after[k]);
  }
}

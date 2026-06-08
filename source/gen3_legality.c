#include "gen3_legality.h"
#include "data_tables.h"
#include "learnsets.h"

/* fixed-string messages only (keeps this module pure C — no siprintf/stdio). */
static void add(PkLegality* L, uint8_t sev, const char* t) {
  if (L->n >= 24) return;
  int i = 0;
  for (; t[i] && i < 39; i++) L->issue[L->n].text[i] = t[i];
  L->issue[L->n].text[i] = 0;
  L->issue[L->n].sev = sev;
  if (sev) L->ok = false;
  L->n++;
}

PkLegality pk_check_legality(const PkMon* m) {
  PkLegality L; L.ok = true; L.n = 0;

  if (m->isBadEgg) { add(&L, 1, "Bad egg (checksum failed)"); return L; }
  if (m->species < 1 || m->species > 411) { add(&L, 1, "Species id out of range"); return L; }

  /* level vs EXP (party levels are plaintext; a hacked level won't match the exp
   * band; box levels are computed so always consistent) */
  uint8_t growth = pk_species_growth(m->species);
  if (m->level < 1 || m->level > 100) add(&L, 1, "Level out of 1..100");
  else if (pk_level_from_exp(growth, m->experience) != m->level)
    add(&L, 1, "Level does not match EXP");

  /* EV total cap */
  int evsum = 0;
  for (int i = 0; i < 6; i++) evsum += m->evs[i];
  if (evsum > 510) add(&L, 1, "EV total over 510");

  /* ability: the bit is always 0/1; flag a 2nd slot on a single-ability species */
  if (m->abilityNum == 1 &&
      pk_species_ability(m->species, 1) == pk_species_ability(m->species, 0))
    add(&L, 0, "2nd ability but species has one");

  /* moves: present, in range, no duplicates, PP within the PP-up max */
  if (!m->isEgg && m->moves[0] == 0) add(&L, 1, "No moves");
  for (int i = 0; i < 4; i++) {
    uint16_t mv = m->moves[i];
    if (mv == 0) continue;
    if (mv > 354) { add(&L, 1, "Move id out of range"); continue; }
    for (int j = 0; j < i; j++)
      if (m->moves[j] == mv) { add(&L, 1, "Duplicate move"); break; }
    uint8_t base = pk_move_pp(mv);
    uint8_t ups = (uint8_t)((m->ppBonuses >> (i * 2)) & 3);
    uint8_t maxpp = (uint8_t)(base + base / 5 * ups);
    if (m->pp[i] > maxpp) add(&L, 1, "PP above maximum");
    /* move-source legality (V2): warn-only, and independent of the PP check
     * above (a doubly-tampered move can fail both). pk_can_learn over-accepts
     * every TM/HM/tutor move, so this never false-flags a legit mon; a hit means
     * no Gen-3 level-up/egg/TM/tutor method teaches this move to the species line
     * (e.g. a signature move on the wrong species, or a later-gen move). */
    if (!m->isEgg && !pk_can_learn(m->species, mv))
      add(&L, 0, "Move not learnable by species");
  }

  if (m->heldItem > 376) add(&L, 1, "Held item id invalid");
  if (m->metLevel > m->level) add(&L, 1, "Met level above current level");
  if (m->metLevel > 100) add(&L, 1, "Met level above 100");      /* impossible */
  if (m->pokeball < 1 || m->pokeball > 12) add(&L, 0, "Unusual Poke Ball id");

  /* Origin game (the 4-bit "game of origin"): 1 Sapphire, 2 Ruby, 3 Emerald,
   * 4 FireRed, 5 LeafGreen, 15 Colosseum/XD. Anything else can't occur on a legit
   * Gen-3 mon. Warn-only (conservative): a non-egg with a 0 origin is the common
   * crude-hack tell, but we never hard-fail on it. Eggs carry their breed game. */
  if (m->metGame == 0 || (m->metGame > 5 && m->metGame != 15))
    add(&L, 0, "Unusual origin game");

  /* V1 + V2 move-source done above. The species-vs-met-LOCATION check ("Skitty
   * can't appear on Route 101") is intentionally NOT done: reliably (zero false
   * positives) it needs per-game MAPSEC tables, MAP->MAPSEC aggregation across
   * ~480 maps/game, wild tables, evolved-mon skipping, and a curated special-
   * source (starter/fossil/gift/static/legendary/trade) exclusion set — a
   * PKHeX-scale dataset. Left as a dedicated milestone. */
  return L;
}

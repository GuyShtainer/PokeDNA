#include "gen3_stars.h"
#include "gen3_flags.h"    /* frontier symbol flags */
#include "gen3_dex.h"      /* seen/owned setters (all mirrors) */
#include "gen3_edit.h"     /* gen3_encode_char (museum plaque strings) */
#include <string.h>

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static void     wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void     wr32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Per-game achievement kinds, in display order (== star order on the card). */
enum { ACH_HOF, ACH_DEX, ACH_MUSEUM, ACH_FRONTIER, ACH_TOWER,
       ACH_KANTO, ACH_NATDEX, ACH_MINIGAME };
static const uint8_t E_ACH[4]    = { ACH_HOF, ACH_DEX,   ACH_MUSEUM, ACH_FRONTIER };
static const uint8_t RS_ACH[4]   = { ACH_HOF, ACH_DEX,   ACH_TOWER,  ACH_MUSEUM   };
static const uint8_t FRLG_ACH[4] = { ACH_HOF, ACH_KANTO, ACH_NATDEX, ACH_MINIGAME };

/* SB1 offset of the 5 museum-painting ContestWinner slots (0x20 apiece,
 * species u16 @ +8): Emerald contestWinners[8] = 0x2E90 + 8*0x20; RS has a
 * dedicated museumPortraits[5] array. FRLG has no museum. */
static uint16_t museum_off(PkGame g) { return g == PK_EMERALD ? 0x2F90 : 0x2EFC; }
#define RS_TOWER_STREAK 0x0572     /* SB2 u16 bestBattleTowerWinStreak (RS only) */
/* FRLG minigame records (SB2, plaintext; pokefirered include/global.h):
 * pokeJump @ 0xB00 (u16 jumpsInRow first), berryPick @ 0xB10 (u32 bestScore,
 * then u16 berriesPicked @ +4). */
#define FRLG_JUMPS_IN_ROW   0x0B00
#define FRLG_BERRIES_PICKED 0x0B14

/* The three FRLG dex ranges of HasAllMons (pokefirered src/pokedex.c): Kanto
 * minus Mew, Johto minus Lugia/Ho-Oh/Celebi, Hoenn minus Jirachi/Deoxys.
 * HasAllKantoMons is ranges[0] alone. */
static const uint16_t FRLG_DEX_RANGE[3][2] = { {1, 150}, {152, 248}, {252, 384} };

static int ach_kind(PkGame g, int i) {
  if (i < 0 || i > 3) return -1;
  if (g == PK_EMERALD) return E_ACH[i];
  if (g == PK_RS)      return RS_ACH[i];
  return FRLG_ACH[i];
}

int pk_star_ach_count(PkGame g) { (void)g; return 4; }

const char* pk_star_ach_name(PkGame g, int i) {
  switch (ach_kind(g, i)) {
    case ACH_HOF:      return "Hall of Fame";
    case ACH_DEX:      return "Hoenn dex 200";
    case ACH_MUSEUM:   return "5 museum paint";
    case ACH_FRONTIER: return "14 front. syms";
    case ACH_TOWER:    return "Tower 50-streak";
    case ACH_KANTO:    return "Kanto dex 150";
    case ACH_NATDEX:   return "National dex";
    case ACH_MINIGAME: return "Berry&Jump 200";
  }
  return "";
}

bool pk_star_ach_is_dex(PkGame g, int i) {
  int k = ach_kind(g, i);
  return k == ACH_DEX || k == ACH_KANTO || k == ACH_NATDEX;
}

/* all species in FRLG dex range r (and below it, for ACH_NATDEX) caught? */
static bool frlg_ranges_owned(const uint8_t* sb2, int nranges) {
  for (int r = 0; r < nranges; r++)
    for (uint16_t n = FRLG_DEX_RANGE[r][0]; n <= FRLG_DEX_RANGE[r][1]; n++)
      if (!pk_dex_owned(sb2, n)) return false;
  return true;
}

bool pk_star_ach_done(const uint8_t* sb1, const uint8_t* sb2, PkGame g, int i,
                      const uint16_t* hoenn200) {
  switch (ach_kind(g, i)) {
    case ACH_HOF:                            /* RS counts the debut TIME, E the stat,
                                              * FRLG gates the time on the stat     */
      return g == PK_RS      ? pk_game_stat(sb1, sb2, g, PK_STAT_FIRST_HOF_PLAY_TIME) != 0
           : g == PK_EMERALD ? pk_game_stat(sb1, sb2, g, PK_STAT_ENTERED_HOF) != 0
           : pk_game_stat(sb1, sb2, g, PK_STAT_ENTERED_HOF) != 0 &&
             pk_game_stat(sb1, sb2, g, PK_STAT_FIRST_HOF_PLAY_TIME) != 0;
    case ACH_DEX:
      if (!hoenn200) return false;
      for (int k = 0; k < 200; k++) if (!pk_dex_owned(sb2, hoenn200[k])) return false;
      return true;
    case ACH_MUSEUM:
      for (int k = 0; k < 5; k++)
        if (rd16(sb1 + museum_off(g) + k * 0x20 + 8) == 0) return false;
      return true;
    case ACH_FRONTIER:
      for (int k = 0; k < 14; k++)
        if (!pk_flag_get(sb1, g, pk_frontier_flag(g, k))) return false;
      return true;
    case ACH_TOWER:
      return rd16(sb2 + RS_TOWER_STREAK) > 49;
    case ACH_KANTO:
      return frlg_ranges_owned(sb2, 1);
    case ACH_NATDEX:
      return frlg_ranges_owned(sb2, 3);
    case ACH_MINIGAME:
      return rd16(sb2 + FRLG_BERRIES_PICKED) >= 200 && rd16(sb2 + FRLG_JUMPS_IN_ROW) >= 200;
  }
  return false;
}

int pk_star_count(const uint8_t* sb1, const uint8_t* sb2, PkGame g,
                  const uint16_t* hoenn200) {
  int n = 0;
  for (int i = 0; i < pk_star_ach_count(g); i++)
    if (pk_star_ach_done(sb1, sb2, g, i, hoenn200)) n++;
  return n;
}

bool pk_star_ach_can_set(PkGame g, int i, const uint16_t* hoenn200) {
  int k = ach_kind(g, i);
  if (k < 0) return false;
  return k != ACH_DEX || hoenn200 != 0;      /* dex needs the generated table */
}

/* Fill one empty museum slot with a plausible master-rank winner: the player's
 * own identity as the artist and PIKACHU (internal id 25 == national) as the
 * subject, category = the slot's contest type. Layout is shared by RS and
 * Emerald (Emerald appends contestRank @ +30). */
static void museum_fill(uint8_t* w, const uint8_t* sb2, PkGame g, int cat) {
  static const char* MON = "PIKACHU";
  uint32_t id = (uint32_t)rd16(sb2 + 0x0A) | ((uint32_t)rd16(sb2 + 0x0C) << 16);
  memset(w, 0, 0x20);
  wr32(w + 0, id);                           /* personality (only palettes the art) */
  wr32(w + 4, id);                           /* OT id = the player                  */
  wr16(w + 8, 25);                           /* species                             */
  w[10] = (uint8_t)cat;
  memset(w + 11, 0xFF, 11);                  /* mon nickname (0xFF = EOS + pad)     */
  for (int k = 0; MON[k]; k++) w[11 + k] = gen3_encode_char(MON[k]);
  memcpy(w + 22, sb2, 8);                    /* trainer name, already Gen-3 encoded */
  if (g == PK_EMERALD) w[30] = 3;            /* CONTEST_RANK_MASTER                 */
}

int pk_star_ach_set(uint8_t* sb1, uint8_t* sb2, PkGame g, int i, bool on,
                    const uint16_t* hoenn200) {
  switch (ach_kind(g, i)) {
    case ACH_HOF:
      if (on) {                              /* debut "now": the current play time  */
        uint32_t pt = ((uint32_t)rd16(sb2 + 0x0E) << 16) | ((uint32_t)sb2[0x10] << 8) | sb2[0x11];
        pk_set_game_stat(sb1, sb2, g, PK_STAT_FIRST_HOF_PLAY_TIME, pt ? pt : 1);
        pk_set_game_stat(sb1, sb2, g, PK_STAT_ENTERED_HOF, 1);
      } else {
        pk_set_game_stat(sb1, sb2, g, PK_STAT_FIRST_HOF_PLAY_TIME, 0);
        pk_set_game_stat(sb1, sb2, g, PK_STAT_ENTERED_HOF, 0);
      }
      return 1;
    case ACH_DEX:
      if (!hoenn200) return 0;
      for (int k = 0; k < 200; k++) {
        pk_dex_set_owned(sb2, hoenn200[k], on);
        if (on) pk_dex_set_seen(sb1, sb2, g, hoenn200[k], true);   /* OFF keeps seen */
      }
      return on ? 3 : 2;                     /* seen mirrors live in SB1 too        */
    case ACH_MUSEUM:
      for (int k = 0; k < 5; k++) {
        uint8_t* w = sb1 + museum_off(g) + k * 0x20;
        if (!on)                  memset(w, 0, 0x20);
        else if (rd16(w + 8) == 0) museum_fill(w, sb2, g, k);      /* keep real wins */
      }
      return 1;
    case ACH_FRONTIER:
      for (int k = 0; k < 14; k++) pk_flag_set(sb1, g, pk_frontier_flag(g, k), on);
      return 1;
    case ACH_TOWER:                          /* OFF: 49 = just under the bar        */
      wr16(sb2 + RS_TOWER_STREAK, on ? 50 : 49);
      return 2;
    case ACH_KANTO:                          /* OFF keeps seen (like ACH_DEX)       */
    case ACH_NATDEX: {
      /* NATDEX touches all three ranges; KANTO only the first. Turning NATDEX
       * OFF clears the Johto+Hoenn ranges only — un-catching Kanto is the
       * Kanto star's own toggle (either OFF already breaks the National star). */
      int k = ach_kind(g, i);
      int lo = (k == ACH_NATDEX && !on) ? 1 : 0, hi = (k == ACH_KANTO) ? 1 : 3;
      for (int r = lo; r < hi; r++)
        for (uint16_t n = FRLG_DEX_RANGE[r][0]; n <= FRLG_DEX_RANGE[r][1]; n++) {
          pk_dex_set_owned(sb2, n, on);
          if (on) pk_dex_set_seen(sb1, sb2, g, n, true);
        }
      return on ? 3 : 2;                     /* seen mirrors live in SB1 too        */
    }
    case ACH_MINIGAME:                       /* only nudge across the 200 bar: ON
                                              * lifts sub-200 records to 200, OFF
                                              * caps 200+ records at 199           */
      if (on) {
        if (rd16(sb2 + FRLG_BERRIES_PICKED) < 200) wr16(sb2 + FRLG_BERRIES_PICKED, 200);
        if (rd16(sb2 + FRLG_JUMPS_IN_ROW) < 200)   wr16(sb2 + FRLG_JUMPS_IN_ROW, 200);
      } else {
        if (rd16(sb2 + FRLG_BERRIES_PICKED) >= 200) wr16(sb2 + FRLG_BERRIES_PICKED, 199);
        if (rd16(sb2 + FRLG_JUMPS_IN_ROW) >= 200)   wr16(sb2 + FRLG_JUMPS_IN_ROW, 199);
      }
      return 2;
  }
  return 0;
}

/*
 * Emerald Battle Frontier streak tables + safe writers. See gen3_frontier.h for the
 * layout provenance and the two footguns (winStreakActiveFlags lockstep, and the
 * exact-match Frontier Brain thresholds).
 *
 * Pure C — no tonc, no GBA headers — so tests/host_frontier_test.c runs it on the PC.
 */
#include <string.h>
#include "gen3_frontier.h"

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void     wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* CURRENT / RECORD base offsets + how many battle modes each facility has.
 * Self-consistency check that this table is right: each facility's RECORD array
 * ends exactly where the next named field begins — factory record ends at 0xDF2,
 * which is factoryRentsCount (the offset pret's own comment gets wrong). */
static const struct { uint16_t cur, rec; uint8_t modes; } k_fac[G3F_FACILITIES] = {
  { 0xCE0, 0xCF0, 4 },   /* Tower:   singles / doubles / multis / link-multis */
  { 0xD0C, 0xD14, 2 },   /* Dome:    singles / doubles                        */
  { 0xDC8, 0xDD0, 2 },   /* Palace:  singles / doubles                        */
  { 0xDDA, 0xDDE, 1 },   /* Arena:   singles only                             */
  { 0xDE2, 0xDEA, 2 },   /* Factory: singles / doubles                        */
  { 0xE04, 0xE08, 1 },   /* Pike:    singles only                             */
  { 0xE1A, 0xE1E, 1 },   /* Pyramid: singles only                             */
};

/* winStreakActiveFlags bit per (facility, mode, lvl). NOT sequential — the doubles
 * and multis lanes were appended at bits 14..25 after the original 0..13 block.
 * -1 = the facility has no such lane. Verbatim from constants/frontier_util.h. */
static const int8_t k_bit[G3F_FACILITIES][4][2] = {
  /* Tower   */ { { 0,  1 }, { 14, 15 }, { 16, 17 }, { 18, 19 } },
  /* Dome    */ { { 2,  3 }, { 20, 21 }, { -1, -1 }, { -1, -1 } },
  /* Palace  */ { { 4,  5 }, { 22, 23 }, { -1, -1 }, { -1, -1 } },
  /* Arena   */ { { 6,  7 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
  /* Factory */ { { 8,  9 }, { 24, 25 }, { -1, -1 }, { -1, -1 } },
  /* Pike    */ { { 10, 11 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
  /* Pyramid */ { { 12, 13 }, { -1, -1 }, { -1, -1 }, { -1, -1 } },
};

/* sFrontierBrainStreakAppearances[facility] = { silver, gold, repeat step, modifier }.
 * src/frontier_util.c:86-95. Only [0], [1] and [3] matter to an editor. */
static const uint16_t k_brain[G3F_FACILITIES][4] = {
  { 35,  70, 35, 1 },   /* Tower   */
  {  4,   9,  5, 0 },   /* Dome    */
  { 21,  42, 21, 1 },   /* Palace  */
  { 28,  56, 28, 1 },   /* Arena   */
  { 21,  42, 21, 1 },   /* Factory */
  { 28, 140, 56, 1 },   /* Pike    */
  { 21,  70, 35, 0 },   /* Pyramid */
};

static const char* const k_fac_name[G3F_FACILITIES] = {
  "Battle Tower", "Battle Dome", "Battle Palace", "Battle Arena",
  "Battle Factory", "Battle Pike", "Battle Pyramid",
};
static const char* const k_mode_name[4] = { "Singles", "Doubles", "Multis", "Link multis" };

static bool fac_ok(int f) { return f >= 0 && f < G3F_FACILITIES; }

int g3f_modes(int facility) { return fac_ok(facility) ? k_fac[facility].modes : 0; }

const char* g3f_facility_name(int facility) {
  return fac_ok(facility) ? k_fac_name[facility] : 0;
}

const char* g3f_mode_name(int facility, int mode) {
  if (!fac_ok(facility) || mode < 0 || mode >= k_fac[facility].modes) return 0;
  /* Singles-only facilities read better without a redundant "Singles" label. */
  if (k_fac[facility].modes == 1) return "";
  return k_mode_name[mode];
}

int g3f_streak_cap(int facility) {
  return (facility == G3F_PYRAMID) ? G3F_MAX_PYRAMID : G3F_MAX_STREAK;
}

int g3f_lane_off(int facility, int mode, int lvl, int kind) {
  if (!fac_ok(facility) || lvl < 0 || lvl > 1) return -1;
  if (mode < 0 || mode >= k_fac[facility].modes) return -1;
  if (kind != G3F_CURRENT && kind != G3F_RECORD) return -1;
  uint16_t base = (kind == G3F_CURRENT) ? k_fac[facility].cur : k_fac[facility].rec;
  return (int)base + (mode * 2 + lvl) * 2;      /* lvlMode is the INNER index */
}

int g3f_streak_get(const uint8_t* sb2, int facility, int mode, int lvl, int kind) {
  int off = g3f_lane_off(facility, mode, lvl, kind);
  if (!sb2 || off < 0) return -1;
  return (int)rd16(sb2 + off);
}

int g3f_streak_set(uint8_t* sb2, int facility, int mode, int lvl, int kind, int value) {
  int off = g3f_lane_off(facility, mode, lvl, kind);
  if (!sb2 || off < 0 || off + 2 > G3F_SB2_LIMIT) return -1;
  int cap = g3f_streak_cap(facility);
  if (value < 0) value = 0;
  if (value > cap) value = cap;
  wr16(sb2 + off, (uint16_t)value);
  return value;
}

int g3f_active_bit(int facility, int mode, int lvl) {
  if (!fac_ok(facility) || lvl < 0 || lvl > 1) return -1;
  if (mode < 0 || mode >= k_fac[facility].modes) return -1;
  return k_bit[facility][mode][lvl];
}

bool g3f_active_get(const uint8_t* sb2, int facility, int mode, int lvl) {
  int b = g3f_active_bit(facility, mode, lvl);
  if (!sb2 || b < 0) return false;
  return (rd32(sb2 + G3F_ACTIVE_OFF) >> b) & 1u;
}

void g3f_active_set(uint8_t* sb2, int facility, int mode, int lvl, bool on) {
  int b = g3f_active_bit(facility, mode, lvl);
  if (!sb2 || b < 0) return;
  uint32_t v = rd32(sb2 + G3F_ACTIVE_OFF);
  if (on) v |= (1u << b); else v &= ~(1u << b);
  wr32(sb2 + G3F_ACTIVE_OFF, v);
}

int g3f_set_current(uint8_t* sb2, int facility, int mode, int lvl, int value, bool raise_record) {
  int w = g3f_streak_set(sb2, facility, mode, lvl, G3F_CURRENT, value);
  if (w < 0) return -1;
  /* Lockstep: a nonzero streak whose bit is clear is zeroed by the facility's own
   * challenge-init the moment the player walks in. */
  g3f_active_set(sb2, facility, mode, lvl, w > 0);
  if (raise_record) {
    int rec = g3f_streak_get(sb2, facility, mode, lvl, G3F_RECORD);
    if (rec >= 0 && rec < w) g3f_streak_set(sb2, facility, mode, lvl, G3F_RECORD, w);
  }
  return w;
}

bool g3f_challenge_active(const uint8_t* sb2) {
  if (!sb2) return false;
  return sb2[G3F_CHALLENGE_STATUS_OFF] != 0 || (sb2[G3F_BITFIELD_OFF] & 0x04) != 0;
}
int g3f_challenge_status(const uint8_t* sb2) { return sb2 ? sb2[G3F_CHALLENGE_STATUS_OFF] : 0; }
int g3f_lvl_mode(const uint8_t* sb2)         { return sb2 ? (sb2[G3F_BITFIELD_OFF] & 0x03) : 0; }
int g3f_cur_battle_num(const uint8_t* sb2)   { return sb2 ? (int)rd16(sb2 + G3F_CUR_BATTLE_NUM_OFF) : 0; }

int g3f_u16_get(const uint8_t* sb2, int off) {
  if (!sb2 || off < 0 || off + 2 > G3F_SB2_LIMIT) return -1;
  return (int)rd16(sb2 + off);
}

void g3f_u16_set(uint8_t* sb2, int off, int value, int cap) {
  if (!sb2 || off < 0 || off + 2 > G3F_SB2_LIMIT) return;
  if (value < 0) value = 0;
  if (cap > 0 && value > cap) value = cap;
  wr16(sb2 + off, (uint16_t)value);
}

int g3f_brain_tier(int facility, int symbols_owned) {
  if (!fac_ok(facility)) return -1;
  /* symbolsCount indexes the threshold table directly for 0 and 1. At 2 the game
   * takes the "already have both" path, which accepts arr[0] (and arr[1], and
   * arr[1] + n*step) — arr[0] is the cheapest to reach, so offer that. */
  return (symbols_owned == 1) ? 1 : 0;
}

int g3f_brain_target(int facility, int symbols_owned) {
  int tier = g3f_brain_tier(facility, symbols_owned);
  if (tier < 0) return -1;
  int thr = (int)k_brain[facility][tier];
  int mod = (int)k_brain[facility][3];
  int v = thr - mod;                     /* the value to STORE, not the threshold */
  if (v < 0) v = 0;
  int cap = g3f_streak_cap(facility);
  return v > cap ? cap : v;
}

/* ---- Ruby / Sapphire ------------------------------------------------------- */

int g3f_rs_record(const uint8_t* sb2, int lvl) {
  if (!sb2 || lvl < 0 || lvl > 1) return -1;
  return (int)rd16(sb2 + G3F_RS_RECORD_STREAKS_OFF + lvl * 2);
}

int g3f_rs_set_record(uint8_t* sb2, int lvl, int value) {
  if (!sb2 || lvl < 0 || lvl > 1) return -1;
  if (value < 0) value = 0;
  if (value > G3F_MAX_STREAK) value = G3F_MAX_STREAK;
  wr16(sb2 + G3F_RS_RECORD_STREAKS_OFF + lvl * 2, (uint16_t)value);
  /* 0x572 is a cache the game recomputes as max(record[0], record[1]); refresh it
   * so the display agrees immediately instead of only after the next tower battle. */
  int a = (int)rd16(sb2 + G3F_RS_RECORD_STREAKS_OFF);
  int b = (int)rd16(sb2 + G3F_RS_RECORD_STREAKS_OFF + 2);
  wr16(sb2 + G3F_RS_BEST_STREAK_OFF, (uint16_t)(a > b ? a : b));
  return value;
}

bool g3f_supported(PkGame game) {
  return game == PK_EMERALD || game == PK_RS;   /* FRLG has no streak block at all */
}

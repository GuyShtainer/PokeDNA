/* Mirage Island. See gen3_mirage.h for the mechanic and for why the write is a pre-image. */
#include "gen3_mirage.h"
#include "gen3_save.h"
#include <string.h>

/* vars[] sits immediately after flags[]: Emerald 0x1270+300 = 0x139C, RS 0x1220+288 = 0x1340.
 * VAR_MIRAGE_RND_H is var index 0x24 past VARS_START, VAR_DAYS is index 0x40. Plaintext —
 * Emerald's security key does not obfuscate vars[]. */
#define MIR_H_EM   0x13E4u
#define MIR_H_RS   0x1388u
#define DAYS_EM    0x141Cu
#define DAYS_RS    0x13C0u

/* FLAG_SYS_CLOCK_SET. Without it the game never runs the daily catch-up at all. */
#define CLOCKSET_FLAG_EM 0x895
#define CLOCKSET_FLAG_RS 0x835

/* The game's own LCG. `M - 1` is even and 12345 is odd, which is why there is no fixed point
 * and why no party can hold the island for three consecutive days. */
#define LCG_M 1103515245u
#define LCG_C 12345u
/* Modular inverse of LCG_M mod 2^32 (M is odd, so it exists). Newton's method converges in 5
 * doublings from a 3-bit seed; done once here as a constant so the device does no work. */
#define LCG_MINV 4005161829u   /* 1103515245 * 4005161829 == 1 (mod 2^32) */

#define PARTY_OFF   0x238u     /* SaveBlock1.playerParty — same offset in RSE            */
#define PARTY_CNT   0x234u     /* playerPartyCount                                        */
#define MON_SIZE    100u

static bool off_for(PkGame g, uint32_t* mir, uint32_t* days) {
  if (g == PK_EMERALD) { *mir = MIR_H_EM; *days = DAYS_EM; return true; }
  if (g == PK_RS)      { *mir = MIR_H_RS; *days = DAYS_RS; return true; }
  return false;                                   /* FRLG has no Mirage Island at all */
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void     wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

bool mirage_get(const uint8_t* sb1, PkGame game, uint16_t* hi, uint16_t* lo) {
  uint32_t m, d;
  if (!sb1 || !off_for(game, &m, &d)) return false;
  if (hi) *hi = rd16(sb1 + m);
  if (lo) *lo = rd16(sb1 + m + 2);
  return true;
}

bool mirage_set(uint8_t* sb1, PkGame game, uint16_t hi, uint16_t lo) {
  uint32_t m, d;
  if (!sb1 || !off_for(game, &m, &d)) return false;
  wr16(sb1 + m, hi);
  wr16(sb1 + m + 2, lo);
  return true;
}

int mirage_party_keys(const uint8_t* sb1, uint16_t* out_key, uint8_t* out_slot) {
  int n = 0;
  if (!sb1 || !out_key) return 0;
  int cnt = sb1[PARTY_CNT];
  if (cnt > MIRAGE_PARTY_SLOTS) cnt = MIRAGE_PARTY_SLOTS;
  for (int i = 0; i < cnt; i++) {
    const uint8_t* mon = sb1 + PARTY_OFF + (uint32_t)i * MON_SIZE;
    /* The game's gate is `species != 0` read through GetMonData, which returns a real species
     * for an egg (only a BAD egg reports SPECIES_EGG) — so eggs are valid donors and must be
     * offered. Reading the species means decrypting; the personality and OT id are plaintext at
     * +0x00/+0x04, and the encrypted substructs start at +0x20. Rather than decrypt here, treat
     * a nonzero personality with a plausible checksum as an occupied slot: an empty party slot
     * in Gen 3 is all zeroes. */
    uint32_t pers = (uint32_t)mon[0] | ((uint32_t)mon[1] << 8) |
                    ((uint32_t)mon[2] << 16) | ((uint32_t)mon[3] << 24);
    if (!pers) continue;
    out_key[n] = (uint16_t)(pers & 0xFFFFu);
    if (out_slot) out_slot[n] = (uint8_t)i;
    n++;
  }
  return n;
}

bool mirage_present(const uint8_t* sb1, PkGame game) {
  uint16_t hi, lo;
  if (!mirage_get(sb1, game, &hi, &lo)) return false;
  uint16_t key[MIRAGE_PARTY_SLOTS];
  int n = mirage_party_keys(sb1, key, 0);
  for (int i = 0; i < n; i++) if (key[i] == hi) return true;
  return false;
}

int mirage_days_owed(const uint8_t* sb1, PkGame game, int cur_day) {
  uint32_t m, d;
  if (!sb1 || !off_for(game, &m, &d)) return 0;
  /* UpdatePerDay only runs when the player has set the clock. */
  int cf = (game == PK_EMERALD) ? CLOCKSET_FLAG_EM : CLOCKSET_FLAG_RS;
  /* flags[] base: Emerald 0x1270, RS 0x1220 — the same bases gen3_flags.c pins. */
  uint32_t fbase = (game == PK_EMERALD) ? 0x1270u : 0x1220u;
  if (!((sb1[fbase + (uint32_t)cf / 8] >> (cf & 7)) & 1)) return 0;

  int stored = (int)rd16(sb1 + d);
  int owed = cur_day - stored;
  /* The game's guard is `VAR_DAYS != today && VAR_DAYS <= today`, so a stored day in the FUTURE
   * (an RTC that has been wound back) means no roll at all, not a negative one. */
  return owed > 0 ? owed : 0;
}

void mirage_advance(uint16_t hi, uint16_t lo, int steps, uint16_t* out_hi, uint16_t* out_lo) {
  uint32_t v = ((uint32_t)hi << 16) | lo;
  for (int i = 0; i < steps; i++) v = LCG_M * v + LCG_C;
  if (out_hi) *out_hi = (uint16_t)(v >> 16);
  if (out_lo) *out_lo = (uint16_t)(v & 0xFFFFu);
}

void mirage_solve(uint16_t target_hi, uint16_t target_lo, int steps,
                  uint16_t* out_hi, uint16_t* out_lo) {
  /* Step the LCG BACKWARDS: x = Minv * (y - C). Exact on 32 bits because M is odd, so the map
   * is a bijection and every target has exactly one pre-image per step count. */
  uint32_t v = ((uint32_t)target_hi << 16) | target_lo;
  for (int i = 0; i < steps; i++) v = LCG_MINV * (v - LCG_C);
  if (out_hi) *out_hi = (uint16_t)(v >> 16);
  if (out_lo) *out_lo = (uint16_t)(v & 0xFFFFu);
}

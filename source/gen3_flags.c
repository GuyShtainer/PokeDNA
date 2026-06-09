#include "gen3_flags.h"

/* SaveBlock1 flags[] base offset per game family. */
static uint16_t flags_off(PkGame g) { return g == PK_FRLG ? 0x0EE0 : g == PK_EMERALD ? 0x1270 : 0x1220; }

int pk_flags_count(PkGame g) { return g == PK_EMERALD ? 812 * 8 : 800 * 8; }

bool pk_flag_get(const uint8_t* sb1, PkGame g, int n) {
  if (n < 0 || n >= pk_flags_count(g)) return false;
  return (sb1[flags_off(g) + (n >> 3)] >> (n & 7)) & 1;
}

void pk_flag_set(uint8_t* sb1, PkGame g, int n, bool v) {
  if (n < 0 || n >= pk_flags_count(g)) return;
  uint8_t* b = &sb1[flags_off(g) + (n >> 3)];
  uint8_t m = (uint8_t)(1 << (n & 7));
  if (v) *b |= m; else *b &= (uint8_t)~m;
}

/* FLAG_BADGE01_GET resolves to SYSTEM_FLAGS + 0x7; the bases are E 0x860 / FRLG
 * 0x800 / RS 0x800, so badge1 = 0x867 / 0x820 / 0x807. */
int pk_badge_flag(PkGame g, int badge) {
  if (badge < 0 || badge > 7) return -1;
  int base = (g == PK_EMERALD) ? 0x867 : (g == PK_FRLG) ? 0x820 : 0x807;
  return base + badge;
}

/* Emerald frontier symbols start at SYSTEM_FLAGS(0x860)+0x64 = 0x8C4 (Tower Silver). */
int pk_frontier_flag(PkGame g, int sym) {
  if (g != PK_EMERALD || sym < 0 || sym > 13) return -1;
  return 0x8C4 + sym;
}

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

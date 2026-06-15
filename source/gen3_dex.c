#include "gen3_dex.h"

/* Within SaveBlock2: pokedex struct @ 0x18; owned[] @ +0x10, seen[] @ +0x44. */
#define DEX_OWNED 0x28   /* 0x18 + 0x10 */
#define DEX_SEENA 0x5C   /* 0x18 + 0x44 */

/* The two extra "seen" copies in SaveBlock1, per game (from each decomp's global.h). */
static void dex_sb1_seen(PkGame g, int* s1, int* s2) {
  switch (g) {
    case PK_EMERALD: *s1 = 0x0988; *s2 = 0x3B24; break;   /* seen1 / seen2          */
    case PK_FRLG:    *s1 = 0x05F8; *s2 = 0x3A18; break;   /* seen1 / seen2          */
    default:         *s1 = 0x0938; *s2 = 0x3A8C; break;   /* RS: dexSeen2 / dexSeen3 */
  }
}

void pk_dex_set_owned(uint8_t* sb2, uint16_t nat, bool on) {
  if (nat < 1 || nat > G3_DEX_NAT_MAX) return;
  int i = nat - 1, byte = i >> 3; uint8_t m = (uint8_t)(1u << (i & 7));
  if (on) sb2[DEX_OWNED + byte] |= m; else sb2[DEX_OWNED + byte] &= (uint8_t)~m;
}

void pk_dex_set_seen(uint8_t* sb1, uint8_t* sb2, PkGame g, uint16_t nat, bool on) {
  if (nat < 1 || nat > G3_DEX_NAT_MAX) return;
  int i = nat - 1, byte = i >> 3; uint8_t m = (uint8_t)(1u << (i & 7));
  int s1, s2; dex_sb1_seen(g, &s1, &s2);
  if (on) { sb2[DEX_SEENA + byte] |= m;          sb1[s1 + byte] |= m;          sb1[s2 + byte] |= m; }
  else    { sb2[DEX_SEENA + byte] &= (uint8_t)~m; sb1[s1 + byte] &= (uint8_t)~m; sb1[s2 + byte] &= (uint8_t)~m; }
}

bool pk_dex_seen(const uint8_t* sb2, uint16_t nat) {
  if (nat < 1 || nat > G3_DEX_NAT_MAX) return false;
  int i = nat - 1; return (sb2[DEX_SEENA + (i >> 3)] >> (i & 7)) & 1;
}
bool pk_dex_owned(const uint8_t* sb2, uint16_t nat) {
  if (nat < 1 || nat > G3_DEX_NAT_MAX) return false;
  int i = nat - 1; return (sb2[DEX_OWNED + (i >> 3)] >> (i & 7)) & 1;
}

int pk_dex_count(const uint8_t* sb2, bool owned) {
  int n = 0, base = owned ? DEX_OWNED : DEX_SEENA;
  for (int nat = 1; nat <= G3_DEX_NAT_MAX; nat++) {
    int i = nat - 1;
    if ((sb2[base + (i >> 3)] >> (i & 7)) & 1) n++;
  }
  return n;
}

#include "gen3_dex.h"
#include "gen3_flags.h"   /* pk_flag_get / pk_flag_set for FLAG_SYS_NATIONAL_DEX */

/* Within SaveBlock2: pokedex struct @ 0x18; owned[] @ +0x10, seen[] @ +0x44. */
#define DEX_POKEDEX 0x18
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

/* --- National Dex unlock (the three values EnableNationalPokedex sets, per game) --- */
typedef struct { int magic_off; uint8_t magic; int var_off; uint16_t var_val; int flag; } NatlParams;
static NatlParams natl_params(PkGame g) {
  NatlParams p;
  /* magic byte inside the pokedex struct: RS/E pokedex+0x02 (0xDA); FRLG pokedex+0x03 (0xB9) */
  p.magic_off = DEX_POKEDEX + (g == PK_FRLG ? 0x03 : 0x02);
  p.magic     = (g == PK_FRLG) ? 0xB9 : 0xDA;
  /* VAR_NATIONAL_DEX lives in SB1.vars[] (base per game) at index (varnum - 0x4000) */
  int vars_base = (g == PK_FRLG) ? 0x1000 : (g == PK_EMERALD) ? 0x139C : 0x1340;
  int var_idx   = (g == PK_FRLG) ? 0x4E : 0x46;                 /* 0x404E (FRLG) / 0x4046 */
  p.var_off     = vars_base + var_idx * 2;
  p.var_val     = (g == PK_FRLG) ? 0x6258 : 0x302;
  p.flag        = (g == PK_EMERALD) ? 0x896 : (g == PK_FRLG) ? 0x840 : 0x836;
  return p;
}

bool pk_dex_national_on(const uint8_t* sb1, const uint8_t* sb2, PkGame g) {
  NatlParams p = natl_params(g);
  uint16_t v = (uint16_t)(sb1[p.var_off] | (sb1[p.var_off + 1] << 8));
  return sb2[p.magic_off] == p.magic && v == p.var_val && pk_flag_get(sb1, g, p.flag);
}

void pk_dex_set_national(uint8_t* sb1, uint8_t* sb2, PkGame g, bool on) {
  NatlParams p = natl_params(g);
  if (on) {
    /* BACKLOG #370: the Emerald dex VIEW (+0x01) / ORDER (+0x00) bytes are written ONLY on the
     * locked -> unlocked transition. A Catch ALL on a save whose National Dex is ALREADY on used to
     * flip a Hoenn-view dex to the National list (and zero its sort order) without National changing. */
    bool was_on = pk_dex_national_on(sb1, sb2, g);
    sb2[p.magic_off] = p.magic;
    sb1[p.var_off]     = (uint8_t)(p.var_val & 0xFF);
    sb1[p.var_off + 1] = (uint8_t)(p.var_val >> 8);
    pk_flag_set(sb1, g, p.flag, true);
    if (g == PK_EMERALD && !was_on) { sb2[DEX_POKEDEX + 0x01] = 1; sb2[DEX_POKEDEX + 0x00] = 0; }  /* DEX_MODE_NATIONAL, order 0 */
  } else {
    sb2[p.magic_off] = 0;
    sb1[p.var_off] = 0; sb1[p.var_off + 1] = 0;
    pk_flag_set(sb1, g, p.flag, false);
    /* BACKLOG #370: a National VIEW with National locked is a state the game cannot reach (the Catch ALL
     * Undo and the Natl Dex: OFF toggle both land here): put the view back to Hoenn (0). */
    if (g == PK_EMERALD) sb2[DEX_POKEDEX + 0x01] = 0;
  }
}

/* BACKLOG #380: see gen3_dex.h. Same offset pk_dex_set_national zeroes (DEX_POKEDEX + 0x00). */
uint8_t pk_dex_order_get(const uint8_t* sb2, PkGame g) {
  return (sb2 && g == PK_EMERALD) ? sb2[DEX_POKEDEX + 0x00] : 0;
}
void pk_dex_order_set(uint8_t* sb2, PkGame g, uint8_t v) {
  if (sb2 && g == PK_EMERALD) sb2[DEX_POKEDEX + 0x00] = v;
}

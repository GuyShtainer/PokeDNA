#include "gen3_trainer.h"
#include "gen3_edit.h"     /* gen3_encode_char (Gen-3 string encoder) */

static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* SaveBlock1 offsets */
static uint16_t money_off(PkGame g) { return g == PK_FRLG ? 0x0290 : 0x0490; }       /* RS=E=0x490 */
static uint16_t stats_off(PkGame g) { return g == PK_FRLG ? 0x1200 : g == PK_EMERALD ? 0x159C : 0x1540; }
/* SaveBlock2 security key offset (0 => plaintext, i.e. Ruby/Sapphire) */
static uint16_t key_off(PkGame g)   { return g == PK_FRLG ? 0x0F20 : g == PK_EMERALD ? 0x00AC : 0; }

static uint32_t sec_key(const uint8_t* sb2, PkGame g) {
  uint16_t ko = key_off(g);
  return ko ? rd32(sb2 + ko) : 0;
}

uint32_t pk_money(const uint8_t* sb1, const uint8_t* sb2, PkGame g) {
  return rd32(sb1 + money_off(g)) ^ sec_key(sb2, g);
}

uint32_t pk_game_stat(const uint8_t* sb1, const uint8_t* sb2, PkGame g, int stat) {
  return rd32(sb1 + stats_off(g) + (uint32_t)stat * 4) ^ sec_key(sb2, g);
}

static void wr32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int pk_game_stat_count(PkGame g) { return g == PK_RS ? 50 : 64; }

void pk_set_game_stat(uint8_t* sb1, const uint8_t* sb2, PkGame g, int stat, uint32_t value) {
  if (stat < 0 || stat >= pk_game_stat_count(g)) return;
  wr32(sb1 + stats_off(g) + (uint32_t)stat * 4, value ^ sec_key(sb2, g));
}

void pk_set_money(uint8_t* sb1, const uint8_t* sb2, PkGame g, uint32_t money) {
  if (money > 999999) money = 999999;
  wr32(sb1 + money_off(g), money ^ sec_key(sb2, g));
}

/* ---- SaveBlock2 trainer identity setters (plaintext; not key-encrypted) ---- */
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

void pk_set_trainer_name(uint8_t* sb2, const char* s) {
  int i = 0;
  for (; i < 7 && s[i]; i++) sb2[i] = gen3_encode_char(s[i]);
  for (; i < 8; i++) sb2[i] = 0xFF;          /* 0xFF = EOS + pad (Gen-3 name field) */
}
void pk_set_gender(uint8_t* sb2, uint8_t g) { sb2[0x08] = g ? 1 : 0; }
void pk_set_trainer_id(uint8_t* sb2, uint16_t tid, uint16_t sid) {
  wr16(sb2 + 0x0A, tid); wr16(sb2 + 0x0C, sid);   /* TID lo16, SID hi16 */
}
void pk_set_playtime(uint8_t* sb2, uint16_t h, uint8_t m, uint8_t s) {
  if (h > 999) h = 999; if (m > 59) m = 59; if (s > 59) s = 59;
  wr16(sb2 + 0x0E, h); sb2[0x10] = m; sb2[0x11] = s;
}

bool pk_hof_time(const uint8_t* sb1, const uint8_t* sb2, PkGame g,
                 uint16_t* h, uint8_t* m, uint8_t* s) {
  if (pk_game_stat(sb1, sb2, g, PK_STAT_ENTERED_HOF) == 0) return false;
  uint32_t pt = pk_game_stat(sb1, sb2, g, PK_STAT_FIRST_HOF_PLAY_TIME);
  if (h) *h = (uint16_t)(pt >> 16);
  if (m) *m = (uint8_t)(pt >> 8);
  if (s) *s = (uint8_t)pt;
  return true;
}

void pk_pokedex(const uint8_t* sb2, int* seen, int* caught, bool* national) {
  const uint8_t* dex   = sb2 + 0x18;       /* struct Pokedex */
  const uint8_t* owned = dex + 0x10;
  const uint8_t* seenA = dex + 0x44;
  if (national) *national = (dex[0x03] == 0xB9);
  int sc = 0, cc = 0;
  for (int i = 0; i < 386; i++) {           /* national #1..386 -> bit i */
    if (owned[i >> 3] & (1u << (i & 7))) cc++;
    if (seenA[i >> 3] & (1u << (i & 7))) sc++;
  }
  if (seen) *seen = sc;
  if (caught) *caught = cc;
}

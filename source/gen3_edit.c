#include "gen3_edit.h"
#include "gen3_save.h"     /* gen3_decode_char (for symmetry doc), sizes */
#include "data_tables.h"   /* base stats, nature mods, growth, exp, move PP */
#include <string.h>

/* --- little-endian helpers --- */
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* substruct slot order by personality % 24 (same table as gen3_mon.c). */
static const uint8_t k_substruct_pos[24][3] = {
  {0,1,2},{0,1,3},{0,2,1},{0,3,1},{0,2,3},{0,3,2},
  {1,0,2},{1,0,3},{2,0,1},{3,0,1},{2,0,3},{3,0,2},
  {1,2,0},{1,3,0},{2,1,0},{3,1,0},{2,3,0},{3,2,0},
  {1,2,3},{1,3,2},{2,1,3},{3,1,2},{2,3,1},{3,2,1},
};

uint8_t gen3_encode_char(char c) {
  if (c == ' ') return 0x00;
  if (c >= '0' && c <= '9') return (uint8_t)(0xA1 + (c - '0'));
  if (c >= 'A' && c <= 'Z') return (uint8_t)(0xBB + (c - 'A'));
  if (c >= 'a' && c <= 'z') return (uint8_t)(0xD5 + (c - 'a'));
  switch (c) {
    case '!':  return 0xAB;
    case '?':  return 0xAC;
    case '.':  return 0xAD;
    case '-':  return 0xAE;
    case '\'': return 0xB3;
    case ',':  return 0xBA;
    default:   return 0x00;   /* unknown -> space */
  }
}

void gen3_edit_load(const uint8_t* rec, bool is_party, EditMon* e) {
  memset(e, 0, sizeof(*e));
  e->is_party = is_party;
  memcpy(e->raw, rec, is_party ? 100 : 80);
  e->personality = rd32(rec + 0x00);
  e->otId = rd32(rec + 0x04);
  uint32_t key = e->personality ^ e->otId;

  uint8_t dec[48];
  for (int w = 0; w < 12; w++) wr32(dec + w * 4, rd32(rec + 0x20 + (uint32_t)w * 4) ^ key);

  const uint8_t* pos = k_substruct_pos[e->personality % 24];
  int g = pos[0], a = pos[1], ev = pos[2], m = 6 - (g + a + ev);
  memcpy(e->sub[0], dec + (uint32_t)g  * 12, 12);  /* Growth  */
  memcpy(e->sub[1], dec + (uint32_t)a  * 12, 12);  /* Attacks */
  memcpy(e->sub[2], dec + (uint32_t)ev * 12, 12);  /* EVs     */
  memcpy(e->sub[3], dec + (uint32_t)m  * 12, 12);  /* Misc    */
}

void gen3_edit_commit(const EditMon* e, uint8_t* out) {
  int n = e->is_party ? 100 : 80;
  memcpy(out, e->raw, n);                       /* preserve ALL plaintext (incl. stored stats/padding) */

  uint32_t pers = e->personality, key = pers ^ e->otId;
  wr32(out + 0x00, pers);
  wr32(out + 0x04, e->otId);

  uint8_t dec[48];
  const uint8_t* pos = k_substruct_pos[pers % 24];
  int g = pos[0], a = pos[1], ev = pos[2], m = 6 - (g + a + ev);
  memcpy(dec + (uint32_t)g  * 12, e->sub[0], 12);
  memcpy(dec + (uint32_t)a  * 12, e->sub[1], 12);
  memcpy(dec + (uint32_t)ev * 12, e->sub[2], 12);
  memcpy(dec + (uint32_t)m  * 12, e->sub[3], 12);

  uint16_t sum = 0;                             /* per-mon checksum: sum of the 24 u16 (order-invariant) */
  for (int h = 0; h < 24; h++) sum = (uint16_t)(sum + rd16(dec + h * 2));
  wr16(out + 0x1C, sum);

  for (int w = 0; w < 12; w++) wr32(out + 0x20 + (uint32_t)w * 4, rd32(dec + w * 4) ^ key);
}

/* ---- field mutators ---- */

/* Recompute the party plaintext stats from the current IVs/EVs/level/nature/species.
 * Called only after a stat-affecting edit, so a true no-op stays byte-identical. */
static void recompute_party_stats(EditMon* e) {
  if (!e->is_party) return;
  uint16_t species = rd16(e->sub[0] + 0);
  uint8_t base[6];
  pk_base_stats(species, base);
  uint32_t iv = rd32(e->sub[3] + 4);
  uint8_t ivs[6] = {
    (uint8_t)(iv & 0x1F), (uint8_t)((iv >> 5) & 0x1F), (uint8_t)((iv >> 10) & 0x1F),
    (uint8_t)((iv >> 15) & 0x1F), (uint8_t)((iv >> 20) & 0x1F), (uint8_t)((iv >> 25) & 0x1F),
  };
  const uint8_t* ev = e->sub[2];
  uint8_t level = e->raw[0x54];
  uint8_t nat = (uint8_t)(e->personality % 25);
  int nb = pk_nature_boost(nat), nh = pk_nature_hinder(nat);
  uint16_t hp = pk_calc_hp(base[PK_HP], ivs[PK_HP], ev[PK_HP], level);
  wr16(e->raw + 0x58, hp);
  wr16(e->raw + 0x56, hp);                      /* current HP = max */
  for (int s = PK_ATK; s <= PK_SPD; s++) {
    int mod = (s == nb) ? 1 : (s == nh) ? -1 : 0;
    wr16(e->raw + 0x58 + s * 2, pk_calc_stat(base[s], ivs[s], ev[s], level, mod));
  }
}

void em_set_iv(EditMon* e, int stat, uint8_t v) {
  if (v > 31) v = 31;
  uint32_t iv = rd32(e->sub[3] + 4);
  iv &= ~(0x1Fu << (stat * 5));
  iv |= ((uint32_t)v << (stat * 5));
  wr32(e->sub[3] + 4, iv);
  recompute_party_stats(e);
}

void em_set_ev(EditMon* e, int stat, uint8_t v) {
  e->sub[2][stat] = v;
  recompute_party_stats(e);
}

void em_set_species(EditMon* e, uint16_t species) {
  wr16(e->sub[0] + 0, species);
  recompute_party_stats(e);
}

void em_set_item(EditMon* e, uint16_t item) { wr16(e->sub[0] + 2, item); }

void em_set_move(EditMon* e, int i, uint16_t move) {
  wr16(e->sub[1] + i * 2, move);
  e->sub[1][8 + i] = pk_move_pp(move);          /* reset PP to base */
  e->sub[0][8] &= ~(0x3u << (i * 2));           /* PP Ups bind to the move, not the slot:
                                                 * clear this slot's PP-Up bonus (Growth byte 8),
                                                 * mirroring the game's RemoveMonPPBonus. */
}

void em_set_pp(EditMon* e, int i, uint8_t pp) { e->sub[1][8 + i] = pp; }

void em_set_friendship(EditMon* e, uint8_t f) { e->sub[0][9] = f; }

void em_set_ability(EditMon* e, uint8_t n) {
  uint32_t iv = rd32(e->sub[3] + 4);
  iv = (iv & ~(1u << 31)) | (((uint32_t)(n & 1)) << 31);
  wr32(e->sub[3] + 4, iv);
}

/* Misc substruct (sub[3]): [1] = metLocation; [2..3] = origins u16
 * (bits 0-6 metLevel, 7-10 originGame, 11-14 pokeBall, 15 otGender). */
void em_set_metloc(EditMon* e, uint8_t loc) { e->sub[3][1] = loc; }
void em_set_ball(EditMon* e, uint8_t ball) {
  uint16_t o = rd16(e->sub[3] + 2);
  wr16(e->sub[3] + 2, (uint16_t)((o & ~(0xFu << 11)) | (((uint16_t)ball & 0xF) << 11)));
}
void em_set_metlevel(EditMon* e, uint8_t lvl) {
  uint16_t o = rd16(e->sub[3] + 2);
  wr16(e->sub[3] + 2, (uint16_t)((o & ~0x7Fu) | (lvl & 0x7F)));
}
void em_set_metgame(EditMon* e, uint8_t game) {
  uint16_t o = rd16(e->sub[3] + 2);
  wr16(e->sub[3] + 2, (uint16_t)((o & ~(0xFu << 7)) | (((uint16_t)game & 0xF) << 7)));
}

void em_set_level(EditMon* e, uint8_t level) {
  if (level < 1) level = 1;
  if (level > 100) level = 100;
  uint16_t species = rd16(e->sub[0] + 0);
  wr32(e->sub[0] + 4, pk_exp_for_level(pk_species_growth(species), level));
  if (e->is_party) {
    e->raw[0x54] = level;
    recompute_party_stats(e);
  }
}

/* Switch a loaded record between box(80) and party(100) kinds. Converting a box
 * record TO party derives the plaintext level (from exp) + battle stats; commit
 * then writes 100 bytes. Converting party->box just drops the plaintext block
 * (commit writes 80). Used by the clipboard to paste across containers. */
void em_set_party_flag(EditMon* e, bool is_party) {
  e->is_party = is_party;
  if (is_party) {
    uint16_t species = rd16(e->sub[0] + 0);
    uint32_t exp = rd32(e->sub[0] + 4);
    e->raw[0x54] = pk_level_from_exp(pk_species_growth(species), exp);
    recompute_party_stats(e);
  }
}

static void encode_name(uint8_t* dst, int cap, const char* s) {
  int i = 0;
  for (; i < cap && s[i]; i++) dst[i] = gen3_encode_char(s[i]);
  for (; i < cap; i++) dst[i] = 0xFF;            /* terminator + padding */
}

void em_set_nickname(EditMon* e, const char* s) { encode_name(e->raw + 0x08, 10, s); }
void em_set_otname(EditMon* e, const char* s)   { encode_name(e->raw + 0x14, 7, s); }

static void apply_pid(EditMon* e, uint32_t pid) {
  e->personality = pid;
  wr32(e->raw + 0x00, pid);
  recompute_party_stats(e);                      /* nature may have changed -> stats shift */
}

bool em_reroll(EditMon* e, int want_nature, int want_shiny, int want_gender, uint8_t gender_ratio) {
  uint16_t tid = (uint16_t)(e->otId & 0xFFFF), sid = (uint16_t)(e->otId >> 16);

  if (want_shiny == 1) {
    /* CONSTRUCT shiny PIDs (bounded): for each low half, the high half is forced
     * so (tid^sid^lo^hi) < 8 — far faster than brute force. */
    for (uint32_t lo = 0; lo < 0x10000u; lo++) {
      for (int k = 0; k < 8; k++) {
        uint16_t hi = (uint16_t)(tid ^ sid ^ (uint16_t)lo ^ k);
        uint32_t pid = ((uint32_t)hi << 16) | lo;
        if (want_nature >= 0 && (int)(pid % 25) != want_nature) continue;
        if (want_gender >= 0 && pk_gender_from(pid, gender_ratio) != want_gender) continue;
        apply_pid(e, pid);
        return true;
      }
    }
    return false;
  }

  /* non-shiny / don't-care: short capped sequential search (~a frame or two) */
  uint32_t pid = e->personality;
  for (uint32_t i = 0; i < 400000u; i++) {
    pid = pid * 1103515245u + 12345u;
    if (want_nature >= 0 && (int)(pid % 25) != want_nature) continue;
    if (want_shiny == 0) {
      int shiny = ((uint16_t)(tid ^ sid ^ (uint16_t)(pid & 0xFFFF) ^ (uint16_t)(pid >> 16)) < 8);
      if (shiny) continue;
    }
    if (want_gender >= 0 && pk_gender_from(pid, gender_ratio) != want_gender) continue;
    apply_pid(e, pid);
    return true;
  }
  return false;
}

/* Set the Unown letter (0..27) by finding a PID that yields it while preserving
 * the current nature + shininess (Unown is genderless, so gender is fixed). */
bool em_set_unown_form(EditMon* e, int form) {
  if (form < 0 || form > 27) return false;
  uint16_t tid = (uint16_t)(e->otId & 0xFFFF), sid = (uint16_t)(e->otId >> 16);
  int cur_nat = (int)(e->personality % 25);
  int cur_shiny = ((uint16_t)(tid ^ sid ^ (uint16_t)(e->personality & 0xFFFF) ^ (uint16_t)(e->personality >> 16)) < 8);
  if (pk_unown_form(e->personality) == form) return true;
  uint32_t pid = e->personality;
  for (uint32_t i = 0; i < 4000000u; i++) {
    pid = pid * 1103515245u + 12345u;
    if (pk_unown_form(pid) != form) continue;
    if ((int)(pid % 25) != cur_nat) continue;
    int shiny = ((uint16_t)(tid ^ sid ^ (uint16_t)(pid & 0xFFFF) ^ (uint16_t)(pid >> 16)) < 8);
    if (shiny != cur_shiny) continue;
    apply_pid(e, pid);
    return true;
  }
  return false;
}

void em_preview(const EditMon* e, PkMon* out) {
  uint8_t scratch[100];
  gen3_edit_commit(e, scratch);
  pk_decode_mon(scratch, e->is_party, out);
}

bool gen3_edit_roundtrip_ok(const uint8_t* rec, bool is_party) {
  EditMon e;
  uint8_t out[100];
  gen3_edit_load(rec, is_party, &e);
  gen3_edit_commit(&e, out);
  return memcmp(rec, out, is_party ? 100 : 80) == 0;
}

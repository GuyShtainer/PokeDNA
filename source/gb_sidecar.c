#include "gb_sidecar.h"
#include "gen3_edit.h"     /* EditMon, gen3_edit_load/commit, em_set_*             */
#include "gen3_mon.h"      /* PkMon, pk_decode_mon, PK_* stat order                */
#include "gen12_convert.h" /* gen12_iv_from_dv -- the exact inverse of DV = IV/2   */
#include <string.h>

/* ---- little-endian codec, CRC-16/CCITT-FALSE -------------------------------- */

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* poly 0x1021, init 0xFFFF, no reflection, no xorout -- CRC-16/CCITT-FALSE. The
 * standard check value, crc16(("123456789"), 9) == 0x29B1, is asserted in the test. */
static uint16_t crc16(const uint8_t* data, uint32_t len) {
  uint16_t crc = 0xFFFFu;
  for (uint32_t i = 0; i < len; i++) {
    crc = (uint16_t)(crc ^ ((uint16_t)data[i] << 8));
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
  }
  return crc;
}

/* ---- the fingerprint --------------------------------------------------------- */

uint64_t gbsc_key(uint8_t gen, uint16_t otid16, const uint8_t dv4[4],
                  const uint8_t otname[GB_NAME_BYTES]) {
  uint8_t buf[1 + 2 + 4 + GB_NAME_BYTES];
  int p = 0;
  buf[p++] = gen;
  buf[p++] = (uint8_t)otid16;
  buf[p++] = (uint8_t)(otid16 >> 8);
  for (int i = 0; i < 4; i++) buf[p++] = dv4[i];
  for (int i = 0; i < GB_NAME_BYTES; i++) buf[p++] = otname[i];

  uint64_t h = 14695981039346656037ULL;   /* FNV-1a-64 offset basis */
  for (int i = 0; i < p; i++) {
    h ^= (uint64_t)buf[i];
    h *= 1099511628211ULL;                /* FNV-1a-64 prime */
  }
  return h;
}

void gbsc_key_hex(uint64_t key, char out[17]) {
  static const char digits[] = "0123456789ABCDEF";
  if (!out) return;
  for (int i = 0; i < 16; i++) out[i] = digits[(key >> ((15 - i) * 4)) & 0xFu];
  out[16] = 0;
}

/* ---- one entry, decoded ------------------------------------------------------ */

void gbsc_entry_from(GbscEntry* e, const GbEditMon* written, const uint8_t* original80,
                     uint32_t rtc_epoch) {
  if (!e) return;
  memset(e, 0, sizeof *e);
  if (!written || !original80) return;

  e->gen             = written->gen;
  e->claimed         = 0;
  e->species_written = gb_get_species_dex(written);
  e->otid16          = gb_get_otid(written);
  e->dv4[0] = gb_get_dv(written, GB_ATK);
  e->dv4[1] = gb_get_dv(written, GB_DEF);
  e->dv4[2] = gb_get_dv(written, GB_SPE);
  e->dv4[3] = gb_get_dv(written, GB_SPC);
  memcpy(e->otname_written, written->otname, GB_NAME_BYTES);
  memcpy(e->nick_written,   written->nick,   GB_NAME_BYTES);
  e->exp_written = gb_get_exp(written);
  e->rtc_epoch   = rtc_epoch;
  memcpy(e->original80, original80, 80);
}

/* ---- the file ----------------------------------------------------------------
 * Entry byte layout (GBSC_ENTRY == 128), see gb_sidecar.h for the field table. */
enum {
  E_GEN = 0, E_CLAIMED = 1, E_SPECIES = 2, E_OTID = 4, E_DV4 = 6,
  E_OTNAME = 10, E_NICK = 21, E_EXP = 32, E_RTC = 36, E_ORIG80 = 40, E_CRC = 126
};

int gbsc_init(uint8_t* buf, uint64_t key) {
  if (!buf) return -1;
  memset(buf, 0, GBSC_HEADER);
  memcpy(buf, "PDS1", 4);
  buf[4] = 1;    /* version */
  buf[5] = 0;    /* count   */
  /* bytes 6-7: flags, reserved 0 */
  for (int i = 0; i < 8; i++) buf[8 + i] = (uint8_t)(key >> (8 * i));
  wr16(buf + 16, crc16(buf, 16));
  return GBSC_HEADER;
}

int gbsc_count(const uint8_t* buf, uint32_t len) {
  if (!buf || len < GBSC_HEADER) return -1;
  if (memcmp(buf, "PDS1", 4) != 0) return -1;
  if (buf[4] != 1) return -1;                        /* version */
  uint8_t count = buf[5];
  if (count > GBSC_MAX_ENTRIES) return -1;
  if (len != (uint32_t)GBSC_HEADER + (uint32_t)count * GBSC_ENTRY) return -1;
  if (rd16(buf + 16) != crc16(buf, 16)) return -1;    /* header crc16 */

  for (uint8_t i = 0; i < count; i++) {
    const uint8_t* e = buf + GBSC_HEADER + (uint32_t)i * GBSC_ENTRY;
    if (rd16(e + E_CRC) != crc16(e, GBSC_ENTRY - 2)) return -1;
  }
  return (int)count;
}

int gbsc_add(uint8_t* buf, uint32_t* len, uint32_t cap, const GbscEntry* e) {
  if (!buf || !len || !e) return -1;
  int count = gbsc_count(buf, *len);
  if (count < 0 || count >= GBSC_MAX_ENTRIES) return -1;
  uint32_t new_len = (uint32_t)GBSC_HEADER + (uint32_t)(count + 1) * GBSC_ENTRY;
  if (new_len > cap) return -1;

  uint8_t* dst = buf + GBSC_HEADER + (uint32_t)count * GBSC_ENTRY;
  memset(dst, 0, GBSC_ENTRY);
  dst[E_GEN] = e->gen;
  dst[E_CLAIMED] = e->claimed;
  wr16(dst + E_SPECIES, e->species_written);
  wr16(dst + E_OTID, e->otid16);
  memcpy(dst + E_DV4, e->dv4, 4);
  memcpy(dst + E_OTNAME, e->otname_written, GB_NAME_BYTES);
  memcpy(dst + E_NICK, e->nick_written, GB_NAME_BYTES);
  wr32(dst + E_EXP, e->exp_written);
  wr32(dst + E_RTC, e->rtc_epoch);
  memcpy(dst + E_ORIG80, e->original80, 80);
  /* bytes [120..125]: pad, already zero */
  wr16(dst + E_CRC, crc16(dst, GBSC_ENTRY - 2));

  buf[5] = (uint8_t)(count + 1);
  wr16(buf + 16, crc16(buf, 16));
  *len = new_len;
  return count;
}

bool gbsc_get(const uint8_t* buf, uint32_t len, int idx, GbscEntry* out) {
  int count = gbsc_count(buf, len);
  if (count < 0 || !out || idx < 0 || idx >= count) return false;
  const uint8_t* e = buf + GBSC_HEADER + (uint32_t)idx * GBSC_ENTRY;
  out->gen             = e[E_GEN];
  out->claimed         = e[E_CLAIMED];
  out->species_written = rd16(e + E_SPECIES);
  out->otid16          = rd16(e + E_OTID);
  memcpy(out->dv4, e + E_DV4, 4);
  memcpy(out->otname_written, e + E_OTNAME, GB_NAME_BYTES);
  memcpy(out->nick_written,   e + E_NICK,   GB_NAME_BYTES);
  out->exp_written = rd32(e + E_EXP);
  out->rtc_epoch   = rd32(e + E_RTC);
  memcpy(out->original80, e + E_ORIG80, 80);
  return true;
}

int gbsc_find(const uint8_t* buf, uint32_t len, const GbEditMon* now, int start) {
  int count = gbsc_count(buf, len);
  if (count < 0 || !now || start < 0) return -1;
  uint8_t dv4[4] = {
    gb_get_dv(now, GB_ATK), gb_get_dv(now, GB_DEF),
    gb_get_dv(now, GB_SPE), gb_get_dv(now, GB_SPC)
  };
  uint16_t otid = gb_get_otid(now);
  for (int i = start; i < count; i++) {
    const uint8_t* e = buf + GBSC_HEADER + (uint32_t)i * GBSC_ENTRY;
    if (e[E_GEN] != now->gen) continue;
    if (rd16(e + E_OTID) != otid) continue;
    if (memcmp(e + E_DV4, dv4, 4) != 0) continue;
    if (memcmp(e + E_OTNAME, now->otname, GB_NAME_BYTES) != 0) continue;
    return i;                 /* species deliberately not compared -- see gb_sidecar.h */
  }
  return -1;
}

int gbsc_remove(uint8_t* buf, uint32_t* len, int idx) {
  if (!buf || !len) return -1;
  int count = gbsc_count(buf, *len);
  if (count < 0 || idx < 0 || idx >= count) return -1;

  uint8_t* dst = buf + GBSC_HEADER + (uint32_t)idx * GBSC_ENTRY;
  uint32_t tail_bytes = (uint32_t)(count - idx - 1) * GBSC_ENTRY;
  if (tail_bytes) memmove(dst, dst + GBSC_ENTRY, tail_bytes);
  memset(buf + GBSC_HEADER + (uint32_t)(count - 1) * GBSC_ENTRY, 0, GBSC_ENTRY);

  buf[5] = (uint8_t)(count - 1);
  wr16(buf + 16, crc16(buf, 16));
  *len = (uint32_t)GBSC_HEADER + (uint32_t)(count - 1) * GBSC_ENTRY;
  return 0;
}

/* ---- the merge UP ------------------------------------------------------------
 * docs/GEN3-TO-GB-SIDECAR-DESIGN.md section 4. Species mapping: National Dex and the
 * Gen-3 INTERNAL index are the same number for 1..251 (gen12_convert.c:304-307), and
 * every species a Game Boy record can hold is in that range, so em_set_species() takes
 * gb_get_species_dex()'s result directly -- no lookup table on this side either. */
bool gbsc_merge_up(const GbscEntry* e, const GbEditMon* now, uint8_t out80[80],
                   GbscMergeReport* rep) {
  GbscMergeReport local;
  if (!rep) rep = &local;
  memset(rep, 0, sizeof *rep);
  if (!e || !now || !out80) return false;
  if (e->gen != now->gen) return false;

  EditMon em;
  gen3_edit_load(e->original80, false, &em);

  uint16_t dex_now = gb_get_species_dex(now);
  if (dex_now != 0 && dex_now != e->species_written) {
    em_set_species(&em, dex_now);
    rep->evolved = true;
  }

  if (gb_get_exp(now) != e->exp_written) {
    em_set_level(&em, gb_get_level(now));
    rep->level_changed = true;
  }

  /* Move ids agree 1:1 across Gen 1/2/3 for every id a Game Boy can hold
   * (gb_editor.h: "Gen-1/2 move ids ARE the Gen-3 ids for 1..251"), so the ORIGINAL
   * record's own moves -- decoded straight out of `original80` -- are exactly what
   * the Game Boy record held right after gen3_to_gb ran. No separate "moves_written"
   * field is needed in the entry. */
  PkMon orig;
  bool moves_differ = false;
  if (pk_decode_mon(e->original80, false, &orig)) {
    for (int i = 0; i < 4; i++) {
      uint8_t orig_mv = (orig.moves[i] > 255u) ? 0 : (uint8_t)orig.moves[i];
      uint8_t orig_up = (uint8_t)((orig.ppBonuses >> (i * 2)) & 0x3u);
      if (gb_get_move(now, i) != orig_mv || gb_get_ppup(now, i) != orig_up) {
        moves_differ = true;
        break;
      }
    }
  }
  if (moves_differ) {
    /* Moves first: em_set_move resets PP/PP-Ups for the slot, so setting PP Ups (and
     * then current PP) has to follow it, not precede it. */
    for (int i = 0; i < 4; i++) {
      em_set_move(&em, i, gb_get_move(now, i));
      em_set_ppups(&em, i, gb_get_ppup(now, i));
      em_set_pp(&em, i, gb_get_pp(now, i));
    }
    rep->moves_changed = true;
  }

  if (memcmp(now->nick, e->nick_written, GB_NAME_BYTES) != 0) {
    char text[GB_TEXT_MAX];
    int n = gb_name_decode(now->gen, text, (int)sizeof text, now->nick, GB_NAME_BYTES);
    bool has_escape = false;
    for (int i = 0; i < n; i++) if (text[i] == '{') { has_escape = true; break; }
    if (has_escape) {
      /* The Gen-3 charset has no way to spell a "{XX}" escape (gb_edit.h NAMES):
       * keep the sidecar's own nickname rather than write a name the player never
       * typed on either side. */
      rep->rename_refused = true;
    } else {
      em_set_nickname(&em, text);
      rep->renamed = true;
    }
  }

  /* DVs: GB_SPC maps to SpA only, never SpD (docs/GEN3-TO-GB-SIDECAR-DESIGN.md
   * section 4's table is explicit that SpD is not touched here). */
  static const int gb_stat[4] = { GB_ATK, GB_DEF, GB_SPE, GB_SPC };
  static const int pk_stat[4] = { PK_ATK, PK_DEF, PK_SPE, PK_SPA };
  for (int i = 0; i < 4; i++) {
    uint8_t dv_now = gb_get_dv(now, gb_stat[i]);
    if (dv_now != e->dv4[i]) {
      em_set_iv(&em, pk_stat[i], gen12_iv_from_dv(dv_now));
      rep->dv_edited = true;
    }
  }

  if (now->gen == GB_GEN2) {
    em_set_friendship(&em, gb_get_friendship(now));      /* same 0..255 scale, always */
    if (gb_get_held_item(now) != 0) rep->gb_item_ignored = true;
  }

  gen3_edit_commit(&em, out80);
  return true;
}

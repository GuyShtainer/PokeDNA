#include "gb_sidecar.h"
#include "gen3_edit.h"     /* EditMon, gen3_edit_load/commit, em_set_*             */
#include "gen3_mon.h"      /* PkMon, pk_decode_mon, PK_* stat order                */
#include "gen3_box.h"      /* pk_resolve -- the ORIGINAL record's true level       */
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

/* ---- the merge UP, one stage per helper ---------------------------------------
 * docs/GEN3-TO-GB-SIDECAR-DESIGN.md section 4. Species mapping: National Dex and the
 * Gen-3 INTERNAL index are the same number for 1..251 (gen12_convert.c:304-307), and
 * every species a Game Boy record can hold is in that range, so em_set_species() takes
 * gb_get_species_dex()'s result directly -- no lookup table on this side either.
 *
 * `orig`/`have_orig`: the ORIGINAL Gen-3 record decoded once by the caller
 * (gbsc_merge_up below), with pk_resolve() already run so `orig->level` is its true
 * level even though it was stored as a box-shaped (is_party=false) record — pk_resolve
 * computes level from EXP+growth-rate for exactly that case (gen3_box.c). Every helper
 * takes `have_orig` rather than re-checking a NULL, because a failed decode is not
 * this function's problem to solve twice. */

static void merge_species_and_level(EditMon* em, const GbscEntry* e, const GbEditMon* now,
                                    const PkMon* orig, bool have_orig,
                                    GbscMergeReport* rep) {
  uint16_t dex_now = gb_get_species_dex(now);
  if (dex_now != 0 && dex_now != e->species_written) {
    em_set_species(em, dex_now);
    rep->evolved = true;
  }

  /* Gated on LEVEL, not EXP: Gen 2 shows a player only the level, and em_set_level()
   * writes the EXP FLOOR for that level under the (possibly new) species' growth
   * rate. Gating on "does EXP differ from exp_written" instead — an earlier revision
   * of this function did exactly that — meant ANY exp gain on the Game Boy without a
   * level-up (which happens on every single battle) called em_set_level() and reset
   * the Gen-3 mon's careful within-level EXP progress down to that level's floor:
   * measured on the real corpus, 101 of 314 accepted Gen-2 conversions would have
   * lost EXP this way, the worst by 14,454 points. There is no way to reflect a
   * Game-Boy-side EXP gain that has not produced a level-up: Gen 2 does not expose
   * it, and there is nothing else to write. So a same-level EXP change on the Game
   * Boy is simply not applied — the Gen-3 side keeps its own precise EXP — and only
   * an actual level change (the one thing the player can see and this merge can
   * therefore know really happened) moves it. */
  if (have_orig && gb_get_level(now) != orig->level) {
    em_set_level(em, gb_get_level(now));
    rep->level_changed = true;
  }
}

/* Move ids agree 1:1 across Gen 1/2/3 for every id a Game Boy can hold (gb_editor.h:
 * "Gen-1/2 move ids ARE the Gen-3 ids for 1..251"), so the ORIGINAL record's own
 * moves -- decoded straight out of `original80` -- are exactly what the Game Boy
 * record held right after gen3_to_gb ran. No separate "moves_written" field is
 * needed in the entry. */
static void merge_moves(EditMon* em, const GbEditMon* now, const PkMon* orig,
                        bool have_orig, GbscMergeReport* rep) {
  if (!have_orig) return;
  bool differ = false;
  for (int i = 0; i < 4; i++) {
    uint8_t orig_mv = (orig->moves[i] > 255u) ? 0 : (uint8_t)orig->moves[i];
    uint8_t orig_up = (uint8_t)((orig->ppBonuses >> (i * 2)) & 0x3u);
    if (gb_get_move(now, i) != orig_mv || gb_get_ppup(now, i) != orig_up) {
      differ = true;
      break;
    }
  }
  if (!differ) return;

  /* Moves first: em_set_move resets PP/PP-Ups for the slot, so setting PP Ups (and
   * then current PP) has to follow it, not precede it. */
  for (int i = 0; i < 4; i++) {
    em_set_move(em, i, gb_get_move(now, i));
    em_set_ppups(em, i, gb_get_ppup(now, i));
    em_set_pp(em, i, gb_get_pp(now, i));
  }
  rep->moves_changed = true;
}

/* The Gen-3 charset can only spell ASCII (gen3_edit.h's gen3_encode_char: "unknown ->
 * space"), so a decoded Game Boy name is refused here for EITHER of two reasons: it
 * needed a "{XX}" escape (a byte with no text spelling at all -- gb_edit.h NAMES), or
 * it decoded to a real non-ASCII glyph the Game Boy charset CAN spell but Gen 3's
 * encoder cannot (the gender signs 0xEF/0xF5, e/x/umlauts...). Both keep the
 * sidecar's own nickname untouched rather than silently substitute a space for a
 * character the player actually typed. */
static void merge_nickname(EditMon* em, const GbscEntry* e, const GbEditMon* now,
                           GbscMergeReport* rep) {
  if (memcmp(now->nick, e->nick_written, GB_NAME_BYTES) == 0) return;

  char text[GB_TEXT_MAX];
  int n = gb_name_decode(now->gen, text, (int)sizeof text, now->nick, GB_NAME_BYTES);
  bool unrepresentable = false;
  for (int i = 0; i < n; i++) {
    unsigned char c = (unsigned char)text[i];
    if (c == '{' || c >= 0x80u) { unrepresentable = true; break; }
  }
  if (unrepresentable) {
    rep->rename_refused = true;
  } else {
    em_set_nickname(em, text);
    rep->renamed = true;
  }
}

/* Rebuild the Gen-3 record: start from `e->original80` (so PID, nature, ability,
 * shininess, gender, met data, ball, ribbons, contest, markings, secret ID and EVs
 * all come back exactly), then fold in what changed on the Game Boy between the
 * conversion and now. See gb_sidecar.h for the full contract. */
bool gbsc_merge_up(const GbscEntry* e, const GbEditMon* now, uint8_t out80[80],
                   GbscMergeReport* rep) {
  GbscMergeReport local;
  if (!rep) rep = &local;
  memset(rep, 0, sizeof *rep);
  if (!e || !now || !out80) return false;
  if (e->gen != now->gen) return false;

  EditMon em;
  gen3_edit_load(e->original80, false, &em);

  PkMon orig;
  bool have_orig = pk_decode_mon(e->original80, false, &orig);
  if (have_orig) pk_resolve(&orig);

  merge_species_and_level(&em, e, now, &orig, have_orig, rep);
  merge_moves(&em, now, &orig, have_orig, rep);
  merge_nickname(&em, e, now, rep);

  /* DVs are deliberately NOT re-checked here. dv4 is part of the sidecar's own
   * fingerprint (gbsc_key/gbsc_find), so a DV edit on the Game Boy already changes
   * what gbsc_find() looks for -- by the time a caller reaches this function through
   * the supported find-then-merge path, `now`'s DVs and `e->dv4` cannot disagree. A
   * caller that bypasses gbsc_find and hands over a mismatched entry gets the
   * sidecar's ORIGINAL IVs, unedited: there is no "which side wins" question to
   * answer, because in the supported flow the question cannot arise. See
   * gb_sidecar.h's GbscMergeReport comment and design doc section 4: a DV edit on
   * the Game Boy orphans that Pokemon's sidecar on purpose (S5-B's UI must warn
   * before letting a DV edit happen on a mon that still has one). */

  if (now->gen == GB_GEN2) {
    em_set_friendship(&em, gb_get_friendship(now));      /* same 0..255 scale, always */
    if (gb_get_held_item(now) != 0) rep->gb_item_ignored = true;
  }

  gen3_edit_commit(&em, out80);
  return true;
}

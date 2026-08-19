#include "gen3_save.h"
#include <string.h>

/* --- little-endian readers (saves are LE; GBA is LE too, but be explicit) --- */
static uint16_t rd16(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)(v >> 8);
}

/* Exactly mirrors pokeemerald CalculateChecksum:
 *   sum u32 words over (size/4), then return (u16)((sum >> 16) + sum). */
uint16_t gen3_checksum(const void* data, uint16_t size) {
  const uint8_t* p = (const uint8_t*)data;
  uint32_t checksum = 0;
  uint16_t words = (uint16_t)(size / 4);
  for (uint16_t i = 0; i < words; i++) {
    checksum += rd32(p);
    p += 4;
  }
  return (uint16_t)((checksum >> 16) + checksum);
}

char gen3_decode_char(uint8_t c) {
  if (c == 0xFF) return 0;    /* terminator */
  if (c == 0x00) return ' ';
  if (c >= 0xA1 && c <= 0xAA) return (char)('0' + (c - 0xA1));
  if (c >= 0xBB && c <= 0xD4) return (char)('A' + (c - 0xBB));
  if (c >= 0xD5 && c <= 0xEE) return (char)('a' + (c - 0xD5));
  switch (c) {
    case 0xAB: return '!';
    case 0xAC: return '?';
    case 0xAD: return '.';
    case 0xAE: return '-';
    case 0xB3: return '\'';
    case 0xB4: return '\'';
    /* 0xB8 = ',' and 0xBA = '/' in the real Gen-3 charmap (pokeemerald charmap.txt;
     * also documented independently in source/rom_text.c's own charmap() table, which
     * flagged this exact swap in a comment without fixing it here). This table had it
     * backwards -- 0xBA decoded to ',' and 0xB8 had no case at all, so a genuine comma
     * (0xB8) silently became '?' while a genuine slash (0xBA) silently became ','.
     * Guy's own Lugia, nicknamed "28/01/2026", rendered as "28,01,2026" from exactly
     * this. See tests/host_charmap_test.c for the round-trip regression test. */
    case 0xB8: return ',';
    case 0xBA: return '/';
    default:   return '?';
  }
}

int gen3_find_section(const uint8_t* save, int slot, int section_id) {
  uint32_t base = (uint32_t)slot * G3_SLOT_BYTES;
  for (int s = 0; s < G3_SECTORS_PER_SLOT; s++) {
    const uint8_t* sec = save + base + (uint32_t)s * G3_SECTOR_SIZE;
    if (rd32(sec + G3_OFF_SIGNATURE) != G3_SIGNATURE) continue;
    if (rd16(sec + G3_OFF_ID) == (uint16_t)section_id) return s;
  }
  return -1;
}

uint32_t gen3_read_saveblock1(const uint8_t* save, int slot, uint8_t* dst) {
  uint32_t base = (uint32_t)slot * G3_SLOT_BYTES;
  uint32_t written = 0;
  for (int id = G3_SID_SAVEBLOCK1_START; id <= G3_SID_SAVEBLOCK1_END; id++) {
    int s = gen3_find_section(save, slot, id);
    if (s < 0) return 0;
    const uint8_t* sec = save + base + (uint32_t)s * G3_SECTOR_SIZE;
    memcpy(dst + written, sec, G3_SECTOR_DATA_SIZE);
    written += G3_SECTOR_DATA_SIZE;
  }
  return written; /* 15872 */
}

/* Substructure ordering for a Gen 3 Pokémon. index = personality % 24; the
 * three values are the slot (0..3) within the 48-byte secure block that holds
 * the Growth, Attacks, and EVs substructs respectively (each substruct is 12
 * bytes). Derived from the 24 GAEM permutations and validated bit-exact against
 * real Ruby + Emerald saves; a wrong row breaks the per-mon checksum we verify. */
static const uint8_t k_substruct_pos[24][3] = {
  /* GAEM */ {0, 1, 2}, /* GAME */ {0, 1, 3}, /* GEAM */ {0, 2, 1},
  /* GEMA */ {0, 3, 1}, /* GMAE */ {0, 2, 3}, /* GMEA */ {0, 3, 2},
  /* AGEM */ {1, 0, 2}, /* AGME */ {1, 0, 3}, /* AEGM */ {2, 0, 1},
  /* AEMG */ {3, 0, 1}, /* AMGE */ {2, 0, 3}, /* AMEG */ {3, 0, 2},
  /* EGAM */ {1, 2, 0}, /* EGMA */ {1, 3, 0}, /* EAGM */ {2, 1, 0},
  /* EAMG */ {3, 1, 0}, /* EMGA */ {2, 3, 0}, /* EMAG */ {3, 2, 0},
  /* MGAE */ {1, 2, 3}, /* MGEA */ {1, 3, 2}, /* MAGE */ {2, 1, 3},
  /* MAEG */ {3, 1, 2}, /* MEGA */ {2, 3, 1}, /* MEAG */ {3, 2, 1},
};

/* Decode one 100-byte party mon. Returns true if it should be KEPT (non-egg,
 * non-bad-egg, species != 0, substruct checksum OK). Decoded fields are written
 * only when kept; any out-pointer may be NULL. `nickname` (>=11 bytes) receives
 * the decoded name. */
static bool decode_party_mon(const uint8_t* mon,
                             uint16_t* species, uint16_t* heldItem, uint16_t moves[4],
                             uint8_t* level, uint8_t* avgEV, uint32_t* personality,
                             uint16_t* maxHP, uint16_t* attack, uint16_t* speed,
                             char* nickname) {
  uint8_t flags = mon[0x13];                  /* BoxPokemon flags byte */
  bool is_bad_egg = (flags & 0x01) != 0;
  bool is_egg     = (flags & 0x04) != 0;

  uint32_t pers = rd32(mon + 0x00);
  uint32_t otid = rd32(mon + 0x04);
  uint32_t key  = pers ^ otid;

  uint8_t dec[48];                            /* decrypted secure block */
  for (int w = 0; w < 12; w++) {
    uint32_t word = rd32(mon + 0x20 + (uint32_t)w * 4) ^ key;
    dec[w * 4 + 0] = (uint8_t)(word);
    dec[w * 4 + 1] = (uint8_t)(word >> 8);
    dec[w * 4 + 2] = (uint8_t)(word >> 16);
    dec[w * 4 + 3] = (uint8_t)(word >> 24);
  }

  uint16_t sum = 0;
  for (int h = 0; h < 24; h++) sum = (uint16_t)(sum + rd16(dec + h * 2));
  uint16_t stored = rd16(mon + 0x1C);

  const uint8_t* pos = k_substruct_pos[pers % 24];
  const uint8_t* g = dec + (uint32_t)pos[0] * 12;   /* Growth  */
  const uint8_t* a = dec + (uint32_t)pos[1] * 12;   /* Attacks */
  const uint8_t* e = dec + (uint32_t)pos[2] * 12;   /* EVs     */
  uint16_t sp = rd16(g + 0);

  if (is_bad_egg || is_egg) return false;
  if (sp == 0) return false;
  if (stored != sum) return false;

  if (species)     *species  = sp;
  if (heldItem)    *heldItem = rd16(g + 2);
  if (moves) { moves[0] = rd16(a + 0); moves[1] = rd16(a + 2);
               moves[2] = rd16(a + 4); moves[3] = rd16(a + 6); }
  if (level)       *level = mon[0x54];
  if (avgEV)       *avgEV = (uint8_t)((uint16_t)(e[0]+e[1]+e[2]+e[3]+e[4]+e[5]) / 6);
  if (personality) *personality = pers;
  if (maxHP)       *maxHP  = rd16(mon + 0x58);   /* plaintext battle stats */
  if (attack)      *attack = rd16(mon + 0x5A);
  if (speed)       *speed  = rd16(mon + 0x5E);
  if (nickname) {
    int k = 0;
    for (; k < 10; k++) { char ch = gen3_decode_char(mon[0x08 + k]); if (ch == 0) break; nickname[k] = ch; }
    nickname[k] = 0;
  }
  return true;
}

int gen3_read_live_party_display(const uint8_t* sb1, Gen3DisplayParty* out) {
  memset(out, 0, sizeof(*out));
  uint8_t count = sb1[SB1_OFF_PARTY_COUNT];
  if (count > G3_PARTY_SIZE) count = G3_PARTY_SIZE;

  int kept = 0;
  for (int i = 0; i < count; i++) {
    const uint8_t* mon = sb1 + SB1_OFF_PARTY + (uint32_t)i * G3_MON_SIZE;
    Gen3DisplayMon* d = &out->mon[kept];
    if (!decode_party_mon(mon, &d->species, NULL, NULL, &d->level, NULL, NULL,
                          &d->maxHP, &d->attack, &d->speed, d->nickname))
      continue;
    d->original_slot = (uint8_t)i;
    kept++;
  }
  out->count = kept;
  return kept;
}

static bool is_leap(int y) { return (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0); }

int gen3_rtc_days(int year, int month, int day) {
  static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1) month = 1;
  if (month > 12) month = 12;
  int days = 0;
  for (int y = 2000; y < year; y++) days += is_leap(y) ? 366 : 365;
  for (int m = 1; m < month; m++) { days += mdays[m - 1]; if (m == 2 && is_leap(year)) days++; }
  return days + (day - 1);
}

bool gen3_day_passed(const uint8_t* sb2, int rtc_now_days) {
  int offset_days = (int16_t)rd16(sb2 + SB2_OFF_LOCAL_TIME_OFFSET);
  int last_berry  = (int16_t)rd16(sb2 + SB2_OFF_LAST_BERRY_UPDATE);
  int current_local = rtc_now_days - offset_days;
  return current_local > last_berry;
}

/* ---- RTC clock check & fix ---------------------------------------------- */

static int days_in_month(int year, int month) {
  static const int md[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 30;
  return md[month - 1] + ((month == 2 && is_leap(year)) ? 1 : 0);
}

void gen3_days_to_date(int days, int* year, int* month, int* day) {
  int y = 2000, m = 1;
  if (days < 0) days = 0;                         /* clamp; corrupt input only */
  for (;;) { int yd = is_leap(y) ? 366 : 365; if (days < yd) break; days -= yd; y++; if (y > 2099) break; }
  for (m = 1; m <= 12; m++) { int dm = days_in_month(y, m); if (days < dm) break; days -= dm; }
  if (m > 12) m = 12;
  if (year)  *year  = y;
  if (month) *month = m;
  if (day)   *day   = days + 1;
}

void gen3_clock_read(const uint8_t* sb2, int rtc_now_days, int rtc_now_sec, int has_rtc, Gen3ClockInfo* out) {
  out->has_rtc    = has_rtc;
  out->off_days   = (int16_t)rd16(sb2 + SB2_OFF_LOCAL_TIME_OFFSET);
  out->off_h      = (int8_t)sb2[SB2_OFF_LOCAL_TIME_OFFSET + 2];
  out->off_m      = (int8_t)sb2[SB2_OFF_LOCAL_TIME_OFFSET + 3];
  out->off_s      = (int8_t)sb2[SB2_OFF_LOCAL_TIME_OFFSET + 4];
  out->berry_days = (int16_t)rd16(sb2 + SB2_OFF_LAST_BERRY_UPDATE);
  /* In-game day = liveRTC - localTimeOffset with the game's borrow: if the live
   * time-of-day is earlier than the offset's, one day is borrowed (matches
   * RtcCalcTimeDifference). Without this the derived date is off by a day. */
  out->local_days = rtc_now_days - out->off_days;            /* epoch-0 in-game day */
  { int off_sec = out->off_h * 3600 + out->off_m * 60 + out->off_s;
    int local_sec = rtc_now_sec - off_sec;
    if (local_sec < 0)       out->local_days -= 1;
    else if (local_sec >= 86400) out->local_days += 1; }
  gen3_days_to_date(out->local_days, &out->ly, &out->lm, &out->ld);
  /* berry_days is stored in the game's day convention (epoch-0 + 1); compare in epoch-0. */
  out->delta = out->local_days - (out->berry_days - 1);
  if (!has_rtc) { out->verdict = 4; return; }
  if (out->local_days < 0 || out->ly < 2000 || out->ly > 2099 ||
      out->delta > 36500 || out->delta < -36500) out->verdict = 3;   /* corrupt   */
  else if (out->delta < 0)   out->verdict = 2;                       /* backwards */
  else if (out->delta > 365) out->verdict = 1;                       /* big jump  */
  else                       out->verdict = 0;                       /* ok        */
}

/* Write localTimeOffset + lastBerryTreeUpdate (plaintext, little-endian). Padding
 * bytes +0x05..+0x07 of each struct Time are left untouched. */
static void clock_set(uint8_t* sb2, int off_days, int off_h, int off_m, int off_s,
                      int berry_days, int berry_h, int berry_m, int berry_s) {
  wr16(sb2 + SB2_OFF_LOCAL_TIME_OFFSET, (uint16_t)(int16_t)off_days);
  sb2[SB2_OFF_LOCAL_TIME_OFFSET + 2] = (uint8_t)(int8_t)off_h;
  sb2[SB2_OFF_LOCAL_TIME_OFFSET + 3] = (uint8_t)(int8_t)off_m;
  sb2[SB2_OFF_LOCAL_TIME_OFFSET + 4] = (uint8_t)(int8_t)off_s;
  wr16(sb2 + SB2_OFF_LAST_BERRY_UPDATE, (uint16_t)(int16_t)berry_days);
  sb2[SB2_OFF_LAST_BERRY_UPDATE + 2] = (uint8_t)(int8_t)berry_h;
  sb2[SB2_OFF_LAST_BERRY_UPDATE + 3] = (uint8_t)(int8_t)berry_m;
  sb2[SB2_OFF_LAST_BERRY_UPDATE + 4] = (uint8_t)(int8_t)berry_s;
}

bool gen3_clock_manual(uint8_t* sb2,
                       int ly, int lmo, int ld, int lh, int lmi, int ls,
                       int dy, int dmo, int dd, int dh, int dmi, int ds) {
  int live_days = gen3_rtc_days(ly, lmo, ld);
  int des_days  = gen3_rtc_days(dy, dmo, dd);
  /* localTimeOffset = liveRTC - desired, as a signed total split into days + h/m/s.
   * Day counts stay small (<2^16) so day*86400 would overflow int32 — keep days and
   * seconds separate to avoid that, and floor toward -inf so h/m/s land in range. */
  int off_days = live_days - des_days;
  int off_secs = (lh * 3600 + lmi * 60 + ls) - (dh * 3600 + dmi * 60 + ds);
  if (off_secs < 0) { off_secs += 86400; off_days -= 1; }
  int off_h = off_secs / 3600, off_m = (off_secs % 3600) / 60, off_s = off_secs % 60;
  int berry_days = des_days + 1;   /* game day convention = epoch-0 + 1 */
  /* The game stores Time.days as s16. Refuse rather than silently wrap if a date
   * (or a battery-dead cart's implausible year) pushes a count out of range. */
  if (off_days < -32000 || off_days > 32000 || berry_days < 0 || berry_days > 32000)
    return false;
  /* re-anchor lastBerryTreeUpdate = desired local so the next per-minute update sees
   * zero elapsed (no berry over-advance / freeze). */
  clock_set(sb2, off_days, off_h, off_m, off_s, berry_days, dh, dmi, ds);
  return true;
}

bool gen3_clock_autosync(uint8_t* sb2, int y, int mo, int d, int h, int mi, int s) {
  return gen3_clock_manual(sb2, y, mo, d, h, mi, s, y, mo, d, h, mi, s);  /* desired == live -> offset 0 */
}

uint32_t gen3_secret_base_offset(Gen3Version version) {
  switch (version) {
    case G3_VER_EMERALD: return SB1_OFF_SECRET_BASES_EMERALD;
    case G3_VER_RS:      return SB1_OFF_SECRET_BASES_RS;
    default:             return 0;
  }
}

/* A record "looks sane": either empty (id 0 and no species), or has plausible
 * species (1..MAX) and levels (1..100) for at least the first party slot. */
static bool record_sane(const uint8_t* rec) {
  /* party @ +0x34: species @ +0x7C (offset within record), levels @ +0x94 */
  uint16_t sp0 = rd16(rec + 0x7C);
  uint8_t  lv0 = rec[0x94];
  uint8_t  id  = rec[0x00];
  bool empty = (id == 0 && sp0 == 0);
  if (empty) return true;
  if (sp0 == 0 || sp0 > G3_MAX_SPECIES) return false;
  if (lv0 == 0 || lv0 > 100) return false;
  return true;
}

int gen3_count_secret_bases(const uint8_t* sb1, Gen3Version version) {
  uint32_t off = gen3_secret_base_offset(version);
  if (off == 0) return -1;
  if (off + (uint32_t)G3_SECRET_BASES_COUNT * G3_SECRET_BASE_SIZE > G3_SAVEBLOCK1_BYTES)
    return -1;
  int nonempty = 0, sane = 0;
  for (int i = 0; i < G3_SECRET_BASES_COUNT; i++) {
    const uint8_t* rec = sb1 + off + (uint32_t)i * G3_SECRET_BASE_SIZE;
    uint16_t sp0 = rd16(rec + 0x7C);
    uint8_t  id  = rec[0x00];
    bool empty = (id == 0 && sp0 == 0);
    if (record_sane(rec)) sane++;
    if (!empty) nonempty++;
  }
  /* If any record is insane, treat as "wrong offset" by returning a low score. */
  if (sane < G3_SECRET_BASES_COUNT) return 0;
  return nonempty;
}

bool gen3_section_checksum_ok(const uint8_t* save, int slot, int section_id,
                              uint16_t data_size) {
  int s = gen3_find_section(save, slot, section_id);
  if (s < 0) return false;
  uint32_t base = (uint32_t)slot * G3_SLOT_BYTES;
  const uint8_t* sec = save + base + (uint32_t)s * G3_SECTOR_SIZE;
  uint16_t stored = rd16(sec + G3_OFF_CHECKSUM);
  uint16_t calc = gen3_checksum(sec, data_size);
  return stored == calc;
}

bool gen3_verify_full_checksums(const uint8_t* save, int slot, int* fail_id) {
  if (fail_id) *fail_id = -1;
  static const int full_ids[] = {1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12};
  for (unsigned i = 0; i < sizeof(full_ids) / sizeof(full_ids[0]); i++) {
    int id = full_ids[i];
    int s = gen3_find_section(save, slot, id);
    if (s < 0) continue; /* section absent: not a checksum failure here */
    if (!gen3_section_checksum_ok(save, slot, id, G3_SECTOR_DATA_SIZE)) {
      if (fail_id) *fail_id = id;
      return false;
    }
  }
  return true;
}

void gen3_sb1_touch_sections(Gen3Version version, int* first_id, int* last_id) {
  uint32_t off = gen3_secret_base_offset(version);
  uint32_t last = off + (uint32_t)G3_SECRET_BASES_COUNT * G3_SECRET_BASE_SIZE - 1;
  *first_id = G3_SID_SAVEBLOCK1_START + (int)(off / G3_SECTOR_DATA_SIZE);
  *last_id  = G3_SID_SAVEBLOCK1_START + (int)(last / G3_SECTOR_DATA_SIZE);
}

/* NOTE: this checksums the whole 3968-byte data region. For partial sections (e.g.
 * SaveBlock2 = id 0, whose struct is < 3968) that equals the game's per-size checksum
 * ONLY because the unused tail bytes are zero — true for real saves, and proven by the
 * HW-validated trainer-card editor which writes id 0 through this same path. */
uint32_t gen3_write_full_section(uint8_t* save, int slot, int section_id,
                                 const uint8_t* data) {
  int s = gen3_find_section(save, slot, section_id);
  if (s < 0) return 0xFFFFFFFFu;
  uint32_t base = (uint32_t)slot * G3_SLOT_BYTES;
  uint32_t secoff = base + (uint32_t)s * G3_SECTOR_SIZE;
  uint8_t* sec = save + secoff;

  /* Overwrite only the 3968-byte data region; padding/footer preserved. */
  memcpy(sec, data, G3_SECTOR_DATA_SIZE);
  /* Recompute checksum over the data region (full section -> size 3968). */
  uint16_t cs = gen3_checksum(sec, G3_SECTOR_DATA_SIZE);
  wr16(sec + G3_OFF_CHECKSUM, cs);
  return secoff;
}

bool gen3_parse_into(const uint8_t* save, uint32_t size, Gen3SaveInfo* out,
                     uint8_t* sb1_scratch) {
  memset(out, 0, sizeof(*out));
  out->version_guess = G3_VER_UNKNOWN;
  if (size < (uint32_t)G3_SLOT_BYTES) return false;

  /* Per-slot counter from the first sector with a valid signature. */
  for (int slot = 0; slot < G3_NUM_SLOTS; slot++) {
    uint32_t base = (uint32_t)slot * G3_SLOT_BYTES;
    if (base + (uint32_t)G3_SLOT_BYTES > size) continue;
    for (int s = 0; s < G3_SECTORS_PER_SLOT; s++) {
      const uint8_t* sec = save + base + (uint32_t)s * G3_SECTOR_SIZE;
      if (rd32(sec + G3_OFF_SIGNATURE) == G3_SIGNATURE) {
        out->counter[slot] = rd32(sec + G3_OFF_COUNTER);
        out->slot_valid[slot] = true;
        break;
      }
    }
  }

  int slot = -1;
  if (out->slot_valid[0] && out->slot_valid[1])
    slot = (out->counter[1] > out->counter[0]) ? 1 : 0;
  else if (out->slot_valid[0]) slot = 0;
  else if (out->slot_valid[1]) slot = 1;
  if (slot < 0) return false;
  out->slot = slot;

  /* Count distinct valid sections in the chosen slot. */
  for (int id = 0; id < G3_SECTORS_PER_SLOT; id++)
    if (gen3_find_section(save, slot, id) >= 0) out->sections_found++;

  /* SaveBlock2 (id 0) -> trainer info. */
  int sb2_sec = gen3_find_section(save, slot, G3_SID_SAVEBLOCK2);
  if (sb2_sec < 0) return false;
  uint32_t base = (uint32_t)slot * G3_SLOT_BYTES;
  const uint8_t* sb2 = save + base + (uint32_t)sb2_sec * G3_SECTOR_SIZE;

  int i;
  for (i = 0; i < G3_PLAYER_NAME_LEN; i++) {
    char ch = gen3_decode_char(sb2[SB2_OFF_PLAYER_NAME + i]);
    if (ch == 0) break;
    out->trainer_name[i] = ch;
  }
  out->trainer_name[i] = 0;
  out->gender     = sb2[SB2_OFF_GENDER];
  out->tid_public = rd16(sb2 + SB2_OFF_TRAINER_ID);
  out->tid_secret = rd16(sb2 + SB2_OFF_TRAINER_ID + 2);
  out->play_h     = rd16(sb2 + SB2_OFF_PLAYTIME_H);
  out->play_m     = sb2[SB2_OFF_PLAYTIME_M];
  out->play_s     = sb2[SB2_OFF_PLAYTIME_S];

  /* Version detection via secret-base sanity at each candidate offset.
   *
   * sb1 is the CALLER'S buffer, not a local — see gen3_parse_into's contract in
   * gen3_save.h. A local `uint8_t sb1[G3_SAVEBLOCK1_BYTES]` here is a 15,876-byte
   * stack frame, which is larger than the GBA's entire IWRAM stack. */
  uint8_t* sb1 = sb1_scratch;
  if (sb1 && gen3_read_saveblock1(save, slot, sb1) == G3_SAVEBLOCK1_BYTES) {
    out->sb1_ok   = true;
    out->bases_em = gen3_count_secret_bases(sb1, G3_VER_EMERALD);
    out->bases_rs = gen3_count_secret_bases(sb1, G3_VER_RS);

    uint32_t key = rd32(sb2 + SB2_OFF_ENCRYPT_KEY); /* Emerald only */
    if (out->bases_em > out->bases_rs)
      out->version_guess = G3_VER_EMERALD;
    else if (out->bases_rs > out->bases_em)
      out->version_guess = G3_VER_RS;
    else if (key != 0)
      out->version_guess = G3_VER_EMERALD;
    else
      /* Tie AND no encryption key => Ruby/Sapphire, not "unknown".
       *
       * Emerald generates encryptionKey at new-game and never clears it, so ANY real
       * Emerald save has a nonzero word here; Ruby/Sapphire have no key field at this
       * offset at all. Returning UNKNOWN made pdna_main.c:3496 fall back to PK_EMERALD,
       * which broke every RS save with no secret bases: MEASURED on Guy's own cartridges,
       * his Sapphire save has 0 bases and key 0x00000000 and was mis-detected as Emerald,
       * so the map screen refused his own Sapphire ROM with "WRONG GAME". (His Ruby save
       * survived only because it HAS secret bases — its 0xAC happens to hold 0xFFD3CFC1,
       * which would have mis-detected it too on a tie.)
       *
       * Residual risk unchanged: an RS save with tied base counts AND nonzero junk at
       * 0xAC still reads as Emerald. Distinguishing those needs a stronger discriminator
       * than this function has. */
      out->version_guess = G3_VER_RS;
  }

  out->valid = true;
  return true;
}

#ifndef __arm__
/* Host-test convenience wrapper; see gen3_save.h for why the cartridge build has no
 * such thing. A PC has megabytes of stack and .bss, so the scratch is a static here. */
bool gen3_parse(const uint8_t* save, uint32_t size, Gen3SaveInfo* out) {
  static uint8_t s_sb1[G3_SAVEBLOCK1_BYTES];
  return gen3_parse_into(save, size, out, s_sb1);
}
#endif

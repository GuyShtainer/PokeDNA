#include <string.h>
#include "gen3_record.h"
#include "gen3_save.h"     /* gen3_decode_char, G3_SAVE_FILE_SIZE */
#include "gen3_mon.h"      /* pk_decode_mon, pk_is_shiny (sidecar team summary) */

/* little-endian readers (file-local, like every gen3_* core) */
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* struct RecordedBattleSave field offsets (relative to G3_REC_STRUCT_OFF) */
#define RO_NAMES      1200u   /* u8[4][8], Gen-3 charset, EOS 0xFF   */
#define RO_GENDERS    1232u   /* u8[4]                               */
#define RO_SEED       1256u   /* u32 rngSeed                         */
#define RO_FLAGS      1260u   /* u32 battleFlags                     */
#define RO_OPP_A      1268u   /* u16                                 */
#define RO_OPP_B      1270u   /* u16                                 */
#define RO_PARTNER    1272u   /* u16                                 */
#define RO_MPID       1274u   /* u16 multiplayerId                   */
#define RO_LVLMODE    1276u   /* u8 0=Lv50 1=Open                    */
#define RO_FACILITY   1277u   /* u8 0..6                             */
#define RO_LANES      1308u   /* u8[4][664] recorded inputs          */
#define RO_CHECKSUM   3964u   /* u32 byte-sum of bytes [0, 3963]     */

/* battleFlags sanity mask: bits that can never appear in a stored record (the
 * game rejects a record carrying any of them; nonzero flags are also required). */
#define REC_BAD_FLAGS 0x7D007E92u

bool g3_record_scan(const uint8_t* save, uint32_t size, G3RecordInfo* out) {
  memset(out, 0, sizeof *out);
  if (!save || size < (uint32_t)G3_SAVE_FILE_SIZE) return false;   /* 64 KiB dump: no sector 31 */
  if (rd32(save + G3_REC_SECTOR_OFF) != 0x0000B39Du) return false; /* erased / never recorded   */
  out->present = true;

  const uint8_t* r = save + G3_REC_STRUCT_OFF;
  uint32_t sum = 0;
  for (uint32_t i = 0; i < RO_CHECKSUM; i++) sum += r[i];
  out->checksum_ok = (sum == rd32(r + RO_CHECKSUM));

  out->rng_seed     = rd32(r + RO_SEED);
  out->battle_flags = rd32(r + RO_FLAGS);
  out->flags_ok     = out->battle_flags != 0 && (out->battle_flags & REC_BAD_FLAGS) == 0;
  out->opponent_a   = rd16(r + RO_OPP_A);
  out->opponent_b   = rd16(r + RO_OPP_B);
  out->partner_id   = rd16(r + RO_PARTNER);
  out->multiplayer_id = rd16(r + RO_MPID);
  out->lvl_mode     = r[RO_LVLMODE];
  out->facility     = r[RO_FACILITY];

  for (int p = 0; p < 4; p++) {
    const uint8_t* n = r + RO_NAMES + (uint32_t)p * 8;
    int o = 0, blank = 1;
    for (int i = 0; i < 7 && n[i] != 0xFF; i++) {
      char c = gen3_decode_char(n[i]);
      if (c) { out->names[p][o++] = c; if (c != ' ') blank = 0; }
    }
    out->names[p][o] = 0;
    if (blank) out->names[p][0] = 0;   /* zero-filled slot (non-link battle) decodes to spaces */
    out->genders[p] = r[RO_GENDERS + p];
    const uint8_t* lane = r + RO_LANES + (uint32_t)p * G3_REC_LANE_SIZE;
    int len = 0;
    while (len < (int)G3_REC_LANE_SIZE && lane[len] != 0xFF) len++;
    out->lane_len[p] = len;
  }
  return true;
}

const uint8_t* g3_record_party(const uint8_t* save, int side) {
  return save + G3_REC_STRUCT_OFF + (side ? 600u : 0u);
}

const char* g3_record_facility_name(int facility) {
  static const char* const k_fac[7] = {
    "Battle Tower", "Battle Dome", "Battle Palace", "Battle Arena",
    "Battle Factory", "Battle Pike", "Battle Pyramid",
  };
  return (facility >= 0 && facility < 7) ? k_fac[facility] : "?";
}

const char* g3_record_facility_short(int facility) {
  static const char* const k_short[7] = {
    "Tower", "Dome", "Palace", "Arena", "Factory", "Pike", "Pyramid",
  };
  return (facility >= 0 && facility < 7) ? k_short[facility] : "?";
}

/* ---- current win streak (Emerald SaveBlock2 BattleFrontier) -----------------
 * Offsets are ABSOLUTE within SaveBlock2 and taken from the decomp's own offset
 * comments (struct BattleFrontier is embedded at SB2+0x64C and the comments are
 * already SB2-absolute — pokeemerald include/global.h:378-465, member at :541):
 *   towerWinStreaks  [4][2] u16 @ 0xCE0   include/global.h:393
 *   domeWinStreaks   [2][2] u16 @ 0xD0C   include/global.h:411
 *   palaceWinStreaks [2][2] u16 @ 0xDC8   include/global.h:418
 *   arenaWinStreaks     [2] u16 @ 0xDDA   include/global.h:421
 *   factoryWinStreaks[2][2] u16 @ 0xDE2   include/global.h:423
 *   pikeWinStreaks      [2] u16 @ 0xE04   include/global.h:428
 *   pyramidWinStreaks   [2] u16 @ 0xE1A   include/global.h:437
 * Indexing mirrors GetCurrentFacilityWinStreak (src/frontier_util.c:1804-1830):
 * [battleMode][lvlMode] where a facility has per-mode lanes, [lvlMode] alone for
 * Arena/Pike/Pyramid. The record stores lvlMode = frontier.lvlMode and facility =
 * VAR_FRONTIER_FACILITY verbatim (src/recorded_battle.c:385-386; 0 = Lv50,
 * 1 = Open per FRONTIER_LVL_* in include/constants/global.h:76-78). The battle
 * mode is recovered from the record's battleFlags (include/constants/battle.h:
 * 59 DOUBLE 1<<0, 65 MULTI 1<<6, 82 TOWER_LINK_MULTI 1<<23). All streak fields
 * are plaintext u16 — NOT obfuscated by Emerald's security key. */
static const struct { uint16_t off; uint8_t modes; } k_lane[7] = {
  { 0xCE0, 4 },   /* Tower:   singles/doubles/multis/link-multis */
  { 0xD0C, 2 },   /* Dome:    singles/doubles                    */
  { 0xDC8, 2 },   /* Palace:  singles/doubles                    */
  { 0xDDA, 1 },   /* Arena                                       */
  { 0xDE2, 2 },   /* Factory: singles/doubles                    */
  { 0xE04, 1 },   /* Pike                                        */
  { 0xE1A, 1 },   /* Pyramid                                     */
};

int g3_facility_streak(const uint8_t* sb2, int facility, int mode, int lvl) {
  if (!sb2 || facility < 0 || facility >= 7 || lvl < 0 || lvl > 1) return -1;
  if (mode < 0 || mode >= k_lane[facility].modes) return -1;
  uint32_t v = rd16(sb2 + k_lane[facility].off + ((uint32_t)mode * 2u + (uint32_t)lvl) * 2u);
  return (v <= (uint32_t)G3_REC_MAX_STREAK) ? (int)v : -1;
}

/* The record's own battle mode, recovered from its battleFlags. */
static int rec_mode(const G3RecordInfo* ri) {
  if (ri->battle_flags & (1u << 6))               /* BATTLE_TYPE_MULTI     */
    return (ri->battle_flags & (1u << 23)) ? 3 : 2;
  if (ri->battle_flags & (1u << 0))               /* BATTLE_TYPE_DOUBLE    */
    return 1;
  return 0;                                       /* FRONTIER_MODE_SINGLES */
}

int g3_record_win_streak(const uint8_t* sb2, const G3RecordInfo* ri) {
  if (!sb2 || !ri) return -1;
  if (ri->facility >= 7 || ri->lvl_mode > 1) return -1;
  return g3_facility_streak(sb2, ri->facility, rec_mode(ri), ri->lvl_mode);
}

/* ---- export sidecar ---------------------------------------------------------
 * Everything knowable at export time that the .rec itself cannot carry, as
 * human-readable text (the PC-side rec2mp4 writes its own JSON per video; the
 * two stay complementary, not coupled). Truncation-safe append writer. */
static void sc_put(char* out, int cap, int* n, const char* s) {
  while (*s && *n < cap - 1) out[(*n)++] = *s++;
  out[*n] = 0;
}
static void sc_num(char* out, int cap, int* n, long v) {
  char b[12]; int i = 0;
  if (v < 0) { sc_put(out, cap, n, "-"); v = -v; }
  do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < 11);
  while (i) { char c[2] = { b[--i], 0 }; sc_put(out, cap, n, c); }
}
static void sc_hex8(char* out, int cap, int* n, uint32_t v) {
  for (int s = 28; s >= 0; s -= 4) {
    char c[2] = { "0123456789abcdef"[(v >> s) & 0xF], 0 };
    sc_put(out, cap, n, c);
  }
}

int g3_record_sidecar(char* out, int cap, const G3RecordInfo* ri,
                      const uint8_t* save, const uint8_t* sb2,
                      uint16_t tid_public, const char* stamp) {
  static const char* const k_mode[4] = { "singles", "doubles", "multis", "link" };
  int n = 0;
  if (cap < 2) return 0;
  out[0] = 0;
  sc_put(out, cap, &n, "PokeDNA battle record export\n");
  sc_put(out, cap, &n, "exported: "); sc_put(out, cap, &n, stamp ? stamp : "RTC unavailable");
  sc_put(out, cap, &n, "\nplayer: ");
  sc_put(out, cap, &n, ri->names[ri->multiplayer_id][0] ? ri->names[ri->multiplayer_id] : "?");
  sc_put(out, cap, &n, ri->genders[ri->multiplayer_id] ? " (F)" : " (M)");
  sc_put(out, cap, &n, "  IDNo "); sc_num(out, cap, &n, tid_public);
  sc_put(out, cap, &n, "\nrecord: "); sc_put(out, cap, &n, g3_record_facility_name(ri->facility));
  sc_put(out, cap, &n, ri->lvl_mode ? ", Open Level, " : ", Level 50, ");
  sc_put(out, cap, &n, k_mode[rec_mode(ri)]);
  sc_put(out, cap, &n, ", seed "); sc_hex8(out, cap, &n, ri->rng_seed);
  sc_put(out, cap, &n, "\nopponent: #"); sc_num(out, cap, &n, ri->opponent_a);
  if (ri->opponent_b) { sc_put(out, cap, &n, " + #"); sc_num(out, cap, &n, ri->opponent_b); }
  if (ri->partner_id) { sc_put(out, cap, &n, "  partner #"); sc_num(out, cap, &n, ri->partner_id); }
  sc_put(out, cap, &n, "\ninput bytes:");
  for (int p = 0; p < 4; p++) { sc_put(out, cap, &n, " "); sc_num(out, cap, &n, ri->lane_len[p]); }
  for (int side = 0; side < 2; side++) {
    sc_put(out, cap, &n, side ? "\nopponent team:\n" : "\nplayer team:\n");
    const uint8_t* party = g3_record_party(save, side);
    for (int i = 0; i < 6; i++) {
      PkMon m;
      if (!pk_decode_mon(party + (uint32_t)i * G3_REC_MON_SIZE, false, &m) || !m.species) continue;
      sc_put(out, cap, &n, "  #"); sc_num(out, cap, &n, m.species);
      sc_put(out, cap, &n, " "); sc_put(out, cap, &n, m.nickname);
      sc_put(out, cap, &n, " Lv"); sc_num(out, cap, &n, party[(uint32_t)i * G3_REC_MON_SIZE + 84]);
      { const uint8_t* mb = party + (uint32_t)i * G3_REC_MON_SIZE;   /* raw pers/otId (plaintext) */
        uint32_t pers = rd32(mb), otid = rd32(mb + 4);
        if (pk_is_shiny(pers, (uint16_t)(otid & 0xFFFF), (uint16_t)(otid >> 16)))
          sc_put(out, cap, &n, " shiny"); }
      sc_put(out, cap, &n, "\n");
    }
  }
  sc_put(out, cap, &n, "current streaks (Lv50/Open):\n");
  for (int f = 0; f < 7; f++) {
    sc_put(out, cap, &n, "  "); sc_put(out, cap, &n, g3_record_facility_short(f));
    for (int m = 0; m < 4; m++) {
      int a = g3_facility_streak(sb2, f, m, 0);
      if (a < 0) break;                             /* facility has no more mode lanes */
      int b = g3_facility_streak(sb2, f, m, 1);
      sc_put(out, cap, &n, "  "); sc_put(out, cap, &n, k_mode[m]);
      sc_put(out, cap, &n, " "); sc_num(out, cap, &n, a);
      sc_put(out, cap, &n, "/"); sc_num(out, cap, &n, b < 0 ? 0 : b);
    }
    sc_put(out, cap, &n, "\n");
  }
  return n;
}

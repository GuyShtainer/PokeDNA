#include "gen3_contest.h"
#include "gen3_save.h"   /* gen3_decode_char */
#include <string.h>

/* ASCII -> Gen-3 charset, same table as gen3_edit.c's gen3_encode_char. Duplicated
 * (not #included from gen3_edit.h) so this pure core does not drag in gen3_edit.c's
 * whole dependency chain (data_tables/gen3_daycare/learnsets2/evolutions/gen3_mon)
 * just for one self-contained 12-line function — the same rd16/rd32/wr16/wr32
 * duplication pattern every other pure-C core in this codebase already uses. */
static uint8_t gc_encode_char(char c) {
  if (c == ' ') return 0x00;
  if (c >= '0' && c <= '9') return (uint8_t)(0xA1 + (c - '0'));
  if (c >= 'A' && c <= 'Z') return (uint8_t)(0xBB + (c - 'A'));
  if (c >= 'a' && c <= 'z') return (uint8_t)(0xD5 + (c - 'a'));
  switch (c) {
    case '!':  return 0xAB;
    case '?':  return 0xAC;
    case '.':  return 0xAD;
    case '-':  return 0xAE;
    case '\'': return 0xB4;
    case ',':  return 0xB8;
    case '/':  return 0xBA;
    default:   return 0x00;
  }
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ---- ribbon rank: gc_ribbon_get/set are static inline in gen3_contest.h now -----
 * (see there — header-only so gen3_edit.c can call them with no .o dependency). --- */

bool gc_ribbon_flag_get(uint32_t ribbons, int flagbit) {
  if (flagbit < GC_RFLAG_CHAMPION || flagbit > GC_RFLAG_WORLD) return false;
  return (ribbons >> flagbit) & 1u;
}

uint32_t gc_ribbon_flag_set(uint32_t ribbons, int flagbit, bool on) {
  if (flagbit < GC_RFLAG_CHAMPION || flagbit > GC_RFLAG_WORLD) return ribbons;
  uint32_t bit = 1u << flagbit;
  return on ? (ribbons | bit) : (ribbons & ~bit);
}

/* ---- per-game SaveBlock1 layout (see gen3_contest.h for the derivation) -------- */
#define GC_STRIDE 0x20u

static uint32_t hall_base(PkGame g) { return (g == PK_EMERALD) ? 0x2E90u : 0x2DFCu; }
static uint32_t museum_base(PkGame g) { return (g == PK_EMERALD) ? 0x2F90u : 0x2EFCu; }

bool gc_supported(PkGame g) { return g != PK_FRLG; }

int gc_hall_count(PkGame g) {
  if (g == PK_EMERALD) return 6;
  if (g == PK_RS)      return 8;
  return 0;
}

static void decode_name(char* out, const uint8_t* src, int maxlen) {
  int k = 0;
  for (; k < maxlen; k++) {
    char ch = gen3_decode_char(src[k]);
    if (ch == 0) break;
    out[k] = ch;
  }
  out[k] = 0;
}

static void encode_name(uint8_t* dst, const char* ascii, int len) {
  memset(dst, 0xFF, (size_t)len);
  if (!ascii) return;
  int k = 0;
  for (; k < len && ascii[k]; k++) dst[k] = gc_encode_char(ascii[k]);
}

/* `museum`: a museum-slot record's byte +10 is NOT the plain 0..4 category — it is
 * a "painting caption id" = 3*category + a random 0..2 flavor-text variant
 * (pokeemerald src/contest.c:5527-5552 / pokeruby src/contest_2.c:4074-4090,
 * NUM_PAINTING_CAPTIONS==3; contest_painting.c:530 recovers the category the same
 * way, `/ NUM_PAINTING_CAPTIONS`, to pick the right sMuseumCaptions text and sprite
 * layout). A HALL-winner record's byte +10 IS the plain category (same functions,
 * `rank != CONTEST_SAVE_FOR_MUSEUM` branch) — confirmed against Guy's own saves
 * below (every hall category value read back 0..4, never above). Getting this
 * wrong would not corrupt the save, but would show the wrong caption/category on
 * the in-game painting — this is the offset-vs-real-save subtlety the brief's
 * stop-licence flagged; verified, not assumed. */
#define GC_PAINTING_VARIANTS 3

/* RS has no contestRank field in ContestWinner -- GetContestWinnerSaveIdx (pokeruby
 * src/contest_2.c:4130-4155) picks WHICH of the 8 hall slots a win lands in FROM the
 * rank, so on read the rank must be recovered the same way, from the slot index:
 * Normal->0, Super->1, Hyper->2..4, Master->5..7. A museum slot has no such mapping
 * (no slot varies by rank) -- ShouldReadyContestArtist gates it to Master, always. */
static const uint8_t RS_SLOT_RANK[8] = { 0, 1, 2, 2, 2, 3, 3, 3 };

static bool read_winner(const uint8_t* sb1, uint32_t off, bool has_rank, bool museum,
                        int rs_slot, GcWinner* out) {
  const uint8_t* w = sb1 + off;
  memset(out, 0, sizeof(*out));
  out->personality = rd32(w + 0);
  out->otId        = rd32(w + 4);
  out->species     = rd16(w + 8);
  out->category    = museum ? (uint8_t)(w[10] / GC_PAINTING_VARIANTS) : w[10];
  decode_name(out->monName, w + 11, 10);
  decode_name(out->trainerName, w + 22, 7);
  /* Emerald's stored byte IS the CONTEST_RANK_* scale already (contestRank field) --
   * read it raw, never remapped. RS has no such byte: a museum slot is always Master,
   * a hall slot's rank is implied by which of the 8 slots it occupies. */
  if (has_rank)      out->rank = w[30];
  else if (museum)   out->rank = (uint8_t)CONTEST_RANK_MASTER;
  else               out->rank = RS_SLOT_RANK[rs_slot];
  return true;
}

bool gc_hall_get(const uint8_t* sb1, PkGame g, int idx, GcWinner* out) {
  if (!sb1 || !out || !gc_supported(g)) return false;
  int n = gc_hall_count(g);
  if (idx < 0 || idx >= n) return false;
  return read_winner(sb1, hall_base(g) + (uint32_t)idx * GC_STRIDE, g == PK_EMERALD, false,
                     idx, out);
}

bool gc_museum_get(const uint8_t* sb1, PkGame g, int cat, GcWinner* out) {
  if (!sb1 || !out || !gc_supported(g)) return false;
  if (cat < 0 || cat >= GC_CATEGORY_COUNT) return false;
  return read_winner(sb1, museum_base(g) + (uint32_t)cat * GC_STRIDE, g == PK_EMERALD, true,
                     0, out);
}

bool gc_museum_set_raw(uint8_t* sb1, PkGame g, int cat, uint16_t species, uint32_t personality,
                       uint32_t otId, const char* monName_ascii,
                       const uint8_t trainerName_raw8[8]) {
  if (!sb1 || !gc_supported(g)) return false;
  if (cat < 0 || cat >= GC_CATEGORY_COUNT) return false;

  uint8_t rec[GC_STRIDE];
  memset(rec, 0, sizeof(rec));
  wr32(rec + 0, personality);
  wr32(rec + 4, otId);
  wr16(rec + 8, species);
  /* Caption variant 0 (of 0..2) — this tool has no "how well did it score" context
   * to pick a fitting variant, and variant 0 is a real value the game itself
   * produces (Random() % 3 can roll 0), not an invented sentinel. */
  rec[10] = (uint8_t)(cat * GC_PAINTING_VARIANTS);
  encode_name(rec + 11, monName_ascii, 11);
  /* Trainer name is copied verbatim (already Gen-3 encoded bytes from the caller,
   * e.g. sb2's own OT name) — NOT re-encoded. gen3_decode_char/gc_encode_char is a
   * lossy round trip for any byte outside the plain-text subset (MALE_SYMBOL 0xB5,
   * the PK/MN ligatures, etc.) collapses to '?' (0xAC) on the way back out. A
   * verbatim memcpy is the only way to reproduce a real player's OT name exactly. */
  memcpy(rec + 22, trainerName_raw8, 8);
  if (g == PK_EMERALD) rec[30] = (uint8_t)CONTEST_RANK_MASTER;

  uint8_t* w = sb1 + museum_base(g) + (uint32_t)cat * GC_STRIDE;
  memcpy(w, rec, sizeof(rec));   /* memcpy, not per-field writes: a byte-identical
                                  * donor must leave sb1 untouched at the memory-
                                  * compare level, and the caller diffs before/after
                                  * to decide whether to commit at all. */
  return true;
}

bool gc_museum_set(uint8_t* sb1, PkGame g, int cat, uint16_t species, uint32_t personality,
                   uint32_t otId, const char* monName_ascii, const char* trainerName_ascii) {
  uint8_t raw[8];
  encode_name(raw, trainerName_ascii, 8);
  return gc_museum_set_raw(sb1, g, cat, species, personality, otId, monName_ascii, raw);
}

uint32_t gc_museum_offset(PkGame g, int cat) {
  if (!gc_supported(g) || cat < 0 || cat >= GC_CATEGORY_COUNT) return 0;
  return museum_base(g) + (uint32_t)cat * GC_STRIDE;
}

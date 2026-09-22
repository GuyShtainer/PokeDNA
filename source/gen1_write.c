#include <string.h>
#include "gen1_write.h"

/* Generation-I (R/B/Y, Western) save WRITER. See gen1_write.h for the contract, the
 * four-parallel-structures problem, the current-box duality and the refusal policy.
 *
 * Facts are from pret/pokered (assets/upstream/pokered) — reference only, per
 * docs/kb/licensing.md; no decomp code is copied. Citations are file:line into that
 * checkout. Every offset used here was additionally confirmed against Guy's real
 * Red.sav, and the two places where the decomp and a wiki would disagree are called out.
 */

/* --- big-endian helpers. The GB stores multi-byte fields high byte first; gen1_save.c
 * keeps its own private pair for the same reason (a shared little-endian helper would
 * be far too easy to grab by mistake). --- */
static uint16_t rd16be(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static void     wr16be(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void     wr24be(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v;
}

const char* gen1_write_status_text(Gen1WStatus st) {
  switch (st) {
    case GEN1W_OK:           return "OK";
    case GEN1W_ERR_ARG:      return "bad argument";
    case GEN1W_ERR_SIZE:     return "not a 32 KiB Game Boy save";
    case GEN1W_ERR_SAVE:     return "not a valid Western R/B/Y save";
    case GEN1W_ERR_VIRGIN:   return "the stored boxes are not initialised yet — "
                                    "use CHANGE BOX in the game once, or edit the open box";
    case GEN1W_ERR_UNINIT:   return "that box has never been initialised";
    case GEN1W_ERR_FULL:     return "that box is full";
    case GEN1W_ERR_EMPTY:    return "that slot is empty";
    case GEN1W_ERR_SPECIES:  return "not a Gen-1 species index";
    case GEN1W_ERR_TEXT:     return "that name has a character Gen 1 cannot store";
    case GEN1W_ERR_STRUCT:   return "the box's count/species list is inconsistent";
    case GEN1W_ERR_VERIFY:   return "it did not read back as intended — write refused";
    case GEN1W_ERR_RANGE:    return "outside the header span, or on bytes this module "
                                    "reserves for a Pokemon edit or the checksum";
    default:                 return "?";
  }
}

/* ------------------------------------------------------------------------- */
/* Text: ASCII/UTF-8 -> the Gen-1 charset                                     */
/* ------------------------------------------------------------------------- */

int gen1_encode_char(const char* s, uint8_t* out) {
  const unsigned char* p = (const unsigned char*)s;
  char c;
  if (!p || !out || !p[0]) return 0;

  /* The gender signs, spelled exactly as gen1_char_ascii() emits them (UTF-8
   * U+2642/U+2640). && short-circuits through the NUL, so a string ending in a bare
   * 0xE2 never reads past its terminator. */
  if (p[0] == 0xE2u && p[1] == 0x99u) {
    if (p[2] == 0x82u) { *out = 0xEFu; return 3; }
    if (p[2] == 0x80u) { *out = 0xF5u; return 3; }
    return 0;
  }

  /* BACKLOG #216b: e-acute, spelled exactly as gen1_char_ascii() now emits it (UTF-8
   * "\xC3\xA9") -- the exact inverse of gen1_save.c:99. Gen 1 has no umlaut code
   * points at all (pokered/constants/charmap.asm has none), so nothing else lands
   * here; an Ä/Ö/Ü/ä/ö/ü/× falls through to the `p[0] >= 0x80u` catch-all below and
   * is refused, same as before this fix. */
  if (p[0] == 0xC3u && p[1] == 0xA9u) { *out = 0xBAu; return 2; }

  /* The five lowercase contractions plus 'r and 'm are ONE byte on the GB and two
   * characters in ASCII (gen1_save.c:104-114). Fold them back; a bare apostrophe with
   * anything else after it is the standalone 0xE0. */
  if (p[0] == '\'') {
    switch (p[1]) {
      case 'd': *out = 0xBBu; return 2;
      case 'l': *out = 0xBCu; return 2;
      case 's': *out = 0xBDu; return 2;
      case 't': *out = 0xBEu; return 2;
      case 'v': *out = 0xBFu; return 2;
      case 'r': *out = 0xE4u; return 2;
      case 'm': *out = 0xE5u; return 2;
      default:  *out = 0xE0u; return 1;
    }
  }
  if (p[0] >= 0x80u) return 0;            /* any other non-ASCII: no representation */

  c = (char)p[0];
  if (c >= 'A' && c <= 'Z') { *out = (uint8_t)(0x80 + (c - 'A')); return 1; }
  if (c >= 'a' && c <= 'z') { *out = (uint8_t)(0xA0 + (c - 'a')); return 1; }
  if (c >= '0' && c <= '9') { *out = (uint8_t)(0xF6 + (c - '0')); return 1; }
  switch (c) {
    case ' ': *out = 0x7Fu; return 1;
    case '(': *out = 0x9Au; return 1;
    case ')': *out = 0x9Bu; return 1;
    case ':': *out = 0x9Cu; return 1;
    case ';': *out = 0x9Du; return 1;
    case '[': *out = 0x9Eu; return 1;
    case ']': *out = 0x9Fu; return 1;
    case '-': *out = 0xE3u; return 1;
    case '?': *out = 0xE6u; return 1;
    case '!': *out = 0xE7u; return 1;
    case '.': *out = 0xE8u; return 1;   /* not 0xF2: both decode to '.', this is the
                                         * one the games' own text uses */
    case '/': *out = 0xF3u; return 1;
    case ',': *out = 0xF4u; return 1;
    default:  return 0;                 /* refuse; NEVER substitute a lookalike */
  }
}

bool gen1_encode_name(uint8_t dst[GEN1_NAME_BYTES], const char* utf8, int max_glyphs) {
  uint8_t tmp[GEN1_NAME_BYTES];
  int n = 0;
  if (!dst || !utf8 || max_glyphs < 1 || max_glyphs > GEN1_NAME_BYTES - 1) return false;
  if (!utf8[0]) return false;                    /* empty: see the header */
  while (*utf8) {
    uint8_t b;
    int used;
    if (n >= max_glyphs) return false;           /* too long: refuse, never truncate */
    used = gen1_encode_char(utf8, &b);
    if (used == 0) return false;
    tmp[n++] = b;
    utf8 += used;
  }
  for (; n < GEN1_NAME_BYTES; n++) tmp[n] = GEN1_TEXT_TERM;
  memcpy(dst, tmp, GEN1_NAME_BYTES);             /* dst untouched unless we succeeded */
  return true;
}

/* ------------------------------------------------------------------------- */
/* Blob geometry                                                              */
/* ------------------------------------------------------------------------- */

void gen1_write_layout(int box, Gen1Layout* out) {
  const bool party = (box == GEN1_PARTY_BOX);
  if (!out) return;
  out->cap      = party ? GEN1_PARTY_CAPACITY  : GEN1_BOX_CAPACITY;
  out->recsz    = party ? GEN1_PARTY_REC_BYTES : GEN1_BOX_REC_BYTES;
  out->bytes    = party ? (int)GEN1_PARTY_BYTES : (int)GEN1_BOX_BYTES;
  out->rec_off  = 1 + (out->cap + 1);            /* count, then the species list */
  out->ot_off   = out->rec_off + out->cap * out->recsz;
  out->nick_off = out->ot_off  + out->cap * GEN1_NAME_BYTES;
}

/* ------------------------------------------------------------------------- */
/* Gen-1 arithmetic                                                           */
/* ------------------------------------------------------------------------- */

uint8_t gen1_derive_hp_dv(uint8_t atk, uint8_t def, uint8_t spe, uint8_t spc) {
  return (uint8_t)(((atk & 1u) << 3) | ((def & 1u) << 2) | ((spe & 1u) << 1) | (spc & 1u));
}

/* ceil(sqrt(statexp)) / 4, counted the way the game counts it: b climbs from 1 and
 * STOPS AT 255 (pokered home/move_mon.asm:73-92 — `cp $ff / jr z, .statExpDone` fires
 * before the multiply). So a maxed 65535 stat exp yields 255, not 256, and a bonus of
 * 63 rather than 64. Reproducing that off-by-one is the point of citing the source. */
static uint8_t statexp_bonus(uint16_t statexp) {
  uint32_t b = 0;
  for (;;) {
    b++;
    if (b == 0xFFu) break;
    if (b * b >= (uint32_t)statexp) break;
  }
  return (uint8_t)(b >> 2);                      /* srl b; srl b */
}

uint16_t gen1_calc_stat(int stat, uint8_t base, uint8_t dv, uint16_t statexp, uint8_t level) {
  uint32_t t;
  if (stat < 0 || stat >= G1_NSTATS) return 0;
  if (dv > 15) dv = 15;
  t = (((uint32_t)base + dv) * 2u) + statexp_bonus(statexp);
  t = (t * (uint32_t)level) / 100u;
  t += (stat == G1_HP) ? ((uint32_t)level + 10u) : 5u;
  if (t > 999u) t = 999u;                        /* MAX_STAT_VALUE, ibid.:212-226 */
  return (uint16_t)t;
}

/* pokered data/growth_rates.asm: "[1]/[2]*n**3 + [3]*n**2 + [4]*n - [5]", with the
 * n^2 coefficient stored as a signed magnitude (bit 7 = negative). */
typedef struct { uint8_t num, den, csign, cmag, d, e; } G1Growth;
static const G1Growth k_growth[GEN1_NUM_GROWTH_RATES] = {
  { 1, 1, 0,  0,   0,   0 },   /* Medium Fast   */
  { 3, 4, 0, 10,   0,  30 },   /* Slightly Fast */
  { 3, 4, 0, 20,   0,  70 },   /* Slightly Slow */
  { 6, 5, 1, 15, 100, 140 },   /* Medium Slow   */
  { 4, 5, 0,  0,   0,   0 },   /* Fast          */
  { 5, 4, 0,  0,   0,   0 },   /* Slow          */
};

uint32_t gen1_exp_for_level(uint8_t growth, uint8_t level) {
  const G1Growth* g;
  uint32_t n, cube, sq, v;
  if (growth >= GEN1_NUM_GROWTH_RATES) return 0;
  /* The game never asks for level 1: CalcLevelFromExperience starts its search at 2
   * (engine/pokemon/experience.asm:5-8) and nothing is ever created below level 2. At
   * n=1 the Medium Slow polynomial is negative and the game's 3-byte arithmetic would
   * wrap it to ~16.7 million, which as a stored EXP would make the mon jump to level
   * 100 the moment it is withdrawn. 0 is the only safe answer. */
  if (level <= 1) return 0;
  if (level > 100) level = 100;
  g = &k_growth[growth];
  n = level;
  cube = ((uint32_t)g->num * n * n * n) / g->den;      /* floor(num*n^3/den) */
  sq   = (uint32_t)g->cmag * n * n;
  v    = ((uint32_t)g->d * n - (uint32_t)g->e) & 0xFFFFFFu;   /* linear - constant */
  v    = (g->csign ? (v - sq) : (v + sq)) & 0xFFFFFFu;
  v    = (v + cube) & 0xFFFFFFu;
  return v;
}

uint8_t gen1_level_from_exp(uint8_t growth, uint32_t exp) {
  uint32_t lv;
  if (growth >= GEN1_NUM_GROWTH_RATES) return 1;
  exp &= 0xFFFFFFu;
  /* CalcLevelFromExperience: climb while the requirement is not greater than the exp. */
  for (lv = 2; lv <= 100; lv++)
    if (gen1_exp_for_level(growth, (uint8_t)lv) > exp) return (uint8_t)(lv - 1);
  return 100;
}

/* ------------------------------------------------------------------------- */
/* One Pokemon, loaded for editing                                            */
/* ------------------------------------------------------------------------- */

bool gen1_edit_load(const uint8_t* list, int box, int slot, Gen1EditMon* e) {
  Gen1Layout L;
  const uint8_t* rec;
  if (!e) return false;
  memset(e, 0, sizeof *e);
  if (!list || slot < 0 || slot >= gen1_list_count(list, box)) return false;
  gen1_write_layout(box, &L);
  if (slot >= L.cap) return false;

  rec = list + L.rec_off + (size_t)slot * (size_t)L.recsz;
  if (list[1 + slot] != rec[G1R_SPECIES]) return false;   /* list_mismatch: read-only */

  e->is_party = (box == GEN1_PARTY_BOX);
  memcpy(e->rec,  rec, (size_t)L.recsz);
  memcpy(e->ot,   list + L.ot_off   + (size_t)slot * GEN1_NAME_BYTES, GEN1_NAME_BYTES);
  memcpy(e->nick, list + L.nick_off + (size_t)slot * GEN1_NAME_BYTES, GEN1_NAME_BYTES);
  return true;
}

void g1e_set_dv(Gen1EditMon* e, int stat, uint8_t v) {
  if (!e || v > 15) return;
  /* The HP DV is not stored — it is the low bit of the other four
   * (home/move_mon.asm:109-129) — so there is nothing to set. */
  switch (stat) {
    case G1_ATK: e->rec[G1R_DVS]     = (uint8_t)((e->rec[G1R_DVS]     & 0x0Fu) | ((unsigned)v << 4)); break;
    case G1_DEF: e->rec[G1R_DVS]     = (uint8_t)((e->rec[G1R_DVS]     & 0xF0u) |  v);       break;
    case G1_SPE: e->rec[G1R_DVS + 1] = (uint8_t)((e->rec[G1R_DVS + 1] & 0x0Fu) | ((unsigned)v << 4)); break;
    case G1_SPC: e->rec[G1R_DVS + 1] = (uint8_t)((e->rec[G1R_DVS + 1] & 0xF0u) |  v);       break;
    default: break;
  }
}

void g1e_set_statexp(Gen1EditMon* e, int stat, uint16_t v) {
  if (!e || stat < 0 || stat >= G1_NSTATS) return;
  wr16be(e->rec + G1R_STATEXP + stat * 2, v);
}

void g1e_set_exp(Gen1EditMon* e, uint32_t exp)   { if (e) wr24be(e->rec + G1R_EXP, exp & 0xFFFFFFu); }
void g1e_set_otid(Gen1EditMon* e, uint16_t id)   { if (e) wr16be(e->rec + G1R_OTID, id); }
void g1e_set_status(Gen1EditMon* e, uint8_t st)  { if (e) e->rec[G1R_STATUS] = st; }
void g1e_set_curhp(Gen1EditMon* e, uint16_t hp)  { if (e) wr16be(e->rec + G1R_HP, hp); }
void g1e_set_catch_rate(Gen1EditMon* e, uint8_t v) { if (e) e->rec[G1R_CATCH_RATE] = v; }

void g1e_set_move(Gen1EditMon* e, int i, uint8_t move, uint8_t base_pp) {
  if (!e || i < 0 || i > 3) return;
  e->rec[G1R_MOVES + i] = move;
  /* PP Ups belong to the move, not to the slot, so a new move starts at base PP with
   * no bonus — the same rule gen3_edit.c's em_set_move follows. */
  e->rec[G1R_PP + i] = (uint8_t)(move ? (base_pp & 0x3Fu) : 0u);
}

void g1e_set_pp(Gen1EditMon* e, int i, uint8_t cur_pp) {
  if (!e || i < 0 || i > 3) return;
  if (cur_pp > 63) cur_pp = 63;
  e->rec[G1R_PP + i] = (uint8_t)((e->rec[G1R_PP + i] & 0xC0u) | cur_pp);
}

void g1e_set_ppup(Gen1EditMon* e, int i, uint8_t ups) {
  if (!e || i < 0 || i > 3) return;
  if (ups > 3) ups = 3;
  e->rec[G1R_PP + i] = (uint8_t)((e->rec[G1R_PP + i] & 0x3Fu) | (uint8_t)(ups << 6));
}

bool g1e_set_nickname(Gen1EditMon* e, const char* utf8) {
  return e && gen1_encode_name(e->nick, utf8, GEN1_NICK_GLYPHS);
}
bool g1e_set_otname(Gen1EditMon* e, const char* utf8) {
  return e && gen1_encode_name(e->ot, utf8, GEN1_OT_GLYPHS);
}

bool g1e_set_species(Gen1EditMon* e, uint8_t internal_index, const Gen1SpeciesInfo* si) {
  if (!e || !si) return false;
  if (internal_index == 0 || internal_index > 190) return false;
  /* Five bytes, together — Gen 1 stores types and catch rate PER RECORD, so setting
   * only rec[0] leaves the old species' battle typing behind. */
  e->rec[G1R_SPECIES]    = internal_index;
  e->rec[G1R_TYPE1]      = si->type1;
  e->rec[G1R_TYPE2]      = si->type2;
  e->rec[G1R_CATCH_RATE] = si->catch_rate;
  return true;
}

void g1e_recalc_stats(Gen1EditMon* e, const Gen1SpeciesInfo* si) {
  uint8_t dv[G1_NSTATS], level;
  uint16_t maxhp = 0;
  int s;
  if (!e || !si || !e->is_party) return;         /* box records store no battle stats */
  dv[G1_ATK] = (uint8_t)(e->rec[G1R_DVS] >> 4);
  dv[G1_DEF] = (uint8_t)(e->rec[G1R_DVS] & 0x0Fu);
  dv[G1_SPE] = (uint8_t)(e->rec[G1R_DVS + 1] >> 4);
  dv[G1_SPC] = (uint8_t)(e->rec[G1R_DVS + 1] & 0x0Fu);
  dv[G1_HP]  = gen1_derive_hp_dv(dv[G1_ATK], dv[G1_DEF], dv[G1_SPE], dv[G1_SPC]);
  level = e->rec[G1R_LEVEL];
  for (s = 0; s < G1_NSTATS; s++) {
    uint16_t v = gen1_calc_stat(s, si->base[s], dv[s],
                                rd16be(e->rec + G1R_STATEXP + s * 2), level);
    wr16be(e->rec + G1R_STATS + s * 2, v);
    if (s == G1_HP) maxhp = v;
  }
  /* A stat edit that lowers max HP must not leave current HP above it. */
  if (rd16be(e->rec + G1R_HP) > maxhp) wr16be(e->rec + G1R_HP, maxhp);
}

bool g1e_set_level(Gen1EditMon* e, uint8_t level, const Gen1SpeciesInfo* si) {
  if (!e || !si || level < 1 || level > 100) return false;
  if (si->growth >= GEN1_NUM_GROWTH_RATES) return false;
  /* 0x03 is the level as of the last deposit. Retail only refreshes it when the mon is
   * put IN a box (home/move_mon.asm:421-427) and leaves it stale in the party; writing
   * it now means an edited party mon cannot reappear at its old level the moment it is
   * deposited. */
  e->rec[G1R_BOXLEVEL] = level;
  if (e->is_party) e->rec[G1R_LEVEL] = level;
  wr24be(e->rec + G1R_EXP, gen1_exp_for_level(si->growth, level));
  g1e_recalc_stats(e, si);
  return true;
}

/* ------------------------------------------------------------------------- */
/* Checksums                                                                  */
/* ------------------------------------------------------------------------- */

void gen1_write_fix_main_checksum(uint8_t* img) {
  Gen1Sum s;
  if (!img) return;
  gen1_sum_init(&s);
  gen1_sum_feed(&s, img + GEN1_SUM_FIRST, GEN1_SUM_LAST - GEN1_SUM_FIRST + 1u);
  img[GEN1_OFF_CHECKSUM] = gen1_sum_final(&s);
}

void gen1_write_fix_bank_checksums(uint8_t* img, int bank) {
  uint32_t base, sums;
  Gen1Sum whole;
  int b;
  if (!img || bank < 0 || bank > 1) return;
  base = bank ? GEN1_OFF_BANK3      : GEN1_OFF_BANK2;
  sums = bank ? GEN1_OFF_BANK3_SUMS : GEN1_OFF_BANK2_SUMS;
  gen1_sum_init(&whole);
  for (b = 0; b < 6; b++) {
    Gen1Sum one;
    const uint8_t* p = img + base + (uint32_t)b * GEN1_BOX_BYTES;
    gen1_sum_init(&one);
    gen1_sum_feed(&one,   p, GEN1_BOX_BYTES);
    gen1_sum_feed(&whole, p, GEN1_BOX_BYTES);
    img[sums + 1u + (uint32_t)b] = gen1_sum_final(&one);
  }
  img[sums] = gen1_sum_final(&whole);
}

/* ------------------------------------------------------------------------- */
/* Destinations                                                               */
/* ------------------------------------------------------------------------- */

static uint32_t bank_slot_offset(int box) {
  return (box < 6 ? GEN1_OFF_BANK2 : GEN1_OFF_BANK3) + (uint32_t)(box % 6) * GEN1_BOX_BYTES;
}

Gen1WStatus gen1_write_targets(const uint8_t* img, const Gen1Save* s, int box,
                               uint32_t out[GEN1_WRITE_MAX_TARGETS], int* n) {
  bool boxes_ready;
  if (!img || !s || !out || !n) return GEN1W_ERR_ARG;
  *n = 0;

  if (box == GEN1_PARTY_BOX) {                   /* the party has no second home */
    out[(*n)++] = GEN1_OFF_PARTY;
    return GEN1W_OK;
  }
  if (box < 0 || box >= GEN1_NUM_BOXES) return GEN1W_ERR_ARG;

  /* bit 7 of wCurrentBoxNum = BIT_HAS_CHANGED_BOXES (constants/ram_constants.asm:51).
   * While it is clear, no banked box has ever been erased and the player's first
   * CHANGE BOX will run EmptyAllSRAMBoxes over all twelve (engine/menus/save.asm:367). */
  boxes_ready = (img[GEN1_OFF_CURRENT_NO] & 0x80u) != 0;

  if (box == s->current_box) {
    /* Primary FIRST: the game loads the open box only from here (save.asm:100). */
    out[(*n)++] = GEN1_OFF_CURRENT_BOX;
    /* Mirror, when there is a real bank to mirror into. Retail leaves this slot marked
     * empty while the box is open, but Guy's Red.sav keeps it byte-identical to the
     * live copy, so a reader that trusts it exists in the wild. Writing both cannot
     * lose anything: the next CHANGE BOX overwrites it from the live copy anyway. */
    if (boxes_ready) out[(*n)++] = bank_slot_offset(box);
    return GEN1W_OK;
  }

  if (!boxes_ready) return GEN1W_ERR_VIRGIN;     /* the edit would be wiped, not saved */
  if (s->box_uninit & (uint16_t)(1u << box)) return GEN1W_ERR_UNINIT;
  out[(*n)++] = bank_slot_offset(box);
  return GEN1W_OK;
}

/* ------------------------------------------------------------------------- */
/* The structural gate                                                        */
/* ------------------------------------------------------------------------- */

Gen1WStatus gen1_blob_check(const uint8_t* blob, int box) {
  Gen1Layout L;
  int count, i;
  if (!blob) return GEN1W_ERR_ARG;
  gen1_write_layout(box, &L);

  count = blob[0];
  if (count > L.cap) return GEN1W_ERR_STRUCT;
  /* The terminator must sit at exactly `count`. One too high and the game reads a
   * record out of the OT-name array; one too low and a Pokemon disappears. */
  if (blob[1 + count] != GEN1_LIST_TERM) return GEN1W_ERR_STRUCT;
  for (i = 0; i < count; i++) {
    uint8_t sp = blob[1 + i];
    if (sp == GEN1_LIST_TERM) return GEN1W_ERR_STRUCT;   /* early end: mons vanish */
    if (sp == 0) return GEN1W_ERR_STRUCT;                /* 0 is not a species     */
  }
  /* Deliberately no check on species 1..190 or on record/list agreement for slots this
   * write did not author — a save may legitimately hold a MissingNo, and refusing to
   * edit a whole box because of one glitch mon would be useless. gen1_write_verify_op
   * is what guarantees those slots come through untouched. */
  return GEN1W_OK;
}

/* ------------------------------------------------------------------------- */
/* Blob edits — all four structures, together                                 */
/* ------------------------------------------------------------------------- */

static void put_slot(uint8_t* blob, const Gen1Layout* L, int slot, const Gen1EditMon* e) {
  blob[1 + slot] = e->rec[G1R_SPECIES];          /* the list byte IS the record's */
  memcpy(blob + L->rec_off  + (size_t)slot * (size_t)L->recsz, e->rec,  (size_t)L->recsz);
  memcpy(blob + L->ot_off   + (size_t)slot * GEN1_NAME_BYTES, e->ot,   GEN1_NAME_BYTES);
  memcpy(blob + L->nick_off + (size_t)slot * GEN1_NAME_BYTES, e->nick, GEN1_NAME_BYTES);
}

static Gen1WStatus check_mon(const Gen1Op* op) {
  if (!op->mon) return GEN1W_ERR_ARG;
  if (op->mon->is_party != (op->box == GEN1_PARTY_BOX)) return GEN1W_ERR_ARG;
  if (op->mon->rec[G1R_SPECIES] == 0 || op->mon->rec[G1R_SPECIES] > 190)
    return GEN1W_ERR_SPECIES;
  return GEN1W_OK;
}

Gen1WStatus gen1_blob_apply(uint8_t* blob, Gen1Op* op) {
  Gen1Layout L;
  Gen1WStatus st;
  int count;
  if (!blob || !op) return GEN1W_ERR_ARG;
  gen1_write_layout(op->box, &L);
  count = blob[0];
  if (count > L.cap) return GEN1W_ERR_STRUCT;

  switch (op->kind) {
    case GEN1_OP_REPLACE:
      if (op->slot < 0 || op->slot >= count) return GEN1W_ERR_EMPTY;
      st = check_mon(op);
      if (st != GEN1W_OK) return st;
      put_slot(blob, &L, op->slot, op->mon);
      return GEN1W_OK;

    case GEN1_OP_INSERT:
      if (count >= L.cap) return GEN1W_ERR_FULL;
      st = check_mon(op);
      if (st != GEN1W_OK) return st;
      /* Exactly _MoveMon's append (home/move_mon.asm:364-377): bump the count, write
       * the species where the terminator was, write the new terminator after it. */
      blob[0] = (uint8_t)(count + 1);
      blob[1 + count]     = op->mon->rec[G1R_SPECIES];
      blob[1 + count + 1] = GEN1_LIST_TERM;
      put_slot(blob, &L, count, op->mon);
      op->slot = count;
      return GEN1W_OK;

    case GEN1_OP_DELETE: {
      int slot = op->slot, i;
      bool term = false;
      if (slot < 0 || slot >= count) return GEN1W_ERR_EMPTY;

      /* _RemovePokemon, engine/pokemon/remove_mon.asm:1-107, structure by structure.
       *
       * Look for the terminator BEFORE touching a byte. Retail's shift loop is
       * unbounded and would run out of the species list and into the records; ours
       * refuses instead, and refusing after a half-finished edit would be worse than
       * not trying, so the read-only check comes first. */
      for (i = slot; i < L.cap; i++)
        if (blob[1 + i + 1] == GEN1_LIST_TERM) { term = true; break; }
      if (!term) return GEN1W_ERR_STRUCT;

      blob[0] = (uint8_t)(count - 1);

      /* 1. species list: shift up, copying THROUGH the terminator so it moves too
       *    (ibid.:19-24). */
      for (i = slot; i < L.cap; i++) {
        uint8_t v = blob[1 + i + 1];
        blob[1 + i] = v;
        if (v == GEN1_LIST_TERM) break;
      }

      if (slot < L.cap - 1) {
        size_t nslots = (size_t)(L.cap - 1 - slot);
        /* 2/3/4. records, OT names, nicknames: each array shifts up one slot, all the
         *        way to its own end — including the unused tail, exactly as retail's
         *        CopyDataUntil does (ibid.:57,85,107). */
        memmove(blob + L.rec_off  + (size_t)slot * (size_t)L.recsz,
                blob + L.rec_off  + (size_t)(slot + 1) * (size_t)L.recsz,
                nslots * (size_t)L.recsz);
        memmove(blob + L.ot_off   + (size_t)slot * GEN1_NAME_BYTES,
                blob + L.ot_off   + (size_t)(slot + 1) * GEN1_NAME_BYTES, nslots * GEN1_NAME_BYTES);
        memmove(blob + L.nick_off + (size_t)slot * GEN1_NAME_BYTES,
                blob + L.nick_off + (size_t)(slot + 1) * GEN1_NAME_BYTES, nslots * GEN1_NAME_BYTES);
      } else {
        /* Removing the very last slot: retail shifts nothing and stamps 0xFF over the
         * first byte of that OT field (ibid.:37-44 — its own comment calls the value a
         * bug, since 0x50 is the string terminator; it is harmless because the species
         * list is what marks a slot used). Reproduced so a delete leaves byte-for-byte
         * what the game would have left. */
        blob[(size_t)L.ot_off + (size_t)slot * GEN1_NAME_BYTES] = 0xFFu;
      }
      return GEN1W_OK;
    }
    default:
      return GEN1W_ERR_ARG;
  }
}

/* ------------------------------------------------------------------------- */
/* The semantic gate                                                          */
/* ------------------------------------------------------------------------- */

/* Written as index arithmetic over `before`, deliberately NOT sharing code with the
 * memmoves that produced `after`, so that a mistake in one is visible to the other. */
static bool slot_equal(const uint8_t* a, const uint8_t* b, const Gen1Layout* L,
                       int sa, int sb) {
  return memcmp(a + L->rec_off  + (size_t)sa * (size_t)L->recsz,
                b + L->rec_off  + (size_t)sb * (size_t)L->recsz, (size_t)L->recsz) == 0
      && memcmp(a + L->ot_off   + (size_t)sa * GEN1_NAME_BYTES,
                b + L->ot_off   + (size_t)sb * GEN1_NAME_BYTES, GEN1_NAME_BYTES) == 0
      && memcmp(a + L->nick_off + (size_t)sa * GEN1_NAME_BYTES,
                b + L->nick_off + (size_t)sb * GEN1_NAME_BYTES, GEN1_NAME_BYTES) == 0;
}

static bool slot_is_mon(const uint8_t* after, const Gen1Layout* L, int slot,
                        const Gen1EditMon* e) {
  return memcmp(after + L->rec_off  + (size_t)slot * (size_t)L->recsz, e->rec, (size_t)L->recsz) == 0
      && memcmp(after + L->ot_off   + (size_t)slot * GEN1_NAME_BYTES, e->ot,   GEN1_NAME_BYTES) == 0
      && memcmp(after + L->nick_off + (size_t)slot * GEN1_NAME_BYTES, e->nick, GEN1_NAME_BYTES) == 0;
}

bool gen1_write_verify_op(const uint8_t* before, const uint8_t* after,
                          int box, const Gen1Op* op) {
  Gen1Layout L;
  int oldc, newc, j, k, slot;
  if (!before || !after || !op) return false;
  gen1_write_layout(box, &L);
  oldc = before[0];
  newc = after[0];
  slot = op->slot;

  switch (op->kind) {
    case GEN1_OP_REPLACE:
      if (!op->mon || newc != oldc) return false;
      if (slot < 0 || slot >= oldc) return false;
      for (j = 0; j <= L.cap; j++) {
        uint8_t want = (j == slot) ? op->mon->rec[G1R_SPECIES] : before[1 + j];
        if (after[1 + j] != want) return false;
      }
      for (k = 0; k < L.cap; k++) {
        if (k == slot) { if (!slot_is_mon(after, &L, k, op->mon)) return false; }
        else           { if (!slot_equal(before, after, &L, k, k)) return false; }
      }
      return true;

    case GEN1_OP_INSERT:
      if (!op->mon || newc != oldc + 1) return false;
      if (slot != oldc || oldc >= L.cap) return false;
      for (j = 0; j <= L.cap; j++) {
        uint8_t want;
        if (j < oldc)          want = before[1 + j];
        else if (j == oldc)    want = op->mon->rec[G1R_SPECIES];
        else if (j == oldc + 1) want = GEN1_LIST_TERM;
        else                   want = before[1 + j];      /* tail untouched */
        if (after[1 + j] != want) return false;
      }
      for (k = 0; k < L.cap; k++) {
        if (k == oldc) { if (!slot_is_mon(after, &L, k, op->mon)) return false; }
        else           { if (!slot_equal(before, after, &L, k, k)) return false; }
      }
      return true;

    case GEN1_OP_DELETE:
      if (newc != oldc - 1 || oldc < 1) return false;
      if (slot < 0 || slot >= oldc) return false;
      for (j = 0; j <= L.cap; j++) {
        /* everything from `slot` up to and including the new terminator slid down one;
         * the tail past it was never written. */
        uint8_t want = (j < slot || j > newc) ? before[1 + j] : before[1 + j + 1];
        if (after[1 + j] != want) return false;
      }
      for (k = 0; k < L.cap; k++) {
        int src = (k < slot) ? k : (k <= L.cap - 2 ? k + 1 : L.cap - 1);
        if (slot == L.cap - 1 && k == L.cap - 1) {
          /* the last-slot quirk: only the first OT byte changed */
          const uint8_t* ao = after  + L.ot_off + (size_t)k * GEN1_NAME_BYTES;
          const uint8_t* bo = before + L.ot_off + (size_t)k * GEN1_NAME_BYTES;
          if (ao[0] != 0xFFu) return false;
          if (memcmp(ao + 1, bo + 1, GEN1_NAME_BYTES - 1) != 0) return false;
          if (memcmp(after  + L.rec_off  + (size_t)k * (size_t)L.recsz,
                     before + L.rec_off  + (size_t)k * (size_t)L.recsz,
                     (size_t)L.recsz) != 0) return false;
          if (memcmp(after  + L.nick_off + (size_t)k * GEN1_NAME_BYTES,
                     before + L.nick_off + (size_t)k * GEN1_NAME_BYTES, GEN1_NAME_BYTES) != 0) return false;
        } else if (!slot_equal(before, after, &L, src, k)) {
          return false;
        }
      }
      return true;

    default:
      return false;
  }
}

/* ------------------------------------------------------------------------- */
/* The destination gate                                                       */
/* ------------------------------------------------------------------------- */

Gen1WStatus gen1_write_verify_image_box(const uint8_t* img, uint32_t len,
                                        const Gen1Save* s, int box, const uint8_t* expect) {
  Gen1Save fresh;
  uint32_t targets[GEN1_WRITE_MAX_TARGETS];
  int n = 0, i;
  uint32_t bytes;
  Gen1WStatus st;

  if (!img || !expect) return GEN1W_ERR_ARG;
  if (len < GEN1_SAVE_SIZE) return GEN1W_ERR_SIZE;
  /* Reparse from scratch, with gen1_save.c's own parser — not with anything this file
   * believes about the layout. */
  if (gen1_open(img, len, &fresh) != GEN1_OK) return GEN1W_ERR_SAVE;
  if (s && s->current_box != fresh.current_box) return GEN1W_ERR_VERIFY;

  st = gen1_write_targets(img, &fresh, box, targets, &n);
  if (st != GEN1W_OK) return st;
  if (n < 1) return GEN1W_ERR_VERIFY;

  bytes = gen1_list_bytes(box);
  for (i = 0; i < n; i++)
    if (memcmp(img + targets[i], expect, bytes) != 0) return GEN1W_ERR_VERIFY;

  /* and the parser's own view of the occupancy must agree with the blob */
  if (gen1_count(&fresh, box) != gen1_list_count(expect, box)) return GEN1W_ERR_VERIFY;
  return GEN1W_OK;
}

/* ------------------------------------------------------------------------- */
/* The write                                                                  */
/* ------------------------------------------------------------------------- */

static void banks_touched(const uint32_t* t, int n, bool touched[2]) {
  int i;
  touched[0] = touched[1] = false;
  for (i = 0; i < n; i++) {
    if (t[i] >= GEN1_OFF_BANK3)      touched[1] = true;
    else if (t[i] >= GEN1_OFF_BANK2) touched[0] = true;
  }
}

Gen1WStatus gen1_write_apply(uint8_t* img, uint32_t len, Gen1Save* s,
                             Gen1Op* op, Gen1WriteScratch* scratch) {
  Gen1Save cur;
  Gen1Layout L;
  uint32_t targets[GEN1_WRITE_MAX_TARGETS];
  int n = 0, i;
  uint32_t bytes;
  Gen1WStatus st;
  bool touched[2];
  uint8_t save_main;
  uint8_t save_bank[2][7];

  if (!img || !s || !op || !scratch) return GEN1W_ERR_ARG;
  if (len < GEN1_SAVE_SIZE) return GEN1W_ERR_SIZE;
  /* Never write into a save we do not already understand. */
  if (gen1_open(img, len, &cur) != GEN1_OK) return GEN1W_ERR_SAVE;

  st = gen1_write_targets(img, &cur, op->box, targets, &n);
  if (st != GEN1W_OK) return st;
  /* The parser's authoritative READ offset must be the primary WRITE destination. If
   * these two ever drifted apart, every edit would be invisible to every reader. */
  if (n < 1 || targets[0] != gen1_list_offset(&cur, op->box)) return GEN1W_ERR_VERIFY;

  gen1_write_layout(op->box, &L);
  bytes = (uint32_t)L.bytes;

  memcpy(scratch->old, img + targets[0], bytes);
  st = gen1_blob_check(scratch->old, op->box);      /* build only on sound ground */
  if (st != GEN1W_OK) return st;
  memcpy(scratch->neu, scratch->old, bytes);

  st = gen1_blob_apply(scratch->neu, op);
  if (st != GEN1W_OK) return st;
  st = gen1_blob_check(scratch->neu, op->box);
  if (st != GEN1W_OK) return st;
  if (!gen1_write_verify_op(scratch->old, scratch->neu, op->box, op)) return GEN1W_ERR_VERIFY;

  /* A no-op writes NOTHING — not the blob, not the checksums. Anything else would make
   * "open the editor and press B" alter the file. */
  if (memcmp(scratch->old, scratch->neu, bytes) == 0) return GEN1W_OK;

  banks_touched(targets, n, touched);
  save_main = img[GEN1_OFF_CHECKSUM];
  memcpy(save_bank[0], img + GEN1_OFF_BANK2_SUMS, 7);
  memcpy(save_bank[1], img + GEN1_OFF_BANK3_SUMS, 7);

  for (i = 0; i < n; i++) memcpy(img + targets[i], scratch->neu, bytes);
  gen1_write_fix_main_checksum(img);
  if (touched[0]) gen1_write_fix_bank_checksums(img, 0);
  if (touched[1]) gen1_write_fix_bank_checksums(img, 1);

  /* --- prove it, from the image, at every destination --- */
  st = gen1_write_verify_image_box(img, len, &cur, op->box, scratch->neu);
  if (st == GEN1W_OK && op->kind != GEN1_OP_DELETE) {
    /* and the record must decode as a Pokemon through gen1_save.c's own decoder */
    Gen1Save chk;
    Gen1Mon m;
    if (gen1_open(img, len, &chk) != GEN1_OK) st = GEN1W_ERR_SAVE;
    else if (!gen1_decode_image(&chk, img, op->box, op->slot, &m)) st = GEN1W_ERR_VERIFY;
  }
  if (st == GEN1W_OK && gen1_open(img, len, &cur) != GEN1_OK) st = GEN1W_ERR_VERIFY;
  if (st != GEN1W_OK) {
    /* Put every byte back exactly as it was — including checksum bytes that were
     * already wrong before we touched them (ten of Red.sav's fourteen bank bytes are).
     * A refused write must be indistinguishable from no write at all. */
    for (i = 0; i < n; i++) memcpy(img + targets[i], scratch->old, bytes);
    img[GEN1_OFF_CHECKSUM] = save_main;
    memcpy(img + GEN1_OFF_BANK2_SUMS, save_bank[0], 7);
    memcpy(img + GEN1_OFF_BANK3_SUMS, save_bank[1], 7);
    return GEN1W_ERR_VERIFY;
  }
  *s = cur;                                    /* the handle now matches the image */
  return GEN1W_OK;
}

/* ------------------------------------------------------------------------- */
/* The generic HEADER patch (BACKLOG #49 P0)                                  */
/* ------------------------------------------------------------------------- */

static bool ranges_overlap(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) {
  return a0 <= b1 && b0 <= a1;
}

Gen1WStatus gen1_write_range_ex(uint8_t* img, uint32_t len, Gen1Save* s,
                                uint32_t off, const uint8_t* buf, uint32_t n,
                                uint8_t* snap, uint32_t snap_len) {
  Gen1Save cur, after;
  uint32_t last;
  uint8_t save_checksum;

  if (!img || !s || !buf || !n || !snap) return GEN1W_ERR_ARG;
  if (n > snap_len) return GEN1W_ERR_RANGE;   /* the caller's rollback buffer is the cap,
                                               * not a number this file invents (P0
                                               * review D6) */
  if (len < GEN1_SAVE_SIZE) return GEN1W_ERR_SIZE;
  if (off + n < off) return GEN1W_ERR_RANGE;             /* unsigned wraparound guard */
  last = off + n - 1u;

  /* Never write into a save we do not already understand — same rule gen1_write_apply
   * opens with; `s` is treated as OUTPUT, refreshed on success, never trusted as input. */
  if (gen1_open(img, len, &cur) != GEN1_OK) return GEN1W_ERR_SAVE;

  /* g2w_write_range's refusal set, translated (source/gen2_write.c write_patch /
   * g2w_write_range; docs/GEN12-PARITY-DESIGN.md §4.0). Confined to the main
   * checksummed block alone: that single bound already excludes both SRAM box banks
   * (0x4000.. / 0x6000..), so there is no separate per-box loop to keep in step with
   * gen1_write_targets the way gen2_write.c must for its 14 Gen-2 boxes. */
  if (off < GEN1_SUM_FIRST || last > GEN1_SUM_LAST) return GEN1W_ERR_RANGE;
  if (ranges_overlap(off, last, GEN1_OFF_PARTY, GEN1_OFF_PARTY + GEN1_PARTY_BYTES - 1u))
    return GEN1W_ERR_RANGE;
  if (ranges_overlap(off, last, GEN1_OFF_CURRENT_BOX, GEN1_OFF_CURRENT_BOX + GEN1_BOX_BYTES - 1u))
    return GEN1W_ERR_RANGE;
  if (ranges_overlap(off, last, GEN1_OFF_CURRENT_NO, GEN1_OFF_CURRENT_NO))
    return GEN1W_ERR_RANGE;

  /* A no-op writes NOTHING — not the bytes, not the checksum — matching
   * gen1_write_apply's own rule ("open the editor and press B" must not alter the file). */
  if (memcmp(img + off, buf, n) == 0) { *s = cur; return GEN1W_OK; }

  memcpy(snap, img + off, n);                  /* the rollback copy, in the CALLER's buffer */
  save_checksum = img[GEN1_OFF_CHECKSUM];

  memcpy(img + off, buf, n);
  gen1_write_fix_main_checksum(img);           /* Gen 1 has no backup mirror to fix   */

  if (gen1_open(img, len, &after) == GEN1_OK) {
    *s = after;                                /* the handle now matches the image   */
    return GEN1W_OK;
  }

  memcpy(img + off, snap, n);                  /* put every byte back, checksum too  */
  img[GEN1_OFF_CHECKSUM] = save_checksum;
  return GEN1W_ERR_VERIFY;
}

#ifndef __arm__   /* host-only: no GBA caller (gb_session.c uses _ex with its own scratch); keeping
                   * it in the image made the delta layout's address-taken sweep flag it as an
                   * unreached orphan (2026-09-16, BACKLOG #106 G2 class). tests/host_gen1write_test.c
                   * still exercises it on the host. */
Gen1WStatus gen1_write_range(uint8_t* img, uint32_t len, Gen1Save* s,
                             uint32_t off, const uint8_t* buf, uint32_t n) {
  /* The 64-byte convenience wrapper (P0 review D6): every field small enough that a
   * caller would rather not find its own rollback buffer. GEN1_WRITE_RANGE_MAX is
   * THIS wrapper's cap, not gen1_write_range_ex's — that one is capped by whatever
   * `snap_len` its caller hands it, which gb_session.c's gbs_write_field sizes to the
   * session's own scratch (1152 B, comfortably covering every field
   * source/gb_fields.c defines, up to GBF_EVENT_FLAGS_BASE's 320 B). */
  uint8_t snap[GEN1_WRITE_RANGE_MAX];
  /* n > GEN1_WRITE_RANGE_MAX is refused by _ex itself (n > snap_len); not re-checked
   * here to avoid a second copy of that comparison drifting out of step with it. */
  return gen1_write_range_ex(img, len, s, off, buf, n, snap, sizeof snap);
}
#endif /* !__arm__ */

/* ------------------------------------------------------------------------- */
/* The Hall-of-Fame allowlist (BACKLOG #89) -- see gen1_write.h for the contract   */
/* ------------------------------------------------------------------------- */

Gen1WStatus gen1_write_outside_sum(uint8_t* img, uint32_t len, Gen1Save* s,
                                   uint32_t off, const uint8_t* buf, uint32_t n,
                                   uint8_t* snap, uint32_t snap_len) {
  Gen1Save cur;
  uint32_t last, done;

  if (!img || !s || !buf || !n || !snap || !snap_len) return GEN1W_ERR_ARG;
  if (len < GEN1_SAVE_SIZE) return GEN1W_ERR_SIZE;
  if (off + n < off) return GEN1W_ERR_RANGE;             /* unsigned wraparound guard */
  last = off + n - 1u;

  /* The allowlist: exactly the HoF blob, nothing wider -- see the "NAMED ALLOWLIST,
   * not a widened bound" note in gen1_write.h. */
  if (off < GEN1_OFF_HOF || last >= GEN1_OFF_HOF + GEN1_HOF_BYTES) return GEN1W_ERR_RANGE;

  if (gen1_open(img, len, &cur) != GEN1_OK) return GEN1W_ERR_SAVE;

  for (done = 0; done < n; ) {
    uint32_t chunk = n - done;
    uint32_t coff  = off + done;
    Gen1Save after;

    if (chunk > snap_len) chunk = snap_len;

    /* A no-op chunk writes nothing at all, same rule as gen1_write_range_ex. */
    if (memcmp(img + coff, buf + done, chunk) == 0) {
      *s = cur;
      done += chunk;
      continue;
    }

    memcpy(snap, img + coff, chunk);          /* THIS chunk's rollback copy only */
    memcpy(img + coff, buf + done, chunk);

    if (gen1_open(img, len, &after) == GEN1_OK) {
      cur = after;
      *s = after;
      done += chunk;
      continue;
    }

    /* Restore just this chunk; chunks 0..done-1 already committed to `img` are the
     * caller's problem -- see the big comment in gen1_write.h. */
    memcpy(img + coff, snap, chunk);
    return GEN1W_ERR_VERIFY;
  }

  return GEN1W_OK;
}

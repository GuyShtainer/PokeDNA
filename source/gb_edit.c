#include "gb_edit.h"
#include "data_tables.h"   /* pk_species_growth / pk_exp_for_level / pk_base_stats / pk_move_pp */
#include <string.h>

/* Generation-1/2 record editor. Contract, sources and rationale live in gb_edit.h.
 *
 * Pure C: no tonc, no FatFs, no statics that are not `const`. Everything works on the
 * caller's GbEditMon, which is a stack-sized struct (~90 B) — the hardware build has
 * ~1.5 KB of free EWRAM and a post-link guard, so this file must not grow a buffer. */

/* --- big-endian helpers. A Game Boy save stores multi-byte fields high byte first,
 * which is the reverse of every gen3_* core in this tree, so they get their own names
 * rather than a shared one that would be easy to grab by mistake (the same reasoning as
 * gen1_save.c:9). --- */
static uint16_t rd16be(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t rd24be(const uint8_t* p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}
static void wr16be(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr24be(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v;
}

/* --- record field offsets ---------------------------------------------------
 * Gen 1, pokered/macros/ram.asm:7 (box_struct) + :28 (party_struct):
 *   00 Species | 01 HP(2) | 03 BoxLevel | 04 Status | 05 Type1 | 06 Type2 |
 *   07 CatchRate | 08 Moves(4) | 0C OTID(2) | 0E Exp(3) | 11 StatExp(5x2) |
 *   1B DVs(2) | 1D PP(4)  == 33 bytes;  party adds  21 Level | 22 Stats(5x2) == 44
 * Gen 2, pokecrystal/macros/ram.asm:7 + :32:
 *   00 Species | 01 Item | 02 Moves(4) | 06 ID(2) | 08 Exp(3) | 0B StatExp(5x2) |
 *   15 DVs(2) | 17 PP(4) | 1B Happiness | 1C Pokerus | 1D CaughtTime+Level |
 *   1E CaughtGender+Location | 1F Level  == 32 bytes;  party adds
 *   20 Status | 21 Unused | 22 HP(2) | 24 MaxHP,Atk,Def,Spd,SpA,SpD (6x2) == 48
 * The Gen-1 party's stat block starts with MaxHP (no separate current-HP entry: current
 * HP is the box_struct's own 0x01), which is why the two generations index it
 * differently below. */
enum {
  R1_SPECIES = 0x00, R1_CURHP = 0x01, R1_BOXLVL = 0x03, R1_STATUS = 0x04,
  R1_TYPE1 = 0x05, R1_TYPE2 = 0x06, R1_CATCH = 0x07, R1_MOVES = 0x08,
  R1_OTID = 0x0C, R1_EXP = 0x0E, R1_STATEXP = 0x11, R1_DVS = 0x1B, R1_PP = 0x1D,
  R1_LEVEL = 0x21, R1_STATS = 0x22
};
enum {
  R2_SPECIES = 0x00, R2_ITEM = 0x01, R2_MOVES = 0x02, R2_OTID = 0x06, R2_EXP = 0x08,
  R2_STATEXP = 0x0B, R2_DVS = 0x15, R2_PP = 0x17, R2_HAPPY = 0x1B, R2_PKRS = 0x1C,
  R2_CAUGHT0 = 0x1D, R2_CAUGHT1 = 0x1E, R2_LEVEL = 0x1F,
  R2_STATUS = 0x20, R2_CURHP = 0x22, R2_STATS = 0x24
};

#define PP_MASK     0x3Fu   /* pokered/constants/pokemon_data_constants.asm:102 */
#define PP_UP_SHIFT 6       /* PP_UP_MASK %11000000, same file :101            */

#define NAME_TERM   0x50u   /* charmap "@", $50 — string terminator, both charmaps */

static bool gen_ok(uint8_t g) { return g == GB_GEN1 || g == GB_GEN2; }

/* =========================================================================== */
/* Species numbering                                                            */
/* =========================================================================== */

uint8_t gb_max_species(uint8_t gen) { return gen == GB_GEN1 ? 151u : 251u; }
uint8_t gb_max_move(uint8_t gen)    { return gen == GB_GEN1 ? 165u : 251u; }

uint16_t gb_dex_from_index(uint8_t gen, uint8_t raw) {
  if (gen == GB_GEN1) return gen1_dex_from_index(raw);      /* the verified 190-entry map */
  return (raw >= 1 && raw <= G2_SPECIES_MAX) ? raw : 0;     /* Gen 2 indexes BY dex number */
}

/* The inverse of gen1_dex_from_index. Deliberately a SCAN of that table rather than a
 * second table: the 151 non-zero entries are a bijection onto dex 1..151 (gen1_save.c:45
 * documents the property and the diff against pokered/data/pokemon/dex_order.asm that
 * proved it), so exactly one index matches and a second copy could only ever disagree
 * with the first. 190 byte compares, run once per species edit — not a hot path. */
uint8_t gb_index_from_dex(uint8_t gen, uint16_t dex) {
  if (gen == GB_GEN2) return (dex >= 1 && dex <= G2_SPECIES_MAX) ? (uint8_t)dex : 0;
  if (gen != GB_GEN1 || dex < 1 || dex > 151) return 0;
  for (int i = 1; i <= 190; i++)
    if (gen1_dex_from_index((uint8_t)i) == dex) return (uint8_t)i;
  return 0;
}

/* =========================================================================== */
/* Growth rate / experience                                                     */
/* =========================================================================== */

/* GROWTH RATES ARE SHARED WITH GEN 3 — verified, not assumed.
 *
 * Three things had to be true before pk_species_growth/pk_exp_for_level could be reused
 * for a Game Boy mon, and all three were checked against the decomps in assets/upstream
 * rather than taken on faith:
 *
 *  1. the growth-rate ASSIGNMENT per species. Diffing pokered's per-species base_stats files against
 *     pokecrystal's against pokefirered's species_info.h gives ZERO differences over all
 *     251 species (Mew included);
 *  2. the growth-rate FORMULA. pokered/data/growth_rates.asm and
 *     pokecrystal/data/growth_rates.asm are byte-identical, and evaluating their
 *     `a/b*n^3 + c*n^2 + d*n - e` for levels 2..100 on the four curves any species
 *     actually uses reproduces pk_exp_for_level() exactly, 0 mismatches out of 396;
 *  3. the two curves whose IDS DIFFER between the generations are unused. GB id 1 and 2
 *     are "Slightly Fast"/"Slightly Slow" where Gen 3 has Erratic/Fluctuating — but no
 *     Gen-1 or Gen-2 species is assigned 1 or 2 (the tally is Medium Fast 119, Medium
 *     Slow 64, Slow 45, Fast 23 = 251), so the clash cannot be reached from a real save.
 *
 * Level 1 is the one place they differ: the GB formula underflows for Medium Slow
 * (1.2 - 15 + 100 - 140 < 0) and Gen 3 hard-codes the entry. pk_exp_for_level returns 0
 * for level <= 1, which is what the GB's own CalcLevelFromExperience
 * (pokered/engine/pokemon/experience.asm:2) reports back for 0 exp, so the pair stays
 * self-consistent. */
uint8_t gb_growth_rate(uint16_t dex) {
  if (dex < 1 || dex > G2_SPECIES_MAX) return 0;
  return pk_species_growth(dex);          /* Gen-3 internal index == dex for 1..251 */
}
uint32_t gb_exp_for_level(uint16_t dex, uint8_t level) {
  if (dex < 1 || dex > G2_SPECIES_MAX || level > 100) return 0;
  return pk_exp_for_level(gb_growth_rate(dex), level);
}
uint8_t gb_level_from_exp(uint16_t dex, uint32_t exp) {
  if (dex < 1 || dex > G2_SPECIES_MAX) return 1;
  return pk_level_from_exp(gb_growth_rate(dex), exp);
}

/* =========================================================================== */
/* Moves / PP                                                                   */
/* =========================================================================== */

/* Move IDS and BASE PP are shared with Gen 3, with exactly one exception — and the same
 * exception is the one place the two Game Boy generations disagree with EACH OTHER, so
 * this cannot be a single number for both. Parsing all 165 entries of
 * pokered/data/moves/moves.asm and all 251 of pokecrystal/data/moves/moves.asm and
 * comparing the PP column id by id gives exactly one difference over the shared range:
 *
 *   165 STRUGGLE   pokered/data/moves/moves.asm:178      10 PP
 *                  pokecrystal/data/moves/moves.asm:181   1 PP   (== Gen 3, == pk_move_pp)
 *
 * Struggle is a battle-only fallback that no save stores, but a hacked record can hold
 * it, and a wrong maximum here is a wrong pp_over verdict in gb_check().
 *
 * A move id past the generation's own maximum has no PP at all rather than Gen 3's: a
 * Gen-1 record holding move 200 is holding garbage, not Sludge Bomb. */
uint8_t gb_move_base_pp(uint8_t gen, uint8_t move) {
  if (!gen_ok(gen) || move == 0 || move > gb_max_move(gen)) return 0;
  if (move == 165) return (gen == GB_GEN1) ? 10u : 1u;
  return pk_move_pp(move);
}

/* max PP = base + ups * min(base/5, 7).
 * The min() is not a rounding detail: a 40-PP move would reach 64 with three PP Ups and
 * overflow the 6-bit PP field, so both games cap the per-Up bonus at 7, which is where
 * the famous maximum of 61 comes from. pokered/engine/items/item_effects.asm:2418
 * (AddBonusPP, "cap the amount at 7") and pokecrystal/.../item_effects.asm:2752
 * (ComputeMaxPP, same comment spelled out). */
uint8_t gb_max_pp(uint8_t gen, uint8_t move, uint8_t ups) {
  uint8_t base = gb_move_base_pp(gen, move);
  uint8_t step;
  if (base == 0) return 0;
  if (ups > 3) ups = 3;
  step = (uint8_t)(base / 5u);
  if (step > 7) step = 7;
  return (uint8_t)(base + ups * step);
}

/* =========================================================================== */
/* List geometry                                                                */
/* =========================================================================== */

bool gb_box_is_party(uint8_t gen, int box) {
  return gen == GB_GEN1 ? (box == GEN1_PARTY_BOX) : (box == G2_BOX_PARTY);
}
bool gb_box_valid(uint8_t gen, int box) {
  if (gen == GB_GEN1) return box >= 0 && box <= GEN1_PARTY_BOX;
  if (gen == GB_GEN2) return box >= 0 && box <= G2_BOX_PARTY;
  return false;
}
int gb_list_capacity(uint8_t gen, int box) {
  if (!gb_box_valid(gen, box)) return -1;
  return gen == GB_GEN1 ? gen1_list_capacity(box) : g2_list_capacity(box);
}
int gb_list_size(uint8_t gen, int box) {
  if (!gb_box_valid(gen, box)) return -1;
  return gen == GB_GEN1 ? (int)gen1_list_bytes(box) : g2_list_size(box);
}
int gb_rec_size(uint8_t gen, bool is_party) {
  if (gen == GB_GEN1) return is_party ? GEN1_PARTY_REC_BYTES : GEN1_BOX_REC_BYTES;
  if (gen == GB_GEN2) return is_party ? G2_PARTY_ENTRY : G2_BOX_ENTRY;
  return -1;
}

/* Where the species list starts. Gen 1 puts the count byte first and the list straight
 * after it (gen1_save.c:208-211); Gen 2 exports the same number as g2_off_species_area. */
static int sp_area(uint8_t gen, int box) {
  if (!gb_box_valid(gen, box)) return -1;
  return gen == GB_GEN1 ? 1 : g2_off_species_area(box);
}

/* Gen 1's list offsets are computed here from the PUBLIC geometry constants in
 * gen1_save.h — the same arithmetic gen1_decode does at gen1_save.c:208-211, which that
 * file keeps private. Gen 2 already exports its own (g2_off_*), so those are called
 * rather than repeated. A slot outside the capacity returns -1 instead of an address. */
static int g1_off(int box, int slot, int which) {
  int cap   = gen1_list_capacity(box);
  int recsz = gb_rec_size(GB_GEN1, box == GEN1_PARTY_BOX);
  int rec_off, ot_off;
  if (slot < 0 || slot >= cap) return -1;
  rec_off = 1 + (cap + 1);
  ot_off  = rec_off + cap * recsz;
  switch (which) {
    case 0: return 1 + slot;                                  /* species list      */
    case 1: return rec_off + slot * recsz;                    /* record            */
    case 2: return ot_off  + slot * GEN1_NAME_BYTES;          /* OT name           */
    default:return ot_off  + cap * GEN1_NAME_BYTES + slot * GEN1_NAME_BYTES;
  }
}
static int g2_slot_ok(int box, int slot) {
  return slot >= 0 && slot < g2_list_capacity(box);
}
int gb_off_species(uint8_t gen, int box, int slot) {
  if (!gb_box_valid(gen, box)) return -1;
  if (gen == GB_GEN1) return g1_off(box, slot, 0);
  return g2_slot_ok(box, slot) ? g2_off_species_area(box) + slot : -1;
}
int gb_off_record(uint8_t gen, int box, int slot) {
  if (!gb_box_valid(gen, box)) return -1;
  if (gen == GB_GEN1) return g1_off(box, slot, 1);
  return g2_slot_ok(box, slot) ? g2_off_record(box, slot) : -1;
}
int gb_off_otname(uint8_t gen, int box, int slot) {
  if (!gb_box_valid(gen, box)) return -1;
  if (gen == GB_GEN1) return g1_off(box, slot, 2);
  return g2_slot_ok(box, slot) ? g2_off_otname(box, slot) : -1;
}
int gb_off_nickname(uint8_t gen, int box, int slot) {
  if (!gb_box_valid(gen, box)) return -1;
  if (gen == GB_GEN1) return g1_off(box, slot, 3);
  return g2_slot_ok(box, slot) ? g2_off_nickname(box, slot) : -1;
}

/* How many slots are occupied, from the blob's own count byte.
 *
 * Gen 2 defers to g2_list_count, which additionally REQUIRES the 0xFF terminator to sit
 * exactly at index `count` (gen2_save.c:334) and returns -1 otherwise — a Gen-2 box whose
 * count and terminator disagree is not a box the shipping reader will open at all, so it
 * is not a box this module will write into either.
 *
 * Gen 1's own reader only clamps (gen1_save.c:192), because an uninitialised bank-2/3 box
 * holds power-up SRAM noise and must read as EMPTY rather than as a rejected save. A
 * count above the capacity is still refused here: it cannot name an occupied slot. */
int gb_list_count(uint8_t gen, const uint8_t* list, int box) {
  int cap;
  if (!list || !gen_ok(gen)) return -1;
  cap = gb_list_capacity(gen, box);
  if (cap < 0) return -1;
  if (gen == GB_GEN2) return g2_list_count(list, box);
  return list[0] > cap ? -1 : (int)list[0];
}

int gb_off_terminator(uint8_t gen, const uint8_t* list, int box) {
  int n = gb_list_count(gen, list, box);
  int a = sp_area(gen, box);
  /* NOT gb_off_species(gen, box, n): for a FULL box the terminator sits one past the
   * last slot, which that function correctly refuses as an out-of-range slot. */
  return (n < 0 || a < 0) ? -1 : a + n;
}

/* =========================================================================== */
/* Load / commit                                                                */
/* =========================================================================== */

bool gb_load_parts(GbEditMon* e, uint8_t gen, bool is_party, const uint8_t* rec,
                   const uint8_t* otname, const uint8_t* nick, uint8_t list_species) {
  if (!e) return false;
  memset(e, 0, sizeof *e);
  if (!gen_ok(gen)) return false;
  e->gen      = gen;
  e->is_party = is_party;
  e->rec_len  = (uint8_t)gb_rec_size(gen, is_party);
  e->list_species = list_species;
  if (rec)    memcpy(e->rec, rec, e->rec_len);
  if (otname) memcpy(e->otname, otname, GB_NAME_BYTES);
  if (nick)   memcpy(e->nick,   nick,   GB_NAME_BYTES);
  return true;
}

/* Returns false — having written NOTHING, including the list_species out-parameter —
 * rather than half-filling the caller's buffers. The silent version left `*list_species`
 * whatever the caller's stack happened to hold, which arm-none-eabi-gcc spotted as a
 * -Wmaybe-uninitialized read in gb_roundtrip_ok. */
bool gb_commit_parts(const GbEditMon* e, uint8_t* rec, uint8_t* otname, uint8_t* nick,
                     uint8_t* list_species) {
  if (!e || !gen_ok(e->gen)) return false;
  if (rec)    memcpy(rec, e->rec, e->rec_len);
  if (otname) memcpy(otname, e->otname, GB_NAME_BYTES);
  if (nick)   memcpy(nick,   e->nick,   GB_NAME_BYTES);
  if (list_species) *list_species = e->list_species;
  return true;
}

bool gb_load(GbEditMon* e, uint8_t gen, const uint8_t* list, int box, int slot) {
  int sp, ro, ot, nk;
  if (!e) return false;
  memset(e, 0, sizeof *e);
  if (!list || !gen_ok(gen)) return false;
  sp = gb_off_species(gen, box, slot);
  ro = gb_off_record(gen, box, slot);
  ot = gb_off_otname(gen, box, slot);
  nk = gb_off_nickname(gen, box, slot);
  if (sp < 0 || ro < 0 || ot < 0 || nk < 0) return false;
  return gb_load_parts(e, gen, gb_box_is_party(gen, box),
                       list + ro, list + ot, list + nk, list[sp]);
}

bool gb_commit(const GbEditMon* e, uint8_t* list, int box, int slot) {
  int sp, ro, ot, nk, n;
  if (!e || !list || !gen_ok(e->gen)) return false;
  if (gb_box_is_party(e->gen, box) != e->is_party) return false;   /* wrong record kind */

  /* THE TERMINATOR. The species list is  count | s0 .. s(count-1) | 0xFF | free space,
   * so the byte at index `count` is not the first free slot's species — it IS the 0xFF
   * terminator, and for any box that is not full the append slot and the terminator are
   * the same byte. Writing a species there leaves the count saying N while the list
   * describes N+1 or more: Gen 2's own reader then refuses the whole box
   * (gen2_save.c:334 returns -1), and the damage survives every checksum.
   *
   * Both games grow a list as one indivisible move instead — bump the count, write the
   * species where the terminator was, put the terminator back one slot along
   * (pokered/engine/pokemon/add_mon.asm:12-28; pokecrystal/engine/pokemon/move_mon.asm:
   * 14-35, whose comment reads "The terminator is usually here, but it'll be back").
   * That is array surgery, which gb_edit.h hands to gen1_write.c / gen2_write.c. This
   * function patches a slot that is ALREADY occupied, and refuses anything else outright
   * rather than half-performing an append. Refusing writes nothing. */
  n = gb_list_count(e->gen, list, box);
  if (n < 0 || slot < 0 || slot >= n) return false;

  /* And the same byte from the other direction: 0xFF is the terminator's own value, so
   * putting one into an OCCUPIED slot truncates the box for the game's own walker. No
   * setter here can produce it (raw species are <= 251), but gb_load_parts takes the
   * list byte from the caller, so the guard belongs at the write. */
  if (e->list_species == GB_LIST_TERMINATOR) return false;

  sp = gb_off_species(e->gen, box, slot);
  ro = gb_off_record(e->gen, box, slot);
  ot = gb_off_otname(e->gen, box, slot);
  nk = gb_off_nickname(e->gen, box, slot);
  if (sp < 0 || ro < 0 || ot < 0 || nk < 0) return false;
  return gb_commit_parts(e, list + ro, list + ot, list + nk, list + sp);
}

bool gb_roundtrip_ok(uint8_t gen, const uint8_t* list, int box, int slot) {
  GbEditMon e;
  uint8_t rec[GB_MAX_REC], ot[GB_NAME_BYTES], nk[GB_NAME_BYTES], sp;
  int ro, oo, no, so;
  if (!gb_load(&e, gen, list, box, slot)) return false;
  /* Guarding the call is what makes `sp` provably written before the compare below —
   * gb_commit_parts's failure path writes none of the four outputs. */
  if (!gb_commit_parts(&e, rec, ot, nk, &sp)) return false;
  ro = gb_off_record(gen, box, slot);
  oo = gb_off_otname(gen, box, slot);
  no = gb_off_nickname(gen, box, slot);
  so = gb_off_species(gen, box, slot);
  if (ro < 0 || oo < 0 || no < 0 || so < 0) return false;
  return memcmp(rec, list + ro, e.rec_len) == 0
      && memcmp(ot,  list + oo, GB_NAME_BYTES) == 0
      && memcmp(nk,  list + no, GB_NAME_BYTES) == 0
      && sp == list[so];
}

/* =========================================================================== */
/* Getters                                                                      */
/* =========================================================================== */

uint8_t gb_get_species_raw(const GbEditMon* e) {
  return (e && gen_ok(e->gen)) ? e->rec[e->gen == GB_GEN1 ? R1_SPECIES : R2_SPECIES] : 0;
}
uint16_t gb_get_species_dex(const GbEditMon* e) {
  return (e && gen_ok(e->gen)) ? gb_dex_from_index(e->gen, gb_get_species_raw(e)) : 0;
}
/* Gen 1 keeps TWO level bytes: 0x03 is the level as of the last deposit and 0x21 is the
 * live one, and only the box copy's 0x03 is kept current (gen1_save.c:222). Read the one
 * that is authoritative for this record's kind. */
uint8_t gb_get_level(const GbEditMon* e) {
  if (!e || !gen_ok(e->gen)) return 0;
  if (e->gen == GB_GEN2) return e->rec[R2_LEVEL];
  return e->is_party ? e->rec[R1_LEVEL] : e->rec[R1_BOXLVL];
}
uint32_t gb_get_exp(const GbEditMon* e) {
  if (!e || !gen_ok(e->gen)) return 0;
  return rd24be(e->rec + (e->gen == GB_GEN1 ? R1_EXP : R2_EXP));
}
static int dvs_off(const GbEditMon* e) { return e->gen == GB_GEN1 ? R1_DVS : R2_DVS; }
static void dv4_of(const GbEditMon* e, uint8_t out[4]) {
  const uint8_t* d = e->rec + dvs_off(e);
  out[0] = (uint8_t)(d[0] >> 4);    /* Attack  */
  out[1] = (uint8_t)(d[0] & 0x0F);  /* Defense */
  out[2] = (uint8_t)(d[1] >> 4);    /* Speed   */
  out[3] = (uint8_t)(d[1] & 0x0F);  /* Special */
}
uint8_t gb_hp_dv(const uint8_t dv4[4]) { return g2_hp_dv(dv4); }   /* one spelling, gen2_save.c:435 */

uint8_t gb_get_dv(const GbEditMon* e, int stat) {
  uint8_t dv[4];
  if (!e || !gen_ok(e->gen) || stat < 0 || stat >= GB_NSTATS) return 0;
  dv4_of(e, dv);
  return stat == GB_HP ? gb_hp_dv(dv) : dv[stat - 1];
}
uint16_t gb_get_statexp(const GbEditMon* e, int stat) {
  if (!e || !gen_ok(e->gen) || stat < 0 || stat >= GB_NSTATS) return 0;
  return rd16be(e->rec + (e->gen == GB_GEN1 ? R1_STATEXP : R2_STATEXP) + stat * 2);
}
static int moves_off(const GbEditMon* e) { return e->gen == GB_GEN1 ? R1_MOVES : R2_MOVES; }
static int pp_off(const GbEditMon* e)    { return e->gen == GB_GEN1 ? R1_PP    : R2_PP; }
uint8_t gb_get_move(const GbEditMon* e, int i) {
  return (e && gen_ok(e->gen) && i >= 0 && i < 4) ? e->rec[moves_off(e) + i] : 0;
}
uint8_t gb_get_pp(const GbEditMon* e, int i) {
  return (e && gen_ok(e->gen) && i >= 0 && i < 4) ? (uint8_t)(e->rec[pp_off(e) + i] & PP_MASK) : 0;
}
uint8_t gb_get_ppup(const GbEditMon* e, int i) {
  return (e && gen_ok(e->gen) && i >= 0 && i < 4) ? (uint8_t)(e->rec[pp_off(e) + i] >> PP_UP_SHIFT) : 0;
}
uint16_t gb_get_otid(const GbEditMon* e) {
  if (!e || !gen_ok(e->gen)) return 0;
  return rd16be(e->rec + (e->gen == GB_GEN1 ? R1_OTID : R2_OTID));
}
uint8_t gb_get_held_item(const GbEditMon* e) {
  return (e && e->gen == GB_GEN2) ? e->rec[R2_ITEM] : 0;
}
uint16_t gb_get_stat(const GbEditMon* e, int i) {
  int n;
  if (!e || !gen_ok(e->gen) || !e->is_party || i < 0) return 0;
  n = (e->gen == GB_GEN1) ? 5 : 6;
  if (i >= n) return 0;
  return rd16be(e->rec + (e->gen == GB_GEN1 ? R1_STATS : R2_STATS) + i * 2);
}
uint16_t gb_get_current_hp(const GbEditMon* e) {
  if (!e || !gen_ok(e->gen) || !e->is_party) return 0;
  return rd16be(e->rec + (e->gen == GB_GEN1 ? R1_CURHP : R2_CURHP));
}
uint8_t gb_get_gen1_type1(const GbEditMon* e) {
  return (e && e->gen == GB_GEN1) ? e->rec[R1_TYPE1] : 0;
}
uint8_t gb_get_gen1_type2(const GbEditMon* e) {
  return (e && e->gen == GB_GEN1) ? e->rec[R1_TYPE2] : 0;
}
uint8_t gb_get_pokerus(const GbEditMon* e) {
  return (e && e->gen == GB_GEN2) ? e->rec[R2_PKRS] : 0;
}
uint8_t gb_get_gen1_status(const GbEditMon* e) {
  return (e && e->gen == GB_GEN1) ? e->rec[R1_STATUS] : 0;
}
bool gb_is_egg(const GbEditMon* e) {
  return e && e->gen == GB_GEN2 && e->list_species == G2_LIST_EGG;
}
int gb_get_nickname(const GbEditMon* e, char* out, int cap) {
  if (!e || !out || cap <= 0) return 0;
  return gb_name_decode(e->gen, out, cap, e->nick, GB_NAME_BYTES);
}
int gb_get_otname(const GbEditMon* e, char* out, int cap) {
  if (!e || !out || cap <= 0) return 0;
  return gb_name_decode(e->gen, out, cap, e->otname, GB_NAME_BYTES);
}

/* =========================================================================== */
/* Stats                                                                        */
/* =========================================================================== */

/* ceil(sqrt(v)), capped at 255 — GetSquareRoot, pokecrystal/engine/math/get_square_root.asm:3
 * ("the index of the first value in a table of squares that isn't lower than de", with an
 * early return once the index reaches 255). Gen 1 computes the same value inline at
 * pokered/home/move_mon.asm:54 (.statExpLoop, which also bails at $FF). Both start the
 * counter at 1, so sqrt(0) comes out as 1 — harmless, since the result is then >>2. */
static uint8_t gb_ceil_sqrt(uint16_t v) {
  uint32_t b = 0;
  while (b < 255u) { b++; if (b * b >= (uint32_t)v) break; }
  return (uint8_t)b;
}

/* Carry current HP across a change of maximum HP the way the game does when a mon's max
 * changes under it (pokered/engine/pokemon/evos_moves.asm:186-203 on evolution, which
 * adds the max-HP delta to the current HP rather than refilling). A fainted mon stays
 * fainted; anything else lands in 1..new_max. The editor must never hand out a free heal
 * and must never leave current HP above the maximum. */
static void carry_hp(uint8_t* p, uint16_t old_max, uint16_t new_max) {
  int32_t cur = (int32_t)rd16be(p);
  /* old_max == 0 cannot be a real Pokemon — the HP formula floors at level+10 — so it
   * means these stats are being computed for the FIRST time (a record built from
   * nothing). The game fills current HP from the fresh maximum in exactly that case:
   * pokered/engine/pokemon/add_mon.asm:131, "calc HP stat (set cur Hp to max HP)". */
  if (old_max == 0) { wr16be(p, new_max); return; }
  if (cur == 0) return;                                  /* fainted stays fainted */
  cur += (int32_t)new_max - (int32_t)old_max;
  if (cur < 1) cur = 1;
  if (cur > (int32_t)new_max) cur = (int32_t)new_max;
  wr16be(p, (uint16_t)cur);
}

/* stat = ((base + DV) * 2 + ceil(sqrt(statexp))/4) * level / 100  + (HP ? level+10 : 5)
 * capped at MAX_STAT_VALUE. Identical in both generations: pokered/home/move_mon.asm:54
 * (CalcStat, ".calcStatFromIV" onward) and pokecrystal/engine/pokemon/move_mon.asm:1424
 * (CalcMonStatC, ".GotDV" onward, STAT_MIN_NORMAL 5 / STAT_MIN_HP 10 / MAX_STAT_VALUE 999). */
static uint16_t gb_calc_stat(uint8_t base, uint8_t dv, uint16_t statexp,
                             uint8_t level, bool is_hp) {
  uint32_t bonus = (uint32_t)(gb_ceil_sqrt(statexp) >> 2);
  uint32_t v = (((uint32_t)base + dv) * 2u + bonus) * (uint32_t)level / 100u;
  v += is_hp ? ((uint32_t)level + 10u) : 5u;
  if (v > GB_MAX_STAT) v = GB_MAX_STAT;
  return (uint16_t)v;
}

bool gb_recalc_stats(GbEditMon* e) {
  uint8_t dv[4], hp_dv, level;
  uint16_t dex;
  if (!e || !gen_ok(e->gen)) return false;
  if (!e->is_party) { e->stats_stale = false; return true; }  /* box records store none */

  dex   = gb_get_species_dex(e);
  level = gb_get_level(e);
  if (dex == 0 || level < 1 || level > 100) return false;     /* nothing honest to compute */
  dv4_of(e, dv);
  hp_dv = gb_hp_dv(dv);

  if (e->gen == GB_GEN2) {
    /* pk_base_stats gives HP,Atk,Def,Spe,SpA,SpD — the exact order and the exact values
     * pokecrystal has for all 251 species (diffed; zero differences). Note SpA and SpD
     * are both driven by the SPECIAL DV and the SPECIAL stat exp: Gen 2 split the stat
     * but not its inputs (move_mon.asm:1424, the .not_spdef rewind). */
    uint8_t base[6];
    uint16_t maxhp, old_max = rd16be(e->rec + R2_STATS);
    pk_base_stats(dex, base);
    maxhp = gb_calc_stat(base[0], hp_dv, gb_get_statexp(e, GB_HP), level, true);
    wr16be(e->rec + R2_STATS + 0, maxhp);
    wr16be(e->rec + R2_STATS + 2, gb_calc_stat(base[1], dv[0], gb_get_statexp(e, GB_ATK), level, false));
    wr16be(e->rec + R2_STATS + 4, gb_calc_stat(base[2], dv[1], gb_get_statexp(e, GB_DEF), level, false));
    wr16be(e->rec + R2_STATS + 6, gb_calc_stat(base[3], dv[2], gb_get_statexp(e, GB_SPE), level, false));
    wr16be(e->rec + R2_STATS + 8, gb_calc_stat(base[4], dv[3], gb_get_statexp(e, GB_SPC), level, false));
    wr16be(e->rec + R2_STATS + 10,gb_calc_stat(base[5], dv[3], gb_get_statexp(e, GB_SPC), level, false));
    carry_hp(e->rec + R2_CURHP, old_max, maxhp);
  } else {
    const uint8_t* b;
    uint16_t maxhp, old_max = rd16be(e->rec + R1_STATS);
    if (!e->have_g1base) return false;   /* see GbGen1Base — Gen 3's table cannot stand in */
    b = e->g1base.base;
    maxhp = gb_calc_stat(b[GB_HP], hp_dv, gb_get_statexp(e, GB_HP), level, true);
    wr16be(e->rec + R1_STATS + 0, maxhp);
    wr16be(e->rec + R1_STATS + 2, gb_calc_stat(b[GB_ATK], dv[0], gb_get_statexp(e, GB_ATK), level, false));
    wr16be(e->rec + R1_STATS + 4, gb_calc_stat(b[GB_DEF], dv[1], gb_get_statexp(e, GB_DEF), level, false));
    wr16be(e->rec + R1_STATS + 6, gb_calc_stat(b[GB_SPE], dv[2], gb_get_statexp(e, GB_SPE), level, false));
    wr16be(e->rec + R1_STATS + 8, gb_calc_stat(b[GB_SPC], dv[3], gb_get_statexp(e, GB_SPC), level, false));
    carry_hp(e->rec + R1_CURHP, old_max, maxhp);
  }
  e->stats_stale = false;
  return true;
}

/* =========================================================================== */
/* Derived DV properties                                                        */
/* =========================================================================== */

void gb_dv_effects(const uint8_t dv4[4], uint16_t dex, uint8_t gender_ratio, GbDvEffects* out) {
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!dv4) return;
  /* All four rules already live in gen2_save.c (:435-461) and are reused rather than
   * re-derived — a second copy could only ever drift from the first. */
  out->hp_dv        = g2_hp_dv(dv4);
  out->shiny        = g2_dv_shiny(dv4);
  out->gender       = g2_gender_from_dv(dv4[0], gender_ratio);
  out->unown_letter = g2_unown_letter(dv4);
  out->unown        = (dex == 201);
}

void gb_dv_effects_of(const GbEditMon* e, GbDvEffects* out) {
  uint8_t dv[4];
  uint16_t dex;
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!e || !gen_ok(e->gen)) return;
  dv4_of(e, dv);
  dex = gb_get_species_dex(e);
  gb_dv_effects(dv, dex, dex ? pk_species_gender_ratio(dex) : 0xFFu, out);
}

bool gb_preview_dv(const GbEditMon* e, int stat, uint8_t v, GbDvEffects* out) {
  uint8_t dv[4];
  uint16_t dex;
  if (!out) return false;
  memset(out, 0, sizeof *out);
  if (!e || !gen_ok(e->gen) || stat <= GB_HP || stat >= GB_NSTATS || v > 15) return false;
  dv4_of(e, dv);
  dv[stat - 1] = v;
  dex = gb_get_species_dex(e);
  gb_dv_effects(dv, dex, dex ? pk_species_gender_ratio(dex) : 0xFFu, out);
  return true;
}

/* =========================================================================== */
/* Text                                                                         */
/* =========================================================================== */

/* THE ENCODER AND THE DECODER BELOW ARE ONE PAIR AND MUST STAY EXACT INVERSES.
 * host_gbedit_test.c proves it the only way that means anything: all 254 non-terminator
 * byte values, both generations, decode -> encode back to themselves.
 *
 * NOT a per-character map: several GB glyphs are TWO ASCII characters ("PK", "MN", and
 * the 'd/'l/'m/'r/'s/'t/'v contractions), the gender signs are three UTF-8 bytes, and a
 * byte with no text spelling at all is a four-character "{XX}" escape. So each direction
 * consumes/produces a variable number of characters for exactly one GB byte.
 *
 * The two generations diverge above 0xB9 and the divergence is real, not cosmetic:
 * Gen 1 puts the contractions at 0xBB-0xBF plus 0xE4/0xE5 and é at 0xBA, where Gen 2 puts
 * the contractions at 0xD0-0xD6, é at 0xEA, the umlauts at 0xC0-0xC5 and '&' at 0xE9
 * (pokered/constants/charmap.asm vs pokecrystal/constants/charmap.asm). Writing a Gen-2
 * byte into a Gen-1 name would render as katakana on a real cartridge.
 *
 * Encoding a glyph the target generation has no byte for still becomes a space, the
 * policy gen3_encode_char already uses — never a silently invented letter. */

static int hexval(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

/* The reversible escape, "{5D}" — exactly 4 characters, which is why GB_GLYPH_MAX is 5.
 * The GB charset has no braces, so an escape can never collide with a name a player
 * could type. Writes 4 characters and does NOT terminate; the caller does. */
static void hex_escape(uint8_t c, char* out) {
  static const char k_hex[] = "0123456789ABCDEF";
  out[0] = '{';
  out[1] = k_hex[(c >> 4) & 0x0Fu];
  out[2] = k_hex[c & 0x0Fu];
  out[3] = '}';
}

/* The encoder proper. `lost` reports that this glyph could NOT be stored exactly — it was
 * replaced by a space or transliterated onto a lookalike — which is the difference between
 * "the player typed a space" and "the player typed something this cartridge cannot hold".
 * gb_char_encode throws the flag away; gb_text_lossy() is what reads it, so that
 * gb_set_nickname can refuse instead of quietly dropping a character (see NAMES). */
static int enc_one(uint8_t gen, const char* s, uint8_t* out, bool* lost) {
  const unsigned char* p = (const unsigned char*)s;
  uint8_t b;
  int used = 1;
  *lost = false;
  if (!p || !out || !*p) return 0;

  /* "{XX}" — the reversible escape for a byte with no text spelling. Refused for the
   * 0x50 terminator alone: planting one mid-name would truncate the field and orphan
   * every byte after it. Everything else, including 0x00, round-trips. */
  if (p[0] == '{' && p[1] && p[2] && p[3] == '}') {
    int hi = hexval(p[1]), lo = hexval(p[2]);
    if (hi >= 0 && lo >= 0) {
      uint8_t v = (uint8_t)((hi << 4) | lo);
      if (v != NAME_TERM) { *out = v; return 4; }
    }
  }
  /* three-byte UTF-8 first: the gender signs, which both generations share */
  if (p[0] == 0xE2u && p[1] == 0x99u && (p[2] == 0x80u || p[2] == 0x82u)) {
    *out = (p[2] == 0x80u) ? 0xF5u : 0xEFu;      /* U+2640 ♀ / U+2642 ♂ */
    return 3;
  }
  /* two-byte UTF-8: é and × in both, and Gen 2's umlauts */
  if (p[0] == 0xC3u) {
    switch (p[1]) {
      case 0xA9u: *out = (gen == GB_GEN1) ? 0xBAu : 0xEAu; return 2;   /* é */
      case 0x97u: *out = 0xF1u; return 2;                              /* U+00D7 × */
      /* Gen 1 has no umlaut tiles, so it transliterates onto the plain letter. That is a
       * real loss of what the player typed — flagged, never hidden. */
      case 0x84u: *lost = gen != GB_GEN2; *out = (gen == GB_GEN2) ? 0xC0u : 0x80u; return 2; /* Ä -> 'A' */
      case 0x96u: *lost = gen != GB_GEN2; *out = (gen == GB_GEN2) ? 0xC1u : 0x8Eu; return 2; /* Ö -> 'O' */
      case 0x9Cu: *lost = gen != GB_GEN2; *out = (gen == GB_GEN2) ? 0xC2u : 0x94u; return 2; /* Ü -> 'U' */
      case 0xA4u: *lost = gen != GB_GEN2; *out = (gen == GB_GEN2) ? 0xC3u : 0xA0u; return 2; /* ä -> 'a' */
      case 0xB6u: *lost = gen != GB_GEN2; *out = (gen == GB_GEN2) ? 0xC4u : 0xAEu; return 2; /* ö -> 'o' */
      case 0xBCu: *lost = gen != GB_GEN2; *out = (gen == GB_GEN2) ? 0xC5u : 0xB4u; return 2; /* ü -> 'u' */
      default: break;
    }
  }
  /* two ASCII characters that are one glyph */
  if (p[0] == 'P' && p[1] == 'K') { *out = 0xE1u; return 2; }
  if (p[0] == 'M' && p[1] == 'N') { *out = 0xE2u; return 2; }
  if (p[0] == '\'') {
    /* LOWERCASE ONLY. Real cartridge data proves it matters: FARFETCH'D in Guy's Gold
     * save is stored 85 80 91 85 84 93 82 87 E0 83 — a bare apostrophe (0xE0) then an
     * uppercase D, because the combined glyph is "'d" with a lowercase d. */
    int hit = 1;
    switch (p[1]) {
      case 'd': b = (gen == GB_GEN1) ? 0xBBu : 0xD0u; break;
      case 'l': b = (gen == GB_GEN1) ? 0xBCu : 0xD1u; break;
      case 'm': b = (gen == GB_GEN1) ? 0xE5u : 0xD2u; break;
      case 'r': b = (gen == GB_GEN1) ? 0xE4u : 0xD3u; break;
      case 's': b = (gen == GB_GEN1) ? 0xBDu : 0xD4u; break;
      case 't': b = (gen == GB_GEN1) ? 0xBEu : 0xD5u; break;
      case 'v': b = (gen == GB_GEN1) ? 0xBFu : 0xD6u; break;
      default:  hit = 0; b = 0; break;
    }
    if (hit) { *out = b; return 2; }
  }
  /* one ASCII character */
  if (p[0] >= 0x80u) {                            /* unmapped non-ASCII -> space */
    b = 0x7Fu;
    used = 1;
    while (((unsigned char)p[used] & 0xC0u) == 0x80u) used++;   /* eat continuations */
    *out = b;
    *lost = true;
    return used;
  }
  if (p[0] >= 'A' && p[0] <= 'Z')      b = (uint8_t)(0x80u + (p[0] - 'A'));
  else if (p[0] >= 'a' && p[0] <= 'z') b = (uint8_t)(0xA0u + (p[0] - 'a'));
  else if (p[0] >= '0' && p[0] <= '9') b = (uint8_t)(0xF6u + (p[0] - '0'));
  else switch (p[0]) {
    case ' ':  b = 0x7Fu; break;
    case '(':  b = 0x9Au; break;
    case ')':  b = 0x9Bu; break;
    case ':':  b = 0x9Cu; break;
    case ';':  b = 0x9Du; break;
    case '[':  b = 0x9Eu; break;
    case ']':  b = 0x9Fu; break;
    case '\'': b = 0xE0u; break;
    case '-':  b = 0xE3u; break;
    case '?':  b = 0xE6u; break;
    case '!':  b = 0xE7u; break;
    case '.':  b = 0xE8u; break;
    case '/':  b = 0xF3u; break;
    case ',':  b = 0xF4u; break;
    /* Gen 2 only: '&' is katakana ァ in Gen 1, so it must not be written there. */
    case '&':  b = (gen == GB_GEN2) ? 0xE9u : 0x7Fu; *lost = gen != GB_GEN2; break;
    /* Both charmaps call 0xF0 the Poke Dollar; g2_decode_text spells it "$", so accept
     * that back and keep one spelling across the tree. */
    case '$':  b = 0xF0u; break;
    /* No GB glyph -> a space, never an invented letter. A literal space is the one case
     * that is NOT a loss; everything else reaching here really did fail to encode. */
    default:   b = 0x7Fu; *lost = p[0] != ' '; break;
  }
  *out = b;
  return used;
}

int gb_char_encode(uint8_t gen, const char* s, uint8_t* out) {
  bool lost;
  return enc_one(gen, s, out, &lost);
}

/* How many glyphs of `s` this generation cannot store exactly, looking only at the first
 * `max_glyphs` (anything past the cap is truncated away and is a separate, documented
 * loss). `first_bad`, when given, receives the offending input glyph as NUL-terminated
 * text so the UI can name it. 0 means every glyph has a byte. */
int gb_text_lossy(uint8_t gen, const char* s, int max_glyphs, char first_bad[GB_GLYPH_MAX]) {
  int n = 0, bad = 0;
  if (first_bad) first_bad[0] = 0;
  if (!gen_ok(gen) || !s) return 0;
  while (n < max_glyphs) {
    uint8_t b;
    bool lost = false;
    int used = enc_one(gen, s, &b, &lost);
    if (used == 0) break;
    if (lost) {
      if (bad == 0 && first_bad) {
        int k = used < GB_GLYPH_MAX - 1 ? used : GB_GLYPH_MAX - 1;
        memcpy(first_bad, s, (size_t)k);
        first_bad[k] = 0;
      }
      bad++;
    }
    s += used;
    n++;
  }
  return bad;
}

/* One GB byte -> its text. The inverse of gb_char_encode, glyph for glyph. Returns the
 * number of characters written (out is NUL-terminated either way); 0 means the byte was
 * the 0x50 terminator and decoding must stop.
 *
 * Unlike gen1_char_ascii (which flattens é to "e" and everything unmapped to "?") and
 * g2_glyph (which flattens <PK>/<MN>/é/×/the umlauts to a space or a lookalike), nothing
 * here is allowed to collide with another byte's spelling: whatever a name holds, the UI
 * can show it and hand it straight back without the record losing a glyph. */
int gb_char_decode(uint8_t gen, uint8_t c, char out[GB_GLYPH_MAX]) {
  const char* g = 0;
  int n = 0;
  if (!out) return 0;
  out[0] = 0;
  if (c == NAME_TERM) return 0;

  if (c >= 0x80u && c <= 0x99u)      { out[0] = (char)('A' + (c - 0x80u)); n = 1; }
  else if (c >= 0xA0u && c <= 0xB9u) { out[0] = (char)('a' + (c - 0xA0u)); n = 1; }
  else if (c >= 0xF6u)               { out[0] = (char)('0' + (c - 0xF6u)); n = 1; }
  else switch (c) {
    case 0x7Fu: g = " ";  break;
    case 0x9Au: g = "(";  break;
    case 0x9Bu: g = ")";  break;
    case 0x9Cu: g = ":";  break;
    case 0x9Du: g = ";";  break;
    case 0x9Eu: g = "[";  break;
    case 0x9Fu: g = "]";  break;
    case 0xE0u: g = "'";  break;
    case 0xE1u: g = "PK"; break;   /* charmap "<PK>", $e1 — in BOTH decomps */
    case 0xE2u: g = "MN"; break;   /* charmap "<MN>", $e2 */
    case 0xE3u: g = "-";  break;
    case 0xE6u: g = "?";  break;
    case 0xE7u: g = "!";  break;
    case 0xE8u: g = ".";  break;
    case 0xEFu: g = "\xE2\x99\x82"; break;   /* ♂, spelled as data_tables.c spells it */
    case 0xF0u: g = "$";  break;             /* ¥ / Poke Dollar */
    case 0xF1u: g = "\xC3\x97"; break;       /* × U+00D7 — NOT the letter x (0xB7) */
    case 0xF3u: g = "/";  break;
    case 0xF4u: g = ",";  break;
    case 0xF5u: g = "\xE2\x99\x80"; break;   /* ♀ */
    default:    break;
  }
  /* 0xF2 is <DOT>, the decimal point — a DIFFERENT tile from 0xE8 "." that both charmaps
   * annotate "same as '.' in English". Spelling it "." too would make the pair
   * ambiguous and silently rewrite it to 0xE8, so it takes the escape below. */
  if (!g && n == 0) {
    if (gen == GB_GEN1) {
      switch (c) {
        case 0xBAu: g = "\xC3\xA9"; break;   /* é */
        case 0xBBu: g = "'d"; break;
        case 0xBCu: g = "'l"; break;
        case 0xBDu: g = "'s"; break;
        case 0xBEu: g = "'t"; break;
        case 0xBFu: g = "'v"; break;
        case 0xE4u: g = "'r"; break;
        case 0xE5u: g = "'m"; break;
        default: break;
      }
    } else {
      switch (c) {
        case 0xC0u: g = "\xC3\x84"; break;   /* Ä */
        case 0xC1u: g = "\xC3\x96"; break;   /* Ö */
        case 0xC2u: g = "\xC3\x9C"; break;   /* Ü */
        case 0xC3u: g = "\xC3\xA4"; break;   /* ä */
        case 0xC4u: g = "\xC3\xB6"; break;   /* ö */
        case 0xC5u: g = "\xC3\xBC"; break;   /* ü */
        case 0xD0u: g = "'d"; break;
        case 0xD1u: g = "'l"; break;
        case 0xD2u: g = "'m"; break;
        case 0xD3u: g = "'r"; break;
        case 0xD4u: g = "'s"; break;
        case 0xD5u: g = "'t"; break;
        case 0xD6u: g = "'v"; break;
        case 0xE9u: g = "&";  break;
        case 0xEAu: g = "\xC3\xA9"; break;   /* é */
        default: break;
      }
    }
  }
  if (g) { n = (int)strlen(g); memcpy(out, g, (size_t)n); }
  else if (n == 0) {
    /* No text spelling in this generation — control codes, the other generation's
     * glyphs, the arrows, the decimal point. "{5D}" rather than "?" or " ", because
     * 0x5D is <TRAINER>, the OT of every in-game trade in Gen 1, and a UI that shows a
     * name must be able to hand it back unchanged. */
    hex_escape(c, out);
    n = 4;
  }
  out[n] = 0;
  return n;
}

/* Would these two characters, side by side, be read back as ONE glyph? That is the whole
 * failure mode gb_name_decode has to defend against: seven GB glyphs spell themselves with
 * TWO ASCII characters ("PK", "MN", "'d"...), and those same two characters can equally be
 * two separate one-character glyphs. Rather than hard-code the list — which would rot the
 * moment a spelling changed — ask the encoder itself: if it swallows both characters for a
 * single byte, the pair merges. */
static bool pair_merges(uint8_t gen, char a, char b) {
  char probe[3];
  uint8_t tmp;
  probe[0] = a; probe[1] = b; probe[2] = 0;
  return gb_char_encode(gen, probe, &tmp) == 2;
}

/* PER-BYTE REVERSIBILITY IS NOT ENOUGH, AND THIS IS WHERE THAT BITES.
 *
 * gb_char_decode/gb_char_encode are exact inverses for every single byte, but a NAME is a
 * sequence, and the seven two-ASCII-character spellings can also be produced by two
 * one-character glyphs standing next to each other. Both readings decode to the same text
 * and the encoder can only pick one, so the other is destroyed:
 *
 *     'P' 'K'   0x8F 0x8A  -> "PK"  -> 0xE1        (<PK>, one glyph)   2 bytes become 1
 *     'M' 'N'   0x8C 0x8D  -> "MN"  -> 0xE2        (<MN>)
 *     '\'' 'd'  0xE0 0xA3  -> "'d"  -> 0xBB / 0xD0 (the contraction)   ... and six more
 *
 * WHICH SIDE GETS THE ESCAPE IS DECIDED BY THE REAL DATA, not by symmetry. The <PK> glyph
 * is genuinely common — Guy's Gold.sav has the OT "Mattia<PK>" on 645 of his 1810 name
 * fields — while a literal 'P' immediately followed by 'K' appears zero times in the whole
 * corpus. So <PK> keeps the readable spelling "PK" and it is the RARE literal pair that
 * gets escaped: 0x8F 0x8A decodes to "P{8A}". Common names stay legible; the reversal is
 * total either way.
 *
 * The rule is general rather than a hard-coded list: emit a glyph, and if its first
 * character would merge with the single character just emitted, escape THIS byte instead.
 * An escape ends in '}' and multi-character spellings end in 'K'/'N'/a letter, none of
 * which open a pair, so one character of lookback is provably sufficient. */
int gb_name_decode(uint8_t gen, char* out, int cap, const uint8_t* src, int nbytes) {
  int n = 0, i;
  char prev = 0;              /* the previous glyph, iff it was a lone ASCII character */
  if (!out || cap <= 0) return 0;
  out[0] = 0;
  if (!src) return 0;
  for (i = 0; i < nbytes; i++) {
    char g[GB_GLYPH_MAX];
    int w = gb_char_decode(gen, src[i], g);
    if (w == 0) break;                /* 0x50 terminator */
    if (prev && pair_merges(gen, prev, g[0])) {
      hex_escape(src[i], g);          /* break the pair, reversibly */
      g[4] = 0;
      w = 4;
    }
    if (n + w >= cap) break;          /* truncate on a glyph boundary, never split UTF-8 */
    memcpy(out + n, g, (size_t)w);
    n += w;
    prev = (w == 1) ? g[0] : 0;       /* only a lone character can open a pair */
  }
  out[n] = 0;
  return n;
}

int gb_name_encode(uint8_t gen, uint8_t* dst, int cap, int max_glyphs, const char* s) {
  int n = 0;
  if (!dst || cap <= 0) return 0;
  if (s) {
    while (n < cap - 1 && n < max_glyphs) {
      uint8_t b;
      int used = gb_char_encode(gen, s, &b);
      if (used == 0) break;
      dst[n++] = b;
      s += used;
    }
  }
  /* Terminator + padding. The games write the name then "@" and leave whatever was in
   * the rest of the field, so real saves carry junk past the terminator (Guy's Red.sav
   * has "KINGLER" 50 50 00 00 and "FLAREON" 50 09 02 00). Everything — the cartridge's
   * own text engine, gen1_decode_name, g2_decode_text, gb_name_decode — stops at 0x50,
   * so filling the tail is safe and leaves no stale bytes to puzzle over later. The tail
   * of a name the player did NOT change is preserved a level up, in gb_set_nickname. */
  for (; n < cap; n++) dst[n] = NAME_TERM;
  return n;
}

/* =========================================================================== */
/* Setters                                                                      */
/* =========================================================================== */

bool gb_set_gen1_base(GbEditMon* e, const GbGen1Base* b) {
  if (!e || e->gen != GB_GEN1 || !b) return false;
  e->g1base = *b;
  e->have_g1base = true;
  e->rec[R1_TYPE1] = b->type1;
  e->rec[R1_TYPE2] = b->type2;
  e->stats_stale = e->is_party;
  return true;
}

/* The catch-rate byte is never written implicitly — see the note above GbGen1Base in the
 * header. Evolution leaves it alone (pokered/engine/pokemon/evos_moves.asm:160-215) and
 * after a Time Capsule trade it holds a Gen-2 held item, so only an explicit call moves it. */
bool gb_set_gen1_catch_rate(GbEditMon* e, uint8_t v) {
  if (!e || e->gen != GB_GEN1) return false;
  e->rec[R1_CATCH] = v;
  return true;
}

/* Set the EXP that belongs with `level` for `dex`, without touching the level byte(s). */
static void set_exp_for(GbEditMon* e, uint16_t dex, uint8_t level) {
  wr24be(e->rec + (e->gen == GB_GEN1 ? R1_EXP : R2_EXP), gb_exp_for_level(dex, level));
}

bool gb_set_level(GbEditMon* e, uint8_t level) {
  uint16_t dex;
  if (!e || !gen_ok(e->gen) || level < 1 || level > 100) return false;
  dex = gb_get_species_dex(e);
  if (dex == 0) return false;            /* a glitch species has no growth rate to use */
  set_exp_for(e, dex, level);
  if (e->gen == GB_GEN2) {
    e->rec[R2_LEVEL] = level;
  } else {
    /* Write BOTH Gen-1 level bytes. 0x21 is the live one; 0x03 is the box level, which
     * the game itself overwrites with 0x21 the moment the mon is deposited
     * (pokered/engine/pokemon/add_mon.asm:427, "de = BoxLevel"). Leaving 0x03 stale would
     * mean the PC list showed the old level until the next deposit. */
    e->rec[R1_BOXLVL] = level;
    if (e->is_party) e->rec[R1_LEVEL] = level;
  }
  e->stats_stale = e->is_party;
  return true;
}

bool gb_set_exp(GbEditMon* e, uint32_t exp) {
  uint16_t dex;
  uint32_t cap;
  uint8_t level;
  if (!e || !gen_ok(e->gen)) return false;
  dex = gb_get_species_dex(e);
  if (dex == 0) return false;
  cap = gb_exp_for_level(dex, 100);
  if (exp > cap) exp = cap;
  wr24be(e->rec + (e->gen == GB_GEN1 ? R1_EXP : R2_EXP), exp);
  level = gb_level_from_exp(dex, exp);   /* keep the pair consistent, always */
  if (e->gen == GB_GEN2) e->rec[R2_LEVEL] = level;
  else { e->rec[R1_BOXLVL] = level; if (e->is_party) e->rec[R1_LEVEL] = level; }
  e->stats_stale = e->is_party;
  return true;
}

bool gb_set_species(GbEditMon* e, uint16_t dex, const GbGen1Base* newbase) {
  uint8_t raw, level;
  if (!e || !gen_ok(e->gen)) return false;
  if (dex < 1 || dex > gb_max_species(e->gen)) return false;
  raw = gb_index_from_dex(e->gen, dex);
  if (raw == 0) return false;                       /* no representation in this gen */
  if (e->gen == GB_GEN1 && !newbase) return false;  /* types/catch rate are not optional */

  level = gb_get_level(e);
  e->rec[e->gen == GB_GEN1 ? R1_SPECIES : R2_SPECIES] = raw;
  /* The species-LIST byte is the menus' copy and has to move with the record. A Gen-2
   * Egg keeps 0xFD there while the record carries the real species (gen2_save.c:519). */
  if (!(e->gen == GB_GEN2 && e->list_species == G2_LIST_EGG)) e->list_species = raw;

  if (e->gen == GB_GEN1) {
    /* Types move with the species because the GAME moves them on evolution
     * (evos_moves.asm:214 -> SetPartyMonTypes). The catch-rate byte does NOT, for the two
     * reasons documented above GbGen1Base — leaving it alone is the faithful behaviour. */
    e->g1base = *newbase;
    e->have_g1base = true;
    e->rec[R1_TYPE1] = newbase->type1;
    e->rec[R1_TYPE2] = newbase->type2;
  }
  /* Keep the LEVEL and move the EXP: the record stores both, and the new species may sit
   * on a different curve, so leaving the EXP alone would silently change the level. */
  if (level >= 1 && level <= 100) set_exp_for(e, dex, level);
  e->stats_stale = e->is_party;
  return true;
}

bool gb_set_move(GbEditMon* e, int i, uint8_t move) {
  if (!e || !gen_ok(e->gen) || i < 0 || i > 3) return false;
  if (move > gb_max_move(e->gen)) return false;
  e->rec[moves_off(e) + i] = move;
  /* PP Ups belong to the MOVE, not to the slot: learning a new move loses them, which is
   * what gen3_edit.c's em_set_move does for the same reason. Full PP for the new move —
   * and a plain 0 byte when the slot is being emptied, which is what all 908 empty move
   * slots in Guy's five real saves hold. */
  e->rec[pp_off(e) + i] = (uint8_t)(gb_move_base_pp(e->gen, move) & PP_MASK);
  return true;
}

bool gb_set_pp(GbEditMon* e, int i, uint8_t pp) {
  uint8_t ups, max;
  if (!e || !gen_ok(e->gen) || i < 0 || i > 3) return false;
  if (gb_get_move(e, i) == 0) return false;   /* no move, no PP — see the header */
  ups = gb_get_ppup(e, i);
  max = gb_max_pp(e->gen, gb_get_move(e, i), ups);
  if (pp > max) pp = max;
  e->rec[pp_off(e) + i] = (uint8_t)(((uint32_t)ups << PP_UP_SHIFT) | (pp & PP_MASK));
  return true;
}

bool gb_set_ppup(GbEditMon* e, int i, uint8_t ups) {
  uint8_t pp, max;
  if (!e || !gen_ok(e->gen) || i < 0 || i > 3 || ups > 3) return false;
  /* An empty slot has no maximum (gb_max_pp is 0 there), so the old code stored ups<<6
   * on top of nothing and produced a PP byte — 0xC0 for three Ups — that no cartridge
   * ever writes. A PP Up is bought for a move; with no move there is nothing to buy it
   * for. Clear the slot with gb_set_move(e, i, 0) instead. */
  if (gb_get_move(e, i) == 0) return false;
  pp  = gb_get_pp(e, i);
  max = gb_max_pp(e->gen, gb_get_move(e, i), ups);
  if (pp > max) pp = max;              /* removing PP Ups must not leave PP above max */
  e->rec[pp_off(e) + i] = (uint8_t)(((uint32_t)ups << PP_UP_SHIFT) | (pp & PP_MASK));
  return true;
}

bool gb_set_dv(GbEditMon* e, int stat, uint8_t v) {
  uint8_t* d;
  if (!e || !gen_ok(e->gen) || v > 15) return false;
  /* GB_HP is refused on purpose: no HP DV is stored anywhere. It is the low bits of the
   * other four (pokered/home/move_mon.asm:54, the "get HP IV" branch), so a setter that
   * appeared to accept it would be lying about what the save can hold. */
  if (stat <= GB_HP || stat >= GB_NSTATS) return false;
  d = e->rec + dvs_off(e);
  switch (stat) {
    case GB_ATK: d[0] = (uint8_t)((d[0] & 0x0Fu) | ((uint32_t)v << 4)); break;
    case GB_DEF: d[0] = (uint8_t)((d[0] & 0xF0u) | v);        break;
    case GB_SPE: d[1] = (uint8_t)((d[1] & 0x0Fu) | ((uint32_t)v << 4)); break;
    default:     d[1] = (uint8_t)((d[1] & 0xF0u) | v);        break;   /* GB_SPC */
  }
  e->stats_stale = e->is_party;
  return true;
}

bool gb_set_statexp(GbEditMon* e, int stat, uint16_t v) {
  if (!e || !gen_ok(e->gen) || stat < 0 || stat >= GB_NSTATS) return false;
  wr16be(e->rec + (e->gen == GB_GEN1 ? R1_STATEXP : R2_STATEXP) + stat * 2, v);
  e->stats_stale = e->is_party;
  return true;
}

void gb_set_otid(GbEditMon* e, uint16_t id) {
  if (!e || !gen_ok(e->gen)) return;
  wr16be(e->rec + (e->gen == GB_GEN1 ? R1_OTID : R2_OTID), id);
}

/* Is `s` exactly what this field already says? If so the field must not be rewritten —
 * see NAMES in the header. This is the mechanism that keeps the junk past the 0x50
 * terminator (733 of the 2738 name fields in Guy's saves carry some) through the
 * get -> show -> confirm cycle a UI performs on every single edit, whether or not the
 * player touched the name at all. */
static bool field_text_is(uint8_t gen, const uint8_t* field, const char* s) {
  char cur[GB_TEXT_MAX];
  gb_name_decode(gen, cur, (int)sizeof cur, field, GB_NAME_BYTES);
  return strcmp(cur, s) == 0;
}

/* THE UNCHANGED-TEXT FAST PATH COMES FIRST, BEFORE THE LOSSY CHECK, and it has to: it is
 * what makes reading a name and handing it straight back a guaranteed no-op. Everything
 * gb_name_decode produces is encodable by construction, so the check below can only ever
 * fire on characters the PLAYER typed — never on a name that was already in the save. */
bool gb_set_nickname(GbEditMon* e, const char* s) {
  if (!e || !gen_ok(e->gen) || !s) return false;
  if (field_text_is(e->gen, e->nick, s)) return true;      /* unchanged: touch nothing */
  if (gb_text_lossy(e->gen, s, GB_NICK_GLYPHS, 0) > 0) return false;
  gb_name_encode(e->gen, e->nick, GB_NAME_BYTES, GB_NICK_GLYPHS, s);
  return true;
}
bool gb_set_otname(GbEditMon* e, const char* s) {
  if (!e || !gen_ok(e->gen) || !s) return false;
  if (field_text_is(e->gen, e->otname, s)) return true;    /* unchanged: touch nothing */
  if (gb_text_lossy(e->gen, s, GB_OT_GLYPHS, 0) > 0) return false;
  gb_name_encode(e->gen, e->otname, GB_NAME_BYTES, GB_OT_GLYPHS, s);
  return true;
}
/* The knowing-caller versions: substitute and write anyway. For a UI that has already
 * shown the player what gb_text_lossy() reported and had them accept it. */
bool gb_set_nickname_lossy(GbEditMon* e, const char* s) {
  if (!e || !gen_ok(e->gen) || !s) return false;
  if (field_text_is(e->gen, e->nick, s)) return true;
  gb_name_encode(e->gen, e->nick, GB_NAME_BYTES, GB_NICK_GLYPHS, s);
  return true;
}
bool gb_set_otname_lossy(GbEditMon* e, const char* s) {
  if (!e || !gen_ok(e->gen) || !s) return false;
  if (field_text_is(e->gen, e->otname, s)) return true;
  gb_name_encode(e->gen, e->otname, GB_NAME_BYTES, GB_OT_GLYPHS, s);
  return true;
}
void gb_set_nickname_raw(GbEditMon* e, const uint8_t b[GB_NAME_BYTES]) {
  if (e && b) memcpy(e->nick, b, GB_NAME_BYTES);
}
void gb_set_otname_raw(GbEditMon* e, const uint8_t b[GB_NAME_BYTES]) {
  if (e && b) memcpy(e->otname, b, GB_NAME_BYTES);
}

bool gb_set_held_item(GbEditMon* e, uint8_t item) {
  if (!e || e->gen != GB_GEN2) return false;   /* Gen-1 +0x01 is current HP, not an item */
  e->rec[R2_ITEM] = item;
  return true;
}
uint8_t gb_get_friendship(const GbEditMon* e) {
  return (e && e->gen == GB_GEN2) ? e->rec[R2_HAPPY] : 0;
}
bool gb_set_friendship(GbEditMon* e, uint8_t f) {
  if (!e || e->gen != GB_GEN2) return false;
  e->rec[R2_HAPPY] = f;
  return true;
}
bool gb_set_pokerus(GbEditMon* e, uint8_t p) {
  if (!e || e->gen != GB_GEN2) return false;
  e->rec[R2_PKRS] = p;
  return true;
}
bool gb_set_caught(GbEditMon* e, uint8_t time, uint8_t level, uint8_t loc, uint8_t ot_gender) {
  if (!e || e->gen != GB_GEN2) return false;
  if (time > 3 || level > 63 || loc > 127 || ot_gender > 1) return false;
  e->rec[R2_CAUGHT0] = (uint8_t)((time << 6) | level);
  e->rec[R2_CAUGHT1] = (uint8_t)((ot_gender << 7) | loc);
  return true;
}
bool gb_set_egg(GbEditMon* e, bool egg) {
  if (!e || e->gen != GB_GEN2) return false;   /* Gen 1 has no eggs at all */
  if (egg) {
    e->list_species = G2_LIST_EGG;
  } else if (e->list_species == G2_LIST_EGG) {
    e->list_species = e->rec[R2_SPECIES];      /* the record kept the real species */
  }
  return true;
}

/* =========================================================================== */
/* Structural gate                                                              */
/* =========================================================================== */

/* Which type ids may legally sit in a Gen-1 record.
 *
 * Gen 1's own set is 0x00-0x08 and 0x14-0x1A (pokered/constants/type_constants.asm; 0x06
 * BIRD exists but no species has it). The set accepted here is WIDER, 0x00-0x09 and
 * 0x14-0x1B, because Gen 2 added STEEL at 0x09 and DARK at 0x1B
 * (pokecrystal/constants/type_constants.asm) and a Time Capsule trade drops a Gen-2
 * Pokemon into a Gen-1 save with its Gen-2 typing intact.
 *
 * That is not theory. Guy's Red.sav holds three such mons — box 2 slot 4 and box 10
 * slot 6 (Magneton) and box 6 slot 7 (Magnemite) — stored as 0x17/0x09,
 * ELECTRIC/STEEL, with Gen-2 held items (80, 173, 60) in the catch-rate byte. Rejecting
 * 0x09 would have flagged three perfectly real Pokemon as corrupt. Gen 2's CURSE_TYPE
 * (0x13) is an internal battle marker, never a mon's type, so it stays out. */
static bool g1_type_ok(uint8_t t) { return t <= 0x09u || (t >= 0x14u && t <= 0x1Bu); }

bool gb_check(const GbEditMon* e, GbIssues* out) {
  GbIssues iss;
  uint16_t dex;
  uint8_t level, raw;
  int i, j;
  memset(&iss, 0, sizeof iss);
  if (out) memset(out, 0, sizeof *out);
  if (!e || !gen_ok(e->gen)) { if (out) out->species_bad = true; return false; }

  raw   = gb_get_species_raw(e);
  dex   = gb_get_species_dex(e);
  level = gb_get_level(e);

  iss.species_bad = (dex == 0);
  /* Gen 2 stores 0xFD for an Egg, and only there does the list byte legitimately differ
   * from the record's species (gen2_save.c:519). */
  iss.list_mismatch = (e->list_species != raw)
                   && !(e->gen == GB_GEN2 && e->list_species == G2_LIST_EGG);
  iss.level_range = (level < 1 || level > 100);
  if (!iss.species_bad && !iss.level_range)
    iss.level_exp_bad = (gb_level_from_exp(dex, gb_get_exp(e)) != level);

  iss.move_empty = (gb_get_move(e, 0) == 0);
  for (i = 0; i < 4; i++) {
    uint8_t m = gb_get_move(e, i);
    if (m == 0) {
      for (j = i + 1; j < 4; j++) if (gb_get_move(e, j) != 0) iss.move_hole = true;
      /* The WHOLE byte, not just the PP bits: PP Ups live in the top two, and a stale
       * 0xC0 there is the fingerprint of the gb_set_ppup bug this gate used to miss.
       * Ground truth: all 908 empty move slots across Guy's five saves hold 0x00. */
      if (e->rec[pp_off(e) + i] != 0) iss.pp_on_empty = true;
      continue;
    }
    if (m > gb_max_move(e->gen)) { iss.move_range = true; continue; }
    for (j = 0; j < i; j++) if (gb_get_move(e, j) == m) iss.move_dup = true;
    if (gb_get_pp(e, i) > gb_max_pp(e->gen, m, gb_get_ppup(e, i))) iss.pp_over = true;
  }

  if (e->gen == GB_GEN1)
    iss.gen1_type_bad = !g1_type_ok(e->rec[R1_TYPE1]) || !g1_type_ok(e->rec[R1_TYPE2]);
  iss.stats_stale = e->stats_stale;

  if (out) *out = iss;
  return !(iss.species_bad || iss.list_mismatch || iss.level_range || iss.level_exp_bad
        || iss.move_empty || iss.move_hole || iss.move_range || iss.move_dup
        || iss.pp_over || iss.pp_on_empty || iss.gen1_type_bad || iss.stats_stale);
}

const char* gb_issue_text(const GbIssues* i) {
  if (!i) return 0;
  if (i->species_bad)    return "species is not a real Pokemon in this game";
  if (i->list_mismatch)  return "the box list and the record disagree about the species";
  if (i->level_range)    return "level is outside 1-100";
  if (i->level_exp_bad)  return "level and experience do not agree";
  if (i->move_empty)     return "no move in the first slot";
  if (i->move_hole)      return "an empty move slot before an occupied one";
  if (i->move_range)     return "a move this game does not have";
  if (i->move_dup)       return "the same move twice";
  if (i->pp_over)        return "PP above that move's maximum";
  if (i->pp_on_empty)    return "PP stored on a move slot that is empty";
  if (i->gen1_type_bad)  return "the record's stored types are not Gen-1 types";
  if (i->stats_stale)    return "stats were not recalculated after the edit";
  return 0;
}

/* =========================================================================== */
/* Re-parse verification, snapshot/rollback, and the checked write              */
/* =========================================================================== */

/* Decode a name field with the SHIPPING reader — deliberately not gb_name_decode, whose
 * whole point is to disagree with these two where they lose a glyph. Comparing the
 * shipping decode of the committed bytes against the name the shipping PARSER extracted
 * is what proves the parser found this slot's field and not the next slot's. */
static bool same_text(const GbEditMon* e, const uint8_t* raw_field, const char* parsed) {
  char mine[40];   /* a Gen-1 nickname can decode to 30 bytes + NUL (gen1_save.h) */
  if (e->gen == GB_GEN1) gen1_decode_name(mine, (int)sizeof mine, raw_field, GB_NAME_BYTES);
  else                   g2_decode_text(raw_field, G2_NAME_CHARS, mine, (int)sizeof mine);
  return strcmp(mine, parsed) == 0;
}

bool gb_verify_slot(const GbEditMon* e, const uint8_t* list, int box, int slot) {
  int i, ro, oo, no, so;
  if (!e || !list || !gen_ok(e->gen)) return false;
  if (gb_box_is_party(e->gen, box) != e->is_party) return false;
  ro = gb_off_record(e->gen, box, slot);
  oo = gb_off_otname(e->gen, box, slot);
  no = gb_off_nickname(e->gen, box, slot);
  so = gb_off_species(e->gen, box, slot);
  if (ro < 0 || oo < 0 || no < 0 || so < 0) return false;

  /* HALF ONE: the raw bytes, every one of them.
   *
   * This is what makes the gate exact. A struct comparison cannot be: Gen1Mon has no
   * stats[], no current HP, no type1/type2 and no box-level byte, so it can only speak
   * for 28 of a Gen-1 party record's 44 bytes — and the missing 16 are exactly the ones
   * gb_recalc_stats, gb_set_gen1_base/gb_set_species and gb_set_level write. The name
   * fields are compared whole, past their 0x50 terminators too, because the editor holds
   * those bytes and "came back exactly as the editor holds it" has to mean all of them. */
  if (memcmp(list + ro, e->rec,    e->rec_len)    != 0) return false;
  if (memcmp(list + oo, e->otname, GB_NAME_BYTES) != 0) return false;
  if (memcmp(list + no, e->nick,   GB_NAME_BYTES) != 0) return false;
  if (list[so] != e->list_species) return false;

  /* HALF TWO: the shipping parser, which derives the four parallel arrays' offsets
   * ITSELF. Half one used this module's arithmetic on both sides, so an off-by-one slot
   * would compare the wrong bytes against themselves and pass; only an independent
   * implementation can catch that. */
  if (e->gen == GB_GEN1) {
    Gen1Mon m;
    if (!gen1_decode(list, box, slot, &m)) return false;
    if (m.list_mismatch) return false;
    if (m.species_idx != gb_get_species_raw(e)) return false;
    if (m.level != gb_get_level(e)) return false;
    if (m.exp   != gb_get_exp(e))   return false;
    if (m.otId  != gb_get_otid(e))  return false;
    if (m.catch_rate != e->rec[R1_CATCH]) return false;
    for (i = 0; i < GB_NSTATS; i++) {
      if (m.dv[i] != gb_get_dv(e, i)) return false;             /* incl. the derived HP DV */
      if (m.statexp[i] != gb_get_statexp(e, i)) return false;
    }
    for (i = 0; i < 4; i++) {
      if (m.moves[i] != gb_get_move(e, i)) return false;
      if (m.pp[i] != gb_get_pp(e, i) || m.ppups[i] != gb_get_ppup(e, i)) return false;
    }
    return same_text(e, e->otname, m.otName) && same_text(e, e->nick, m.nickname);
  }

  {
    G2Mon m;
    uint8_t lsp = 0;
    bool legg = false;
    if (!g2_list_mon(list, box, slot, &m)) return false;
    /* THE SPECIES LIST, read back through the PARSER'S OWN view of where it is.
     * g2_list_mon reports the species out of the RECORD and never looks at the list, so
     * before this the byte whose entire purpose is to be the menus' cache was the one
     * thing half two never saw — an 0xFF landing there truncates the box for the game's
     * own walker while every other field agrees.
     *
     * Note precisely what this line buys, because half one already compares that byte:
     * half one uses gb_off_species() on BOTH sides, so an off-by-one there would write
     * the species into the next slot's list byte and then dutifully compare that same
     * wrong byte against itself and pass. g2_list_species() derives the position itself
     * and is the only thing in this file that would notice.
     *
     * Measured, not asserted: with gb_off_species deliberately shifted one slot and a
     * species edit committed to a slot where no Egg is involved (an Egg would trip the
     * is_egg comparison below by luck rather than by design), gb_verify_slot returns
     * false with these three lines and TRUE without them. */
    if (!g2_list_species(list, box, slot, &lsp, &legg)) return false;
    if (lsp != e->list_species) return false;
    if (legg != gb_is_egg(e)) return false;
    if (m.species != gb_get_species_raw(e)) return false;
    if (m.is_egg != gb_is_egg(e)) return false;
    if (m.held_item != gb_get_held_item(e)) return false;
    if (m.level != gb_get_level(e)) return false;
    if (m.exp   != gb_get_exp(e))   return false;
    if (m.otid  != gb_get_otid(e))  return false;
    if (m.friendship != e->rec[R2_HAPPY] || m.pokerus != e->rec[R2_PKRS]) return false;
    if (m.caught_time  != (uint8_t)(e->rec[R2_CAUGHT0] >> 6)) return false;
    if (m.caught_level != (uint8_t)(e->rec[R2_CAUGHT0] & 0x3F)) return false;
    if (m.ot_gender    != (uint8_t)(e->rec[R2_CAUGHT1] >> 7)) return false;
    if (m.caught_loc   != (uint8_t)(e->rec[R2_CAUGHT1] & 0x7F)) return false;
    if (m.hp_dv != gb_get_dv(e, GB_HP)) return false;
    for (i = 0; i < 4; i++) {
      if (m.dv[i] != gb_get_dv(e, i + 1)) return false;         /* Atk,Def,Spe,Spc */
      if (m.moves[i] != gb_get_move(e, i)) return false;
      if (m.pp[i] != gb_get_pp(e, i) || m.pp_up[i] != gb_get_ppup(e, i)) return false;
    }
    for (i = 0; i < 5; i++) if (m.statexp[i] != gb_get_statexp(e, i)) return false;
    if (e->is_party) {
      if (m.cur_hp != rd16be(e->rec + R2_CURHP)) return false;
      for (i = 0; i < 6; i++) if (m.stats[i] != gb_get_stat(e, i)) return false;
    }
    return same_text(e, e->otname, m.otname) && same_text(e, e->nick, m.nickname);
  }
}

bool gb_slot_save(GbSlotSnapshot* s, uint8_t gen, const uint8_t* list, int box, int slot) {
  int ro, oo, no, so;
  if (!s) return false;
  memset(s, 0, sizeof *s);
  if (!list || !gen_ok(gen)) return false;
  ro = gb_off_record(gen, box, slot);
  oo = gb_off_otname(gen, box, slot);
  no = gb_off_nickname(gen, box, slot);
  so = gb_off_species(gen, box, slot);
  if (ro < 0 || oo < 0 || no < 0 || so < 0) return false;
  s->gen     = gen;
  s->rec_len = (uint8_t)gb_rec_size(gen, gb_box_is_party(gen, box));
  s->box     = box;
  s->slot    = slot;
  memcpy(s->rec,    list + ro, s->rec_len);
  memcpy(s->otname, list + oo, GB_NAME_BYTES);
  memcpy(s->nick,   list + no, GB_NAME_BYTES);
  s->list_species = list[so];
  s->valid = true;
  return true;
}

/* Restores into the box and slot it was taken from — the snapshot carries them so a
 * roll-back can never be aimed at a different Pokemon by a caller's bookkeeping slip. */
bool gb_slot_restore(const GbSlotSnapshot* s, uint8_t* list) {
  int ro, oo, no, so;
  if (!s || !s->valid || !list || !gen_ok(s->gen)) return false;
  ro = gb_off_record(s->gen, s->box, s->slot);
  oo = gb_off_otname(s->gen, s->box, s->slot);
  no = gb_off_nickname(s->gen, s->box, s->slot);
  so = gb_off_species(s->gen, s->box, s->slot);
  if (ro < 0 || oo < 0 || no < 0 || so < 0) return false;
  memcpy(list + ro, s->rec,    s->rec_len);
  memcpy(list + oo, s->otname, GB_NAME_BYTES);
  memcpy(list + no, s->nick,   GB_NAME_BYTES);
  list[so] = s->list_species;
  return true;
}

bool gb_commit_checked(const GbEditMon* e, uint8_t* list, int box, int slot) {
  GbSlotSnapshot snap;                 /* ~76 bytes of the CALLER's stack, no statics */
  if (!e || !list || !gen_ok(e->gen)) return false;
  if (!gb_slot_save(&snap, e->gen, list, box, slot)) return false;
  if (!gb_commit(e, list, box, slot)) return false;   /* refused: nothing was written */
  if (gb_verify_slot(e, list, box, slot)) return true;
  /* The gate is the whole point of the call, so a failure has to leave no trace. */
  gb_slot_restore(&snap, list);
  return false;
}

/* BACKLOG #104 audit: is a cross-generation transfer ROUND-TRIP EXACT today?
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_xfer_roundtrip_test.c \
 *      source/gen3_to_gb.c source/gb_sidecar.c source/bank_cell.c source/gen12_convert.c \
 *      source/gen3_save.c source/gen3_mon.c source/gen3_box.c source/gen3_edit.c \
 *      source/gen3_daycare.c source/data_tables.c source/evolutions.c \
 *      source/gb_edit.c source/gen1_save.c source/gen2_save.c \
 *      source/xfer_rec.c source/bank_restore.c source/item_map_g2g3.c \
 *      -o /tmp/hxfer
 *   /tmp/hxfer /Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/ (.sav files)
 *
 * source/evolutions.c is GENERATED and gitignored (BACKLOG #104 R1's own MAKE LEGAL
 * edge case, section C below, needs a real evolution floor; evolutions.h's own weak
 * fallbacks answer "no data" and that section SKIPs, rather than failing, if it is
 * absent).
 *
 * NO ASSERTIONS ON EQUALITY -- this is an AUDIT, not a regression pin (the sibling
 * tests host_gen3gb_test.c and host_gen12_test.c already pin the two converters'
 * documented behaviour), EXCEPT for two additions BACKLOG #104 R1 makes on top of
 * the original audit (both still additive -- section A's own per-field tally is
 * unchanged): a hard CHECK that KEEP AS IS (no GB-side edit) is byte-identical to
 * the original across the WHOLE real corpus (the "80 bytes" tally already measured
 * this; the R1 brief asks for it ASSERTED, not just counted), and section C, a
 * dedicated MAKE LEGAL edge case built from a REAL corpus record with its level
 * artificially lowered on a /tmp-local copy (never touching the read-only corpus
 * file) via the Gen-3 edit core (gen3_edit.c) already shipped for exactly this
 * kind of synthetic-from-real test record.
 * The only other thing CHECK() below guards is "the code path ran and produced a record"
 * -- never "the round trip was lossless".
 *
 * TWO DIRECTIONS, on real mons (party + box 0 of every save the corpus has):
 *
 *   A. Gen 3 -> GB (Gen 2) -> Gen 3, THROUGH THE SHIPPING SIDECAR
 *      (source/gen3_to_gb.c + source/gb_sidecar.c, docs/GEN3-TO-GB-SIDECAR-DESIGN.md
 *      #39). gbsc_merge_up() is handed the sidecar entry with NO Game-Boy-side edit
 *      applied (the "just came back" case app_paste_gb_merge hits when the mon was
 *      never touched abroad) and its output is byte-compared against the original 80
 *      bytes field by field.
 *
 *   B. GB -> Gen 3 -> GB, WITH NO JOURNAL AT ALL, because none exists for this
 *      direction (source/gen12_convert.h's own header: "ONE-WAY ONLY. Nothing here
 *      ever writes to a GB save" -- gen12_convert.c/gen3_to_gb.c never persist the
 *      native Gen-1/2 bytes anywhere). A native Gen-1/2 mon is imported into Gen 3
 *      with gen12_convert() (the exact function pdna_gen12.c's box grid and
 *      app_copy()'s "no sidecar" fallback both call), and the resulting 80-byte
 *      record is then run BACK through gen3_to_gb() -- exactly what gb_paste_write()
 *      would do if that mon were transferred down again today. The result is
 *      compared against the ORIGINAL native record decoded straight off the save.
 *      Whatever gen12_convert() could not carry the first time (ability, nature,
 *      gender-as-such, ribbons, EVs/stat-exp, met data, secret id, Gen-1
 *      friendship/held item/pokerus) is gone for good the moment the mon lands in
 *      Gen 3 -- there is no sidecar entry to fall back on, because gb_paste_write()
 *      always treats whatever 80 bytes it is handed as "the original" and writes a
 *      FRESH sidecar keyed off the CURRENT Game Boy record, not off anything from
 *      before the import. That gap is what docs/TRANSFER-ROUNDTRIP-DESIGN.md's R2
 *      phase exists to close.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "gen3_edit.h"
#include "evolutions.h"
#include "gen3_to_gb.h"
#include "gb_sidecar.h"
#include "gen12_convert.h"
#include "gen1_save.h"
#include "gen2_save.h"
#include "bank_cell.h"
#include "xfer_rec.h"
#include "bank_restore.h"
#include "data_tables.h"   /* pk_national_no */
#include "item_map_g2g3.h" /* item_g2_to_g3 -- review D2's item-loss comparison */

/* Same convention as host_gen3gb_test.c/host_gbsession_test.c: the Game Boy corpus
 * lives OUTSIDE the repo at a fixed path (gitignored, never published), not on argv
 * -- only the Gen-3 corpus is argv-driven (run_host_tests.py hands that over). */
#define GB_ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { \
    g_check++; \
    if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
  } while (0)

/* ============================================================================ */
/* Aggregation: one bucket of named boolean loss counters, printed as a table.  */
/* ============================================================================ */

#define MAX_FIELDS 24
typedef struct {
  const char* name[MAX_FIELDS];
  int lost[MAX_FIELDS];
  int nfields;
  int nmons;
} FieldTally;

static void tally_init(FieldTally* t) { memset(t, 0, sizeof *t); }
static int tally_field(FieldTally* t, const char* name) {
  for (int i = 0; i < t->nfields; i++) if (strcmp(t->name[i], name) == 0) return i;
  int i = t->nfields++;
  t->name[i] = name;
  return i;
}
static void tally_mark(FieldTally* t, const char* name) {
  t->lost[tally_field(t, name)]++;
}
static void tally_report(const FieldTally* t, const char* title) {
  printf("  -- %s: %d mon(s) audited --\n", title, t->nmons);
  if (t->nfields == 0 || t->nmons == 0) { printf("     (no field losses observed)\n"); return; }
  for (int i = 0; i < t->nfields; i++) {
    if (t->lost[i] == 0) continue;
    printf("     field %-16s lost in %d of %d mons\n", t->name[i], t->lost[i], t->nmons);
  }
}

/* ============================================================================ */
/* A. Gen 3 -> GB (Gen 2) -> Gen 3, through gen3_to_gb + gb_sidecar.             */
/* ============================================================================ */

static void audit_field_a(FieldTally* t, const char* name, bool same) {
  if (!same) tally_mark(t, name);
}

/* BACKLOG #104 R1: the first REAL corpus record found that (a) converts cleanly to
 * Gen 2 and (b) is an evolved species with a level-gated evolution floor
 * (pk_evo_min_level() > 1) -- so section C below has a genuine, non-synthetic
 * "an underlevelled evolved pokemon" to build its edge case from, per the brief's
 * own wording ("edge save on a /tmp copy via the Gen-3 tools: lower an evolved
 * mon's level below its minimum"), not a fully invented record. */
static bool    g_have_evo_sample = false;
static uint8_t g_evo_sample_rec[80];

/* BACKLOG #150 S150-8b: the first real Gen-3 corpus record, for REFUSE-1. */
static bool    g_have_g3_sample = false;
static uint8_t g_g3_sample_rec[80];

static void audit_gen3_roundtrip_one(FieldTally* t, const uint8_t rec80[80]) {
  PkMon orig;
  if (!pk_decode_mon(rec80, false, &orig)) return;      /* empty slot */
  if (orig.isEgg || orig.isBadEgg) return;               /* gen3_to_gb refuses these too */
  t->nmons++;

  GbEditMon down;
  Gen3ToGbLoss loss;
  G3GbStatus st = gen3_to_gb(rec80, GB_GEN2, true, NULL, &down, &loss);
  if (st != G3GB_OK) {
    /* A REAL, expected outcome for a Hoenn species or a post-Gen-2 move, not a
     * pipeline bug -- gen3_to_gb's own documented refusal table. Tallied, not
     * CHECK-failed: the brief asks for a field-loss table, and "cannot even start
     * the trip" is exactly the finding worth counting, at its own severity. */
    tally_mark(t, "(refused down)");
    return;
  }

  GbscEntry e;
  gbsc_entry_from(&e, &down, rec80, 0);

  uint8_t back80[80];
  GbscMergeReport rep;
  CHECK(gbsc_merge_up(&e, &down, back80, &rep), "gbsc_merge_up ran (species %u)", orig.species);

  PkMon merged;
  bool decoded = pk_decode_mon(back80, false, &merged);
  CHECK(decoded, "the merged-up record still decodes (species %u)", orig.species);
  if (!decoded) { tally_mark(t, "(merged undecodable)"); return; }

  audit_field_a(t, "species",     merged.species == orig.species);
  audit_field_a(t, "nickname",    strcmp(merged.nickname, orig.nickname) == 0);
  audit_field_a(t, "OT name",     strcmp(merged.otName, orig.otName) == 0);
  audit_field_a(t, "personality", merged.personality == orig.personality);
  audit_field_a(t, "OT id",       merged.otId == orig.otId);
  audit_field_a(t, "IVs",         memcmp(merged.ivs, orig.ivs, sizeof orig.ivs) == 0);
  audit_field_a(t, "EVs",         memcmp(merged.evs, orig.evs, sizeof orig.evs) == 0);
  audit_field_a(t, "moves",       memcmp(merged.moves, orig.moves, sizeof orig.moves) == 0);
  audit_field_a(t, "PP",          memcmp(merged.pp, orig.pp, sizeof orig.pp) == 0);
  audit_field_a(t, "exp",         merged.experience == orig.experience);
  audit_field_a(t, "nature",      merged.nature == orig.nature);
  audit_field_a(t, "ability",     merged.abilityNum == orig.abilityNum);
  audit_field_a(t, "friendship",  merged.friendship == orig.friendship);
  audit_field_a(t, "held item",   merged.heldItem == orig.heldItem);
  audit_field_a(t, "pokerus",     merged.pokerus == orig.pokerus);
  audit_field_a(t, "met data",    merged.metLocation == orig.metLocation &&
                                   merged.metLevel == orig.metLevel &&
                                   merged.metGame == orig.metGame);
  audit_field_a(t, "ball",        merged.pokeball == orig.pokeball);
  audit_field_a(t, "OT gender",   merged.otGender == orig.otGender);
  audit_field_a(t, "language",    merged.language == orig.language);
  audit_field_a(t, "contest",     memcmp(merged.contest, orig.contest, sizeof orig.contest) == 0);
  audit_field_a(t, "ribbons",     merged.ribbons == orig.ribbons);
  audit_field_a(t, "80 bytes",    memcmp(back80, rec80, 80) == 0);
  /* BACKLOG #104 R1: KEEP AS IS's own contract, ASSERTED rather than only tallied --
   * "the no-correction case (KEEP AS IS = today's bytes, byte-identical on all
   * corpus mons)". This is the SAME comparison "80 bytes" tallies above; the CHECK
   * here turns it into a hard regression pin across the whole real Gen-3 corpus,
   * which the tally-only style deliberately never did before this. */
  CHECK(memcmp(back80, rec80, 80) == 0,
        "KEEP AS IS: merge-up with no GB-side edit is byte-identical to the original (species %u)",
        orig.species);

  /* Capture the first real, cleanly-converting, evolved (level-gated) mon this
   * corpus offers, for section C's MAKE LEGAL edge case below. */
  if (!g_have_evo_sample && pk_evo_have_data()) {
    int min_lvl = pk_evo_min_level(orig.species);
    if (min_lvl != PK_EVO_NO_DATA && min_lvl > 1) {
      g_have_evo_sample = true;
      memcpy(g_evo_sample_rec, rec80, 80);
    }
  }

  /* BACKLOG #150 S150-8b, REFUSE-1: capture the first real Gen-3 corpus record for
   * section D's "xr_merge_down must refuse a non-native original80" check. */
  if (!g_have_g3_sample) {
    g_have_g3_sample = true;
    memcpy(g_g3_sample_rec, rec80, 80);
  }
}

static void run_gen3_corpus_file(FieldTally* t, const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP %s (cannot open)\n", path); return; }
  static uint8_t save[G3_SAVE_FILE_SIZE];
  size_t n = fread(save, 1, sizeof save, f);
  fclose(f);
  Gen3SaveInfo info;
  if (!gen3_parse(save, (uint32_t)n, &info)) { printf("  SKIP %s (parse failed)\n", path); return; }

  static uint8_t sb1[G3_SAVEBLOCK1_BYTES];
  static uint8_t pc[G3_PC_BYTES];
  gen3_read_saveblock1(save, info.slot, sb1);
  gen3_read_pc_storage(save, info.slot, pc);

  bool frlg = false;
  PkMon party[6];
  pk_read_party_auto(sb1, party, &frlg);
  uint16_t doff = frlg ? 0x0038 : 0x0238;
  uint16_t coff = frlg ? 0x0034 : 0x0234;
  uint8_t count = sb1[coff];
  if (count > 6) count = 6;

  int scanned = 0;
  for (int i = 0; i < count; i++) {
    audit_gen3_roundtrip_one(t, sb1 + doff + (uint32_t)i * 100);   /* 80-byte core only */
    scanned++;
  }
  for (int s = 0; s < G3_IN_BOX; s++) {                            /* box 0 only, per the brief */
    audit_gen3_roundtrip_one(t, pc + 0x0004 + (uint32_t)s * 80);
    scanned++;
  }
  printf("  %s: %d slots scanned (party + box 0)\n", path, scanned);
}

/* ============================================================================ */
/* B. GB -> Gen 3 -> GB, with NO journal (there is none today).                 */
/* ============================================================================ */

static void audit_field_b(FieldTally* t, const char* name, bool same, bool applicable) {
  if (!applicable) return;   /* the source generation never had this field to begin with */
  if (!same) tally_mark(t, name);
}

/* `in` is the native Gen-1/2 mon already adapted into a Gb12Mon (gen12_from_gen1/2). */
static void audit_gb_roundtrip_one(FieldTally* t, const Gb12Mon* in, uint8_t gen) {
  if (gen12_can_convert(in) != GB12_OK) return;   /* egg / item-holder / glitch: not importable */
  t->nmons++;

  Gb12Target tgt; tgt.met_game = 3;   /* Emerald, arbitrary -- matches the box grid's own default */
  uint8_t rec80[80];
  Gb12Notes notes;
  Gb12Result gr = gen12_convert(in, &tgt, rec80, &notes);
  CHECK(gr == GB12_OK, "gen12_convert ran (species dex %u)", in->species_dex);
  if (gr != GB12_OK) { tally_mark(t, "(refused up)"); return; }

  /* Now simulate what gb_paste_write() does today if this SAME imported mon is later
   * transferred back down: it treats rec80 as an ordinary Gen-3 record (no journal
   * says otherwise) and calls gen3_to_gb() on it, targeting Gen 2 (a real Gen-1
   * target would need a base-stats ROM table this host test does not have -- the S5-C
   * "needs base" gate; Gen 2 needs none, and is a legal paste target either way). */
  GbEditMon back;
  Gen3ToGbLoss loss;
  G3GbStatus st = gen3_to_gb(rec80, GB_GEN2, true, NULL, &back, &loss);
  if (st != G3GB_OK) { tally_mark(t, "(refused back down)"); return; }

  char back_nick[64] = {0}, back_ot[64] = {0};
  gb_get_nickname(&back, back_nick, sizeof back_nick);
  gb_get_otname(&back, back_ot, sizeof back_ot);

  bool is_gen2 = (gen == GB_GEN2);
  audit_field_b(t, "species dex", gb_get_species_dex(&back) == in->species_dex, true);
  audit_field_b(t, "level",       gb_get_level(&back) == in->level, true);
  audit_field_b(t, "exp",         gb_get_exp(&back) == in->exp, true);
  audit_field_b(t, "DV atk",      gb_get_dv(&back, GB_ATK) == in->dv_atk, true);
  audit_field_b(t, "DV def",      gb_get_dv(&back, GB_DEF) == in->dv_def, true);
  audit_field_b(t, "DV spe",      gb_get_dv(&back, GB_SPE) == in->dv_spd, true);
  audit_field_b(t, "DV spc",      gb_get_dv(&back, GB_SPC) == in->dv_spc, true);
  audit_field_b(t, "moves",       gb_get_move(&back, 0) == in->moves[0] &&
                                   gb_get_move(&back, 1) == in->moves[1] &&
                                   gb_get_move(&back, 2) == in->moves[2] &&
                                   gb_get_move(&back, 3) == in->moves[3], true);
  audit_field_b(t, "PP Ups",      gb_get_ppup(&back, 0) == in->pp_ups[0] &&
                                   gb_get_ppup(&back, 1) == in->pp_ups[1] &&
                                   gb_get_ppup(&back, 2) == in->pp_ups[2] &&
                                   gb_get_ppup(&back, 3) == in->pp_ups[3], true);
  audit_field_b(t, "OT id",       gb_get_otid(&back) == in->ot_id, true);
  audit_field_b(t, "OT name",     strcmp(back_ot, in->ot_name) == 0, true);
  audit_field_b(t, "nickname",    strcmp(back_nick, in->nickname) == 0, true);
  /* held item / friendship / pokerus: Gen 1 never had them (Gb12Mon leaves them 0 by
   * convention for a Gen-1 input), so only audit them for a Gen-2 origin. */
  audit_field_b(t, "held item",   true, false);   /* GB_GEN2 target never carries item back
                                                    * at all -- gen3_to_gb's own item_dropped
                                                    * loss is unconditional; see note below */
  (void)is_gen2;
}

static uint8_t g_gen1_img[64 * 1024];
static uint8_t g_gen2_img[64 * 1024];

static bool load_file(const char* path, uint8_t* buf, size_t cap, size_t* out_len) {
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  *out_len = fread(buf, 1, cap, f);
  fclose(f);
  return *out_len > 0;
}

static void run_gen1_file(FieldTally* t, const char* path) {
  size_t len;
  if (!load_file(path, g_gen1_img, sizeof g_gen1_img, &len)) { printf("  SKIP %s\n", path); return; }
  Gen1Save s;
  if (gen1_open(g_gen1_img, (uint32_t)len, &s) != GEN1_OK) { printf("  SKIP %s (open failed)\n", path); return; }

  int scanned = 0;
  int boxes[2] = { 0, GEN1_PARTY_BOX };
  for (int bi = 0; bi < 2; bi++) {
    int box = boxes[bi];
    int n = gen1_count(&s, box);
    for (int slot = 0; slot < n; slot++) {
      Gen1Mon g1;
      if (!gen1_decode_image(&s, g_gen1_img, box, slot, &g1)) continue;
      if (g1.dex < 1 || g1.dex > 251) continue;   /* MissingNo/glitch -- not importable */
      Gb12Mon in;
      gen12_from_gen1(&g1, (uint32_t)(box * 20 + slot), &in);
      audit_gb_roundtrip_one(t, &in, GB_GEN1);
      scanned++;
    }
  }
  printf("  %s: %d slots scanned (box 0 + party)\n", path, scanned);
}

static void run_gen2_file(FieldTally* t, const char* path) {
  size_t len;
  if (!load_file(path, g_gen2_img, sizeof g_gen2_img, &len)) { printf("  SKIP %s\n", path); return; }
  G2Save sv;
  if (!g2_detect(g_gen2_img, (uint32_t)len, &sv) || !sv.supported) {
    printf("  SKIP %s (detect failed/unsupported)\n", path); return;
  }
  G2Header hd;
  if (!g2_read_header(g_gen2_img, &sv, &hd)) { printf("  SKIP %s (header)\n", path); return; }

  int scanned = 0;
  int boxes[2] = { 0, G2_BOX_PARTY };
  for (int bi = 0; bi < 2; bi++) {
    int box = boxes[bi];
    int n = g2_box_count_at(g_gen2_img, &sv, &hd, box);
    if (n < 0) n = 0;
    for (int slot = 0; slot < n; slot++) {
      G2Mon g2;
      if (!g2_box_mon_at(g_gen2_img, &sv, &hd, box, slot, &g2)) continue;
      if (g2.species < 1 || g2.species > 251 || g2.is_egg) continue;
      Gb12Mon in;
      gen12_from_gen2(&g2, (uint32_t)(box * 20 + slot), &in);
      audit_gb_roundtrip_one(t, &in, GB_GEN2);
      scanned++;
    }
  }
  printf("  %s: %d slots scanned (box 0 + party)\n", path, scanned);
}

/* ============================================================================ */
/* C. BACKLOG #104 R1: MAKE LEGAL's level correction, on a REAL corpus record.   */
/* ============================================================================ */

static void test_make_legal_edge_case(void) {
  printf("== C. BACKLOG #104 R1: MAKE LEGAL, an underlevelled evolved mon ==\n");
  if (!g_have_evo_sample) {
    printf("  SKIP (no real corpus record with a level-gated evolution found)\n");
    return;
  }
  if (!pk_evo_have_data()) { printf("  SKIP (no evolutions table linked)\n"); return; }

  PkMon base;
  bool base_ok = pk_decode_mon(g_evo_sample_rec, false, &base);
  CHECK(base_ok, "the captured sample decodes");
  if (!base_ok) return;

  int min_lvl = pk_evo_min_level(base.species);
  CHECK(min_lvl != PK_EVO_NO_DATA && min_lvl > 1,
        "the captured sample has a level-gated evolution floor (got %d)", min_lvl);
  if (min_lvl == PK_EVO_NO_DATA || min_lvl <= 1) return;

  /* Lower the level well below the floor, on a /tmp-local COPY -- gen3_edit.c is
   * the same "Gen-3 tools" every other edit screen in this tree uses, never the
   * read-only corpus file itself (which is never re-opened by this test at all;
   * `sample` lives only in this function's own stack/locals). */
  uint8_t sample[80];
  memcpy(sample, g_evo_sample_rec, 80);
  uint8_t low_level = (min_lvl > 5) ? (uint8_t)(min_lvl - 5) : 1;
  EditMon em;
  gen3_edit_load(sample, false, &em);
  em_set_level(&em, low_level);
  gen3_edit_commit(&em, sample);

  PkMon edge;
  CHECK(pk_decode_mon(sample, false, &edge), "the lowered-level edge case decodes");
  pk_resolve(&edge);   /* box record: level comes from EXP+growth-rate, gen3_box.c */
  CHECK(edge.level == low_level, "the edge case's level was actually lowered (got %u want %u)",
        edge.level, low_level);
  CHECK(edge.species == base.species, "the edge case's species is unchanged by the level edit");

  GbEditMon down;
  Gen3ToGbLoss loss;
  G3GbStatus st = gen3_to_gb(sample, GB_GEN2, true, NULL, &down, &loss);
  CHECK(st == G3GB_OK, "the underlevelled edge case converts (species %u, %s)",
        base.species, g3gb_status_text(st));
  if (st != G3GB_OK) return;

  /* KEEP AS IS: this specific edge case must still round-trip byte-identically,
   * same as every other untouched conversion -- the mechanism above must not have
   * broken the common path for the species it happens to be exercising. */
  {
    GbscEntry e; gbsc_entry_from(&e, &down, sample, 0);
    uint8_t back80[80]; GbscMergeReport rep;
    CHECK(gbsc_merge_up(&e, &down, back80, &rep), "KEEP AS IS: merge up (no GB edit) succeeds");
    CHECK(!rep.level_changed, "KEEP AS IS: no GB-side edit -> level_changed is false");
    CHECK(memcmp(back80, sample, 80) == 0,
          "KEEP AS IS: the edge case still round-trips byte-identically");
  }

  /* MAKE LEGAL: gb_paste_hook's own sequence, exactly as the shipped UI slice runs
   * it -- probe, then apply the reported level via gb_set_level(). */
  uint8_t from_lvl = 0, to_lvl = 0;
  bool need_fix = gen3_to_gb_evo_needs_fix(&down, &from_lvl, &to_lvl);
  CHECK(need_fix, "the underlevelled edge case is offered the MAKE LEGAL fix");
  CHECK(from_lvl == low_level, "from_level matches the lowered level (got %u want %u)",
        from_lvl, low_level);
  CHECK(to_lvl == (uint8_t)min_lvl, "to_level matches the evolution floor (got %u want %d)",
        to_lvl, min_lvl);

  GbEditMon fixed = down;
  CHECK(gb_set_level(&fixed, to_lvl), "MAKE LEGAL: gb_set_level raises the level");

  GbscEntry e2; gbsc_entry_from(&e2, &fixed, sample, 0);   /* sample: the true, lowered
                                                             * original -- unchanged by
                                                             * the correction. */
  CHECK(e2.written_level == to_lvl,
        "MAKE LEGAL: written_level records the level ACTUALLY WRITTEN (got %u want %u)",
        e2.written_level, to_lvl);
  CHECK(memcmp(e2.original80, sample, 80) == 0,
        "MAKE LEGAL: the sidecar's original80 is the true, uncorrected (lowered-level) original");

  /* The whole point: merge-up with no further Game-Boy-side change restores the
   * ORIGINAL 80 bytes (the lowered-level record) EXACTLY, on a REAL corpus mon's
   * own bytes -- not just the fully synthetic case host_gen3gb_test.c's own
   * section 5 already covers. */
  uint8_t back80[80]; GbscMergeReport rep;
  CHECK(gbsc_merge_up(&e2, &fixed, back80, &rep), "MAKE LEGAL, no further edit: merge up succeeds");
  CHECK(!rep.level_changed, "MAKE LEGAL, no further edit: level_changed is false (the fix)");
  CHECK(memcmp(back80, sample, 80) == 0,
        "MAKE LEGAL, no further edit: restores the original (lowered-level) 80 bytes exactly");
}

/* ============================================================================ */
/* D. BACKLOG #150 S150-8b: xr_merge_down -- the RESTORE edge's own round trips.  */
/*                                                                                */
/* bank_down_convert.c (bdc_convert_gen3_core, the REAL DOWN edge's pure core) is */
/* deliberately NOT linked here -- the brief's own instruction for this file's cc */
/* line is "add source/xfer_rec.c and nothing else". xr_down_sim() below is a     */
/* narrow, test-local stand-in for JUST the one piece these round trips need      */
/* (bdc_convert_gen3_core's own item-relax step, decision 5/6: zero the held item */
/* on the CONVERSION VIEW only, never on the cell itself, so an item holder       */
/* converts instead of refusing with GB12_ERR_HELD_ITEM) -- everything else       */
/* (the cart item-mask, the MAKE LEGAL screen, the ledger file I/O) is out of     */
/* scope for a PURE-C round trip of xr_merge_down and is already covered by       */
/* tests/host_xferdown_test.c (S150-8's own lane) and tests/host_bankcell_test.c. */
/* ============================================================================ */

static uint32_t g_xr_serial = 500000;

static bool xr_down_sim(const GbEditMon* mon, uint8_t origin, uint32_t serial,
                        uint8_t cell80[80], GbEditMon* written, uint8_t out80[80]) {
  if (bc_pack(mon, 0, origin, 0, serial, cell80) != 0) return false;
  BcMeta meta;
  if (!bc_unpack(cell80, written, &meta)) return false;
  Gb12Mon view;
  if (!bc_view(written, &meta, bc_ident32(cell80), &view)) return false;
  view.held_item = 0;   /* decision 5/6's relax, item stays behind in cell80/original80 */
  Gb12Target tgt;
  memset(&tgt, 0, sizeof tgt);
  tgt.met_game = 3;    /* Emerald -- arbitrary, any legal met_game does for this test */
  Gb12Notes notes;
  return gen12_convert(&view, &tgt, out80, &notes) == GB12_OK;
}

/* Builds the ledger entry EXACTLY the way pdna_gen12.c's xfer_down_write() does for
 * the ABROAD_G3 direction (gbsc_entry_from(), then the four S150-8 overrides, then
 * the nick_written override to the Gen-3 record's own raw bytes) -- see
 * source/pdna_gen12.c:2652-2678. */
static void xr_build_entry_asdown(GbscEntry* e, const GbEditMon* written,
                                  const uint8_t cell80[80], const uint8_t g3rec80[80]) {
  gbsc_entry_from(e, written, cell80, 0);
  e->kind = XR_KIND_NATIVE_HOME;
  e->state = XR_STATE_PENDING;
  e->direction = XR_DIR_ABROAD_G3;
  e->claimed = 1;
  memcpy(e->nick_written, g3rec80 + 0x08, 10);
  e->nick_written[10] = 0;
}

/* The round-trip comparison this brief's Acceptance section names explicitly:
 * rec[0..rec_len), otname, nick, gen, rec_len -- NEVER the cell's own bytes (ident32
 * and bank_serial are re-serialised by design, G-M4). */
static void xr_check_roundtrip(const char* tag, const GbEditMon* written,
                               const GbEditMon* merged) {
  CHECK(merged->gen == written->gen, "%s: gen matches", tag);
  CHECK(merged->rec_len == written->rec_len, "%s: rec_len matches", tag);
  CHECK(memcmp(merged->rec, written->rec, written->rec_len) == 0, "%s: rec[] byte-identical", tag);
  CHECK(memcmp(merged->otname, written->otname, GB_NAME_BYTES) == 0, "%s: otname byte-identical", tag);
  CHECK(memcmp(merged->nick, written->nick, GB_NAME_BYTES) == 0, "%s: nick byte-identical", tag);
}

static void xr_check_meta_survives(const char* tag, const GbEditMon* merged, const BcMeta* meta) {
  uint8_t recell[80];
  CHECK(bc_pack(merged, meta->flags, meta->origin_game, meta->rtc_epoch, meta->bank_serial + 1, recell) == 0,
        "%s: re-bc_pack succeeds", tag);
  GbEditMon back2; BcMeta meta2;
  CHECK(bc_unpack(recell, &back2, &meta2), "%s: re-bc_unpack succeeds", tag);
  CHECK(meta2.gen == meta->gen, "%s: BcMeta.gen survives a re-pack", tag);
  CHECK(meta2.origin_game == meta->origin_game, "%s: BcMeta.origin_game survives a re-pack", tag);
  CHECK(meta2.flags == meta->flags, "%s: BcMeta.flags survives a re-pack", tag);
}

/* RT-1 (Gen 2 -> Gen 3 -> Gen 2) / RT-2 (Gen 1 -> Gen 3 -> Gen 1) one record, plus
 * RT-3's item-holder case implicitly (held_item is only zeroed on the transient
 * conversion VIEW above, never on `cell`/`written`, so a real item holder's item is
 * still sitting in e->original80 when xr_merge_down decodes it back out). Captures
 * the first successfully-converted record into `capture` for the MERGE-1/2/3 tests
 * below, if `capture` is non-NULL and nothing was captured into it yet. */
typedef struct {
  bool have;
  GbscEntry e;
  uint8_t g3rec80[80];
  GbEditMon written;
  BcMeta meta;
} XrCapture;

static void xr_run_one(const char* tag, const GbEditMon* mon, uint8_t origin, XrCapture* capture) {
  uint8_t cell[80]; GbEditMon written; uint8_t out80[80];
  if (!xr_down_sim(mon, origin, g_xr_serial++, cell, &written, out80)) {
    printf("  SKIP %s (does not convert -- a GB12 refusal, e.g. species/move out of Gen-3 range)\n", tag);
    return;
  }
  BcMeta meta;
  CHECK(bc_unpack(cell, &written, &meta), "%s: bc_unpack for meta", tag);

  GbscEntry e;
  xr_build_entry_asdown(&e, &written, cell, out80);

  GbEditMon merged; XrMergeReport rep;
  CHECK(xr_merge_down(&e, out80, &merged, &rep), "%s: xr_merge_down runs (no abroad edit)", tag);
  xr_check_roundtrip(tag, &written, &merged);
  xr_check_meta_survives(tag, &merged, &meta);
  CHECK(!rep.moves_changed && !rep.level_changed && !rep.renamed,
        "%s: no field reported changed (nothing happened abroad)", tag);
  if (mon->gen == GB_GEN2) {
    uint8_t item = written.rec[0x03];   /* Gen-2 box record: held item byte           */
    if (item != 0) CHECK(merged.rec[0x03] == item, "%s: held item byte comes back (%u)", tag, item);
  }

  if (capture && !capture->have) {
    capture->have = true;
    capture->e = e;
    memcpy(capture->g3rec80, out80, 80);
    capture->written = written;
    capture->meta = meta;
  }
}

static XrCapture g_rt1_capture;   /* first Gen-2 record that converted cleanly */

static void run_rt_gen1(const char* file) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", GB_ROMS, file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP %s (not present)\n", file); return; }
  static uint8_t img[65536];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);
  Gen1Save s;
  if (gen1_open(img, len, &s) != GEN1_OK) { printf("  SKIP %s (open failed)\n", file); return; }
  uint8_t origin = (strcmp(file, "Yellow.sav") == 0) ? BC_ORIGIN_YELLOW : BC_ORIGIN_RED;
  int n = 0;
  for (int box = 0; box <= GEN1_PARTY_BOX; box++) {
    uint32_t off = gen1_list_offset(&s, box);
    int count = gen1_list_count(img + off, box);
    if (count < 0) continue;
    for (int slot = 0; slot < count; slot++) {
      GbEditMon mon;
      char tag[96];
      snprintf(tag, sizeof tag, "RT-2 %s box%d slot%d", file, box, slot);
      if (!gb_load(&mon, GB_GEN1, img + off, box, slot)) continue;
      xr_run_one(tag, &mon, origin, NULL);
      n++;
    }
  }
  printf("  %s: %d Gen-1 record(s) run through RT-2\n", file, n);
}

static void run_rt_gen2(const char* file) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", GB_ROMS, file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP %s (not present)\n", file); return; }
  static uint8_t img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);
  G2Save sv;
  if (!g2_detect(img, len, &sv) || !sv.supported) { printf("  SKIP %s (unsupported)\n", file); return; }
  G2Header hd;
  if (!g2_read_header(img, &sv, &hd)) { printf("  SKIP %s (header)\n", file); return; }
  bool is_crystal = (strcmp(file, "Crystal.sav") == 0);
  uint8_t origin = is_crystal ? BC_ORIGIN_CRYSTAL : BC_ORIGIN_GOLD;
  int n = 0, eggs = 0;
  for (int box = 0; box <= G2_BOX_PARTY; box++) {
    uint32_t off = g2_list_offset(&sv, box, hd.current_box);
    if (off == 0) continue;
    int count = gb_list_count(GB_GEN2, img + off, box);
    if (count < 0) continue;
    for (int slot = 0; slot < count; slot++) {
      GbEditMon mon;
      char tag[96];
      snprintf(tag, sizeof tag, "RT-1 %s box%d slot%d", file, box, slot);
      if (!gb_load(&mon, GB_GEN2, img + off, box, slot)) continue;
      if (mon.list_species == G2_LIST_EGG) {
        eggs++;
        continue;   /* eggs cannot convert (GB12_ERR_EGG) -- see xr_run_egg() below */
      }
      xr_run_one(tag, &mon, origin, &g_rt1_capture);
      n++;
    }
  }
  printf("  %s: %d Gen-2 record(s) run through RT-1 (%d egg(s) skipped -- see xr_run_egg)\n",
        file, n, eggs);
}

/* An egg cannot reach this path for real: gen12_can_convert() refuses every egg
 * (GB12_ERR_EGG) at the S150-8 DOWN edge itself, so a NATIVE_HOME ledger entry for
 * an egg can never actually exist on a card. This is therefore a defensive UNIT
 * test of xr_merge_down alone (declared deviation #vi, not a real production round
 * trip): a real Gen-2 egg is packed straight into a native cell (bc_pack), and the
 * entry's own baseline fields are set to match a REAL Gen-3 corpus record's own
 * decoded species/level/moves/nickname exactly, so xr_merge_down sees nothing
 * changed and must hand the egg back byte for byte -- proving list_species (the
 * Gen-2 egg marker) survives untouched through *out = home. */
static void xr_run_egg(const char* file, const uint8_t g3_stand_in[80]) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", GB_ROMS, file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP RT-3 egg (%s not present)\n", file); return; }
  static uint8_t img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);
  G2Save sv;
  if (!g2_detect(img, len, &sv) || !sv.supported) { printf("  SKIP RT-3 egg (unsupported)\n"); return; }
  G2Header hd;
  if (!g2_read_header(img, &sv, &hd)) { printf("  SKIP RT-3 egg (header)\n"); return; }

  for (int box = 0; box <= G2_BOX_PARTY; box++) {
    uint32_t off = g2_list_offset(&sv, box, hd.current_box);
    if (off == 0) continue;
    int count = gb_list_count(GB_GEN2, img + off, box);
    if (count < 0) continue;
    for (int slot = 0; slot < count; slot++) {
      GbEditMon mon;
      if (!gb_load(&mon, GB_GEN2, img + off, box, slot)) continue;
      if (mon.list_species != G2_LIST_EGG) continue;

      uint8_t cell[80];
      CHECK(bc_pack(&mon, 0, BC_ORIGIN_CRYSTAL, 0, g_xr_serial++, cell) == 0, "RT-3 egg: bc_pack");
      GbEditMon written; BcMeta meta;
      CHECK(bc_unpack(cell, &written, &meta), "RT-3 egg: bc_unpack");

      PkMon standin;
      CHECK(pk_decode_mon(g3_stand_in, false, &standin), "RT-3 egg: stand-in Gen-3 record decodes");
      pk_resolve(&standin);

      GbscEntry e;
      gbsc_entry_from(&e, &written, cell, 0);
      e.kind = XR_KIND_NATIVE_HOME;
      e.state = XR_STATE_PENDING;
      e.direction = XR_DIR_ABROAD_G3;
      e.claimed = 1;
      /* Baselines set to match the stand-in exactly, so nothing reads as "changed". */
      e.species_written = pk_national_no(standin.species);
      e.written_level = standin.level;
      e.has_written_moves = 1;
      for (int i = 0; i < 4; i++) {
        uint16_t mv = standin.moves[i];
        e.moves_written[i] = (mv > 255u) ? 0 : (uint8_t)mv;
      }
      e.ppup_written = (uint8_t)standin.ppBonuses;
      memcpy(e.nick_written, g3_stand_in + 0x08, 10);
      e.nick_written[10] = 0;

      GbEditMon merged; XrMergeReport rep;
      CHECK(xr_merge_down(&e, g3_stand_in, &merged, &rep),
            "RT-3 egg: xr_merge_down runs on a real egg's original80");
      xr_check_roundtrip("RT-3 egg", &written, &merged);
      CHECK(merged.list_species == G2_LIST_EGG, "RT-3 egg: list_species (the egg marker) survives");
      CHECK(!rep.moves_changed && !rep.level_changed && !rep.renamed,
            "RT-3 egg: no field reported changed (baselines matched the stand-in)");
      printf("  RT-3 egg: %s box%d slot%d verified\n", file, box, slot);
      return;   /* one is enough -- this is a named single-record check, not a sweep */
    }
  }
  printf("  SKIP RT-3 egg (no egg found in %s)\n", file);
}

/* ---- MERGE-1/2/3 and REFUSE-1, on the RT-1 capture ---------------------------- */

static void test_merge_and_refuse(void) {
  printf("\n-- D2. MERGE-1/2/3 and REFUSE-1 (BACKLOG #150 S150-8b) --\n");
  if (!g_rt1_capture.have) {
    printf("  SKIP (no Gen-2 record converted cleanly in RT-1 -- corpus absent?)\n");
  } else {
    /* MERGE-1: change one move slot in the CURRENT Gen-3 record (simulating an
     * in-game move change abroad), re-run xr_merge_down, and check ONLY that slot
     * moved -- every other byte of the rebuilt record stays identical to RT-1's. */
    uint8_t edited_g3[80];
    memcpy(edited_g3, g_rt1_capture.g3rec80, 80);
    PkMon base_pk;
    CHECK(pk_decode_mon(edited_g3, false, &base_pk), "MERGE-1: base record decodes");
    /* Pick an in-range Gen-1/2 move different from slot 1's current one. */
    uint16_t new_move = (base_pk.moves[1] == 1) ? 2 : 1;   /* Pound / Karate Chop -- both <=165 */
    EditMon em;
    gen3_edit_load(edited_g3, false, &em);
    em_set_move(&em, 1, new_move);
    gen3_edit_commit(&em, edited_g3);

    GbEditMon merged1; XrMergeReport rep1;
    CHECK(xr_merge_down(&g_rt1_capture.e, edited_g3, &merged1, &rep1),
          "MERGE-1: xr_merge_down runs on the edited record");
    CHECK(rep1.moves_changed, "MERGE-1: moves_changed is true (a real in-game move change)");
    CHECK(gb_get_move(&merged1, 1) == (uint8_t)new_move,
          "MERGE-1: the new move landed in slot 1 (got %u want %u)",
          gb_get_move(&merged1, 1), (uint8_t)new_move);
    for (int i = 0; i < 4; i++) {
      if (i == 1) continue;
      CHECK(gb_get_move(&merged1, i) == gb_get_move(&g_rt1_capture.written, i),
            "MERGE-1: slot %d unaffected", i);
    }
    CHECK(merged1.rec_len == g_rt1_capture.written.rec_len &&
          memcmp(merged1.otname, g_rt1_capture.written.otname, GB_NAME_BYTES) == 0,
          "MERGE-1: every other field stays identical to RT-1's rebuild");

    /* MERGE-2 (the MAKE-LEGAL guard, G-H9): the SAME edited record, but the entry's
     * moves_written[1] is set to the CORRECTED move (as if a MAKE-LEGAL fix wrote
     * it) -- the merge must read this as "nothing changed abroad" for that slot. */
    GbscEntry e_ml = g_rt1_capture.e;
    e_ml.has_written_moves = 1;
    e_ml.moves_written[1] = (uint8_t)new_move;
    GbEditMon merged2; XrMergeReport rep2;
    CHECK(xr_merge_down(&e_ml, edited_g3, &merged2, &rep2),
          "MERGE-2: xr_merge_down runs with a MAKE-LEGAL baseline");
    CHECK(!rep2.moves_changed, "MERGE-2: moves_changed is FALSE -- a MAKE-LEGAL correction, not an in-game change");
    CHECK(gb_get_move(&merged2, 1) == gb_get_move(&g_rt1_capture.written, 1),
          "MERGE-2: the home's original move is kept, not the corrected one");

    /* Then clear has_written_moves (a pre-#150 entry) -- the baseline falls back to
     * the home's own move, so the SAME edited_g3 now DOES read as a real change. */
    GbscEntry e_pre = g_rt1_capture.e;
    e_pre.has_written_moves = 0;
    GbEditMon merged2b; XrMergeReport rep2b;
    CHECK(xr_merge_down(&e_pre, edited_g3, &merged2b, &rep2b),
          "MERGE-2 (pre-#150 fallback): xr_merge_down runs");
    CHECK(rep2b.moves_changed,
          "MERGE-2 (pre-#150 fallback): with no written-moves baseline, the same edit reads as a change");

    /* MERGE-3: a move id above gb_max_move(gen) in slot 2 -- per-slot refusal, the
     * other three slots merge normally. */
    uint8_t illegal_g3[80];
    memcpy(illegal_g3, g_rt1_capture.g3rec80, 80);
    EditMon em3;
    gen3_edit_load(illegal_g3, false, &em3);
    uint16_t illegal_move = (uint16_t)gb_max_move(g_rt1_capture.written.gen) + 1;
    em_set_move(&em3, 2, illegal_move);
    em_set_move(&em3, 1, new_move);   /* also change slot 1, a legal move, to prove it still merges */
    gen3_edit_commit(&em3, illegal_g3);

    GbEditMon merged3; XrMergeReport rep3;
    CHECK(xr_merge_down(&g_rt1_capture.e, illegal_g3, &merged3, &rep3),
          "MERGE-3: xr_merge_down runs on the record with an illegal slot-2 move");
    CHECK(rep3.move_refused[2], "MERGE-3: slot 2 is refused (move id exceeds gb_max_move)");
    CHECK(gb_get_move(&merged3, 2) == gb_get_move(&g_rt1_capture.written, 2),
          "MERGE-3: slot 2 keeps the home's own move");
    CHECK(!rep3.move_refused[1] && gb_get_move(&merged3, 1) == (uint8_t)new_move,
          "MERGE-3: slot 1's legal move change still merges");

    /* MERGE-3b (Fable review F1): move id 332 (> 255 -- (uint8_t)332 == 76, a
     * PLAUSIBLE, in-range move id after truncation) in slot 2 must be refused and
     * slot 2 must keep the home's own move, never silently truncate/apply 76 or
     * delete the slot to 0. This is the exact shape the reviewer's harness caught:
     * a Gen-3 move id above 255 was clamped to 0 BEFORE the "unchanged?" compare,
     * so it read as "changed" and then failed the >gb_max_move check (0 is never
     * > max), landing gb_set_move(out, i, 0) -- the home's real move DELETED. */
    uint8_t illegal_g3b[80];
    memcpy(illegal_g3b, g_rt1_capture.g3rec80, 80);
    EditMon em3b;
    gen3_edit_load(illegal_g3b, false, &em3b);
    em_set_move(&em3b, 2, 332);
    gen3_edit_commit(&em3b, illegal_g3b);

    GbEditMon merged3b; XrMergeReport rep3b;
    CHECK(xr_merge_down(&g_rt1_capture.e, illegal_g3b, &merged3b, &rep3b),
          "MERGE-3b: xr_merge_down runs on the record with move 332 in slot 2");
    CHECK(rep3b.move_refused[2], "MERGE-3b: slot 2 is refused (move 332 exceeds gb_max_move)");
    CHECK(gb_get_move(&merged3b, 2) == gb_get_move(&g_rt1_capture.written, 2),
          "MERGE-3b: slot 2 keeps the home's own move (got %u want %u)",
          gb_get_move(&merged3b, 2), gb_get_move(&g_rt1_capture.written, 2));
    CHECK(gb_get_move(&merged3b, 2) != 0 || gb_get_move(&g_rt1_capture.written, 2) == 0,
          "MERGE-3b: slot 2 was not deleted to 0 unless the home's own slot 2 really was empty");
  }

  /* REFUSE-1: xr_merge_down on an entry whose original80 is a REAL Gen-3 corpus
   * record (not native) returns false and leaves `out` untouched. */
  if (!g_have_g3_sample) {
    printf("  SKIP REFUSE-1 (no Gen-3 corpus record captured)\n");
    return;
  }
  GbscEntry bad_e;
  memset(&bad_e, 0, sizeof bad_e);
  memcpy(bad_e.original80, g_g3_sample_rec, 80);
  bad_e.kind = XR_KIND_NATIVE_HOME;   /* mis-stamped on purpose -- bc_is_native() must still refuse */
  bad_e.direction = XR_DIR_ABROAD_G3;

  GbEditMon sentinel_out;
  memset(&sentinel_out, 0xAA, sizeof sentinel_out);
  GbEditMon out_copy = sentinel_out;
  XrMergeReport rep;
  CHECK(!xr_merge_down(&bad_e, g_g3_sample_rec, &out_copy, &rep),
        "REFUSE-1: xr_merge_down refuses a non-native original80");
  CHECK(memcmp(&out_copy, &sentinel_out, sizeof out_copy) == 0,
        "REFUSE-1: `out` is left untouched (still the sentinel)");
}

/* ---- BACKLOG #150 S150-8b, D-Q1: bank_restore_from_entry() -- the pure core ---- */

static void test_bank_restore_from_entry(void) {
  printf("\n-- D3. bank_restore_from_entry (BACKLOG #150 S150-8b, D-Q1) --\n");
  if (!g_rt1_capture.have) {
    printf("  SKIP (no Gen-2 record converted cleanly in RT-1 -- corpus absent?)\n");
    return;
  }

  uint8_t cell80[80];
  XrMergeReport rep;
  int rc = bank_restore_from_entry(&g_rt1_capture.e, g_rt1_capture.g3rec80, 4242u,
                                   cell80, &rep);
  CHECK(rc == 1, "bank_restore_from_entry: succeeds on a real NATIVE_HOME entry (rc=%d)", rc);
  if (rc == 1) {
    CHECK(bc_is_native(cell80), "bank_restore_from_entry: the rebuilt cell is native");
    GbEditMon back; BcMeta meta;
    CHECK(bc_unpack(cell80, &back, &meta), "bank_restore_from_entry: rebuilt cell unpacks");
    xr_check_roundtrip("bank_restore_from_entry (no abroad edit)", &g_rt1_capture.written, &back);
    CHECK(meta.bank_serial == 4242u, "bank_restore_from_entry: bank_serial is the caller's fresh serial");
  }

  /* bank_serial == 0 -- caller's allocation failed -- must refuse without writing. */
  uint8_t sentinel[80]; memset(sentinel, 0xAA, sizeof sentinel);
  uint8_t cell_copy[80]; memcpy(cell_copy, sentinel, 80);
  int rc0 = bank_restore_from_entry(&g_rt1_capture.e, g_rt1_capture.g3rec80, 0, cell_copy, NULL);
  CHECK(rc0 == -1, "bank_restore_from_entry: bank_serial 0 is refused (rc=%d)", rc0);
  CHECK(memcmp(cell_copy, sentinel, 80) == 0, "bank_restore_from_entry: out_cell80 untouched on refusal");

  /* kind != XR_KIND_NATIVE_HOME -- the defensive "not this edge's job" path. */
  GbscEntry g3home_e = g_rt1_capture.e;
  g3home_e.kind = XR_KIND_G3_HOME;
  int rcg = bank_restore_from_entry(&g3home_e, g_rt1_capture.g3rec80, 4243u, cell_copy, NULL);
  CHECK(rcg == 0, "bank_restore_from_entry: a Gen-3-home entry returns 0, not 1 or -1 (rc=%d)", rcg);

  /* REFUSE-1's mirror: original80 not actually native -- bc_is_native's own belt. */
  if (g_have_g3_sample) {
    GbscEntry bad_e = g_rt1_capture.e;
    memcpy(bad_e.original80, g_g3_sample_rec, 80);
    int rcb = bank_restore_from_entry(&bad_e, g_rt1_capture.g3rec80, 4244u, cell_copy, NULL);
    CHECK(rcb == -1, "bank_restore_from_entry: a non-native original80 is refused (rc=%d)", rcb);
  }
}

/* ---- BACKLOG #150 S150-8b review F2: the RESTORED mark is a real state change --- */

static void test_restored_mark_is_real(void) {
  printf("\n-- D4. the RESTORED mark is a real state change, not a no-op (review F2) --\n");
  if (!g_rt1_capture.have) {
    printf("  SKIP (no Gen-2 record converted cleanly in RT-1 -- corpus absent?)\n");
    return;
  }
  /* xfer_down_write() sets claimed=1 AND state=XR_STATE_PENDING at birth (pdna_gen12.c) --
   * xr_build_entry_asdown() mirrors that exactly, so this entry starts life already
   * claimed=1, state=PENDING, same as a real card entry after the DOWN edge wrote it. */
  GbscEntry e = g_rt1_capture.e;
  CHECK(e.claimed == 1, "precondition: xfer_down_write's own entry starts claimed=1");
  CHECK(e.state == XR_STATE_PENDING, "precondition: xfer_down_write's own entry starts XR_STATE_PENDING");

  uint8_t buf[GBSC_FILE_MAX];
  uint32_t len = (uint32_t)gbsc_init(buf, xr_key_g3(g_rt1_capture.g3rec80));
  int idx = gbsc_add(buf, &len, GBSC_FILE_MAX, &e);
  CHECK(idx >= 0, "the entry adds into a fresh ledger file");

  /* pc_bank_restore_done()'s own idiom: remove -> mutate state -> re-add. */
  GbscEntry got;
  CHECK(gbsc_get(buf, len, idx, &got), "the entry reads back before marking");
  got.state = XR_STATE_RESTORED;
  CHECK(gbsc_remove(buf, &len, idx) == 0, "the entry removes cleanly");
  int idx2 = gbsc_add(buf, &len, GBSC_FILE_MAX, &got);
  CHECK(idx2 >= 0, "the mutated entry re-adds cleanly");

  GbscEntry after;
  CHECK(gbsc_get(buf, len, idx2, &after), "the marked entry reads back");
  CHECK(after.state == XR_STATE_RESTORED,
        "the RESTORED mark is a REAL state change (got %u want %u) -- gbsc_set_claimed "
        "alone would have left this at XR_STATE_PENDING, a byte-for-byte no-op",
        after.state, (unsigned)XR_STATE_RESTORED);
  CHECK(after.claimed == 1, "claimed stays 1 (unchanged by the state mutation)");
  CHECK(after.kind == XR_KIND_NATIVE_HOME, "kind is unaffected by the state mutation");

  /* app_xfer_promote()'s own guard (source/pdna_main.c) is
   * `e.state != XR_STATE_PENDING` -> "no longer matches", give up (fail-safe). A
   * RESTORED entry (3) can never equal PENDING (1) or CLAIMED (2), so that guard,
   * UNCHANGED, already treats a restored entry as "no longer matches" and refuses to
   * promote/undo it -- pinned here structurally since pdna_main.c does not compile
   * on the host. */
  CHECK(after.state != XR_STATE_PENDING,
        "a RESTORED entry is != XR_STATE_PENDING -- app_xfer_promote's guard refuses it (fail-safe)");
  CHECK(after.state != XR_STATE_CLAIMED,
        "a RESTORED entry is != XR_STATE_CLAIMED -- distinguishable from an ordinary claim");
}

/* ---- BACKLOG #150 S150-8b review F4: an unmappable Gen-3 glyph must refuse, ---- */
/* ---- never silently become a literal '?' in the home's nickname.           ---- */

static void test_nickname_unmappable_glyph(void) {
  printf("\n-- D5. an unmappable Gen-3 nickname glyph refuses, never becomes '?' (review F4) --\n");
  if (!g_rt1_capture.have) {
    printf("  SKIP (no Gen-2 record converted cleanly in RT-1 -- corpus absent?)\n");
    return;
  }
  uint8_t g3_bad[80];
  memcpy(g3_bad, g_rt1_capture.g3rec80, 80);
  /* 0x01 has no case in gen3_decode_char() -- its `default:` folds it to '?', the
   * SAME character the legitimate 0xAC decodes to. Change byte 0 of the nickname so
   * it differs from e->nick_written (forcing xr_merge_nickname past its
   * memcmp-unchanged early-out) and terminate right after so the rest of the field
   * does not confuse gb_text_lossy(). */
  g3_bad[0x08] = 0x01;
  g3_bad[0x09] = 0xFF;   /* terminator -- a short, deliberately-glitched nickname */

  GbEditMon merged; XrMergeReport rep;
  CHECK(xr_merge_down(&g_rt1_capture.e, g3_bad, &merged, &rep),
        "F4: xr_merge_down runs on a nickname containing an unmappable glyph (0x01)");
  CHECK(rep.rename_refused,
        "F4: rename_refused fires for an unmappable glyph (0x01), not silently '?'");
  CHECK(memcmp(merged.nick, g_rt1_capture.written.nick, GB_NAME_BYTES) == 0,
        "F4: the home nickname survives byte-for-byte -- never overwritten with '?'");
  CHECK(!rep.renamed, "F4: renamed is NOT set when the rename was refused");
}

/* ---- BACKLOG #150 S150-8b review D1: written_level must reflect a MAKE-LEGAL --- */
/* ---- correction, or a later restore reads it as an in-game level-up.        --- */

/* Mirrors gb_bank_down_gen3()'s own decision-7 MAKE LEGAL step (source/pdna_gen12.c)
 * on every corpus record whose converted Gen-3 level sits below its species'
 * evolution floor: raises the Gen-3 record (gen3_edit) exactly as the shipped path
 * does, THEN (the D1 fix) raises `written` (the ledger's own species_written/
 * written_level source) the same way -- so xfer_down_write's entry records the level
 * ACTUALLY WRITTEN, not the pre-correction one. Then proves the restore reads this
 * as "nothing changed abroad" (level_changed == false) and comes home byte-identical. */
static void sweep_make_legal_gen2(const char* file, uint8_t origin, int* n_checked, int* n_applied) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", GB_ROMS, file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP MERGE-4 %s (not present)\n", file); return; }
  static uint8_t img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);
  G2Save sv;
  if (!g2_detect(img, len, &sv) || !sv.supported) { printf("  SKIP MERGE-4 %s (unsupported)\n", file); return; }
  G2Header hd;
  if (!g2_read_header(img, &sv, &hd)) { printf("  SKIP MERGE-4 %s (header)\n", file); return; }
  if (!pk_evo_have_data()) { printf("  SKIP MERGE-4 %s (no evolutions table linked)\n", file); return; }

  for (int box = 0; box <= G2_BOX_PARTY; box++) {
    uint32_t off = g2_list_offset(&sv, box, hd.current_box);
    if (off == 0) continue;
    int count = gb_list_count(GB_GEN2, img + off, box);
    if (count < 0) continue;
    for (int slot = 0; slot < count; slot++) {
      GbEditMon mon;
      if (!gb_load(&mon, GB_GEN2, img + off, box, slot)) continue;
      if (mon.list_species == G2_LIST_EGG) continue;

      uint8_t cell[80]; GbEditMon written; uint8_t out80[80];
      if (!xr_down_sim(&mon, origin, g_xr_serial++, cell, &written, out80)) continue;
      (*n_checked)++;

      PkMon pk;
      if (!pk_decode_mon(out80, false, &pk)) continue;
      pk_resolve(&pk);
      uint16_t dex = pk_national_no(pk.species);
      int floor = dex ? pk_evo_floor(dex) : PK_EVO_NO_DATA;
      if (floor == PK_EVO_NO_DATA || pk.level >= (uint8_t)floor) continue;   /* does not qualify */
      (*n_applied)++;
      uint8_t to_lvl = (uint8_t)floor;
      GbEditMon home_orig = written;   /* the NATIVE CELL's own bytes (`cell`, already
                                        * packed above) never change -- only `written`
                                        * (the ledger's own source) and `out80` (the
                                        * Gen-3 record) do. Compare the restore against
                                        * THIS, not the post-correction `written`. */

      /* decision 7's exact sequence, plus the D1 fix. */
      EditMon em; gen3_edit_load(out80, false, &em);
      em_set_level(&em, to_lvl);
      gen3_edit_commit(&em, out80);
      (void)gb_set_level(&written, to_lvl);   /* D1 fix */

      GbscEntry e;
      xr_build_entry_asdown(&e, &written, cell, out80);
      CHECK(e.written_level == to_lvl,
            "MERGE-4 %s box%d slot%d: written_level reflects the MAKE-LEGAL level (got %u want %u)",
            file, box, slot, e.written_level, to_lvl);

      /* Unchanged abroad life: g3_rec80 == out80 (the just-corrected record, never
       * further edited). xr_merge_down must see NOTHING changed. */
      GbEditMon merged; XrMergeReport rep;
      CHECK(xr_merge_down(&e, out80, &merged, &rep),
            "MERGE-4 %s box%d slot%d: xr_merge_down runs", file, box, slot);
      CHECK(!rep.level_changed,
            "MERGE-4 %s box%d slot%d: level_changed is FALSE -- a MAKE-LEGAL correction, "
            "not an in-game level-up (D1)", file, box, slot);
      xr_check_roundtrip("MERGE-4", &home_orig, &merged);
    }
  }
}

static void test_merge4_make_legal_written_level(void) {
  printf("\n-- D6. MERGE-4: written_level survives a MAKE-LEGAL DOWN (review D1) --\n");
  int checked = 0, applied = 0;
  sweep_make_legal_gen2("Gold.sav", BC_ORIGIN_GOLD, &checked, &applied);
  sweep_make_legal_gen2("Crystal.sav", BC_ORIGIN_CRYSTAL, &checked, &applied);
  printf("  MERGE-4: %d/%d corpus record(s) needed the MAKE-LEGAL correction (checked %d)\n",
        applied, checked, checked);
}

/* ---- BACKLOG #150 S150-8b review D2: the confirm's item-loss comparison. ------ */
/* pc_bank_restore_up (pdna_box.c) is not host-buildable; this pins the underlying
 * comparison it relies on -- pm.heldItem vs item_g2_to_g3(gb_get_held_item(&home)) --
 * against a real corpus item holder, both unchanged (must read as "nothing lost")
 * and changed (must read as "something lost"). */
static void test_d2_item_confirm_logic(void) {
  printf("\n-- D7. review D2's item-loss comparison, pinned against a real item holder --\n");
  char path[512];
  snprintf(path, sizeof path, "%s/Gold.sav", GB_ROMS);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP (Gold.sav not present)\n"); return; }
  static uint8_t img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);
  G2Save sv;
  if (!g2_detect(img, len, &sv) || !sv.supported) { printf("  SKIP (unsupported)\n"); return; }
  G2Header hd;
  if (!g2_read_header(img, &sv, &hd)) { printf("  SKIP (header)\n"); return; }

  for (int box = 0; box <= G2_BOX_PARTY; box++) {
    uint32_t off = g2_list_offset(&sv, box, hd.current_box);
    if (off == 0) continue;
    int count = gb_list_count(GB_GEN2, img + off, box);
    if (count < 0) continue;
    for (int slot = 0; slot < count; slot++) {
      GbEditMon mon;
      if (!gb_load(&mon, GB_GEN2, img + off, box, slot)) continue;
      if (mon.list_species == G2_LIST_EGG) continue;
      if (gb_get_held_item(&mon) == 0) continue;   /* need an item holder */

      uint8_t cell[80]; GbEditMon written; uint8_t out80[80];
      if (!xr_down_sim(&mon, BC_ORIGIN_GOLD, g_xr_serial++, cell, &written, out80)) continue;

      GbEditMon home; BcMeta meta;
      CHECK(bc_unpack(cell, &home, &meta), "D2: the native cell unpacks");
      uint16_t mapped = item_g2_to_g3(gb_get_held_item(&home));
      if (mapped == 0) continue;   /* this item has no Gen-3 mapping -- try the next holder */

      /* xr_down_sim() (this file's own item-relax stand-in) always zeroes the item on
       * the conversion view -- unlike the real DOWN edge's decision 5/6, it never
       * simulates a TRAVELLED item landing in the Gen-3 record. Do that here: write
       * `mapped` into out80 directly, the exact outcome a real travels=true DOWN
       * would have produced. */
      EditMon em0; gen3_edit_load(out80, false, &em0);
      em_set_item(&em0, mapped);
      gen3_edit_commit(&em0, out80);
      PkMon pm;
      CHECK(pk_decode_mon(out80, false, &pm), "D2: the converted record decodes");
      CHECK(pm.heldItem == mapped, "D2: the simulated travelled item landed (got %u want %u)",
            pm.heldItem, mapped);

      /* Unchanged: the Gen-3 record's own item is exactly what the native cell maps
       * to -- pdna_box.c's OWN g3_item formula (D2's fix), literally reproduced here,
       * must read false. Before D2 this was a bare `pm.heldItem != 0`, which fires
       * for EVERY item-travelling mon even on a nothing-changed restore -- the exact
       * false-positive confirm the review caught. */
      bool g3_item_unchanged = pm.heldItem != 0 && pm.heldItem != mapped;
      CHECK(!g3_item_unchanged,
            "D2: unchanged case -- Gen-3 item (%u) matches the native cell's mapped "
            "item (%u), g3_item must read false (D2's fix)", pm.heldItem, mapped);

      /* Changed: edit the Gen-3 record's item to something that provably differs
       * from `mapped`, and confirm the comparison now reads "something was lost". */
      uint16_t other_item = (mapped == 1) ? 2 : 1;   /* MASTER BALL vs ULTRA BALL --
                                                       * any two distinct real item ids */
      uint8_t edited[80]; memcpy(edited, out80, 80);
      EditMon em; gen3_edit_load(edited, false, &em);
      em_set_item(&em, other_item);
      gen3_edit_commit(&em, edited);
      PkMon pm2;
      CHECK(pk_decode_mon(edited, false, &pm2), "D2: the edited record decodes");
      CHECK(pm2.heldItem == other_item, "D2: the item edit landed");
      bool g3_item_changed = pm2.heldItem != 0 && pm2.heldItem != mapped;
      CHECK(g3_item_changed,
            "D2: changed case -- a distinct Gen-3 item (%u != mapped %u) reads as lost",
            pm2.heldItem, mapped);
      return;   /* one real item holder is enough -- named single-record check */
    }
  }
  printf("  SKIP (no item holder found in Gold.sav)\n");
}

/* ============================================================================ */

int main(int argc, char** argv) {
  printf("== BACKLOG #104 audit: cross-generation round-trip field survey ==\n");
  printf("(no equality assertions -- this is a table, not a regression pin)\n\n");

  printf("-- A. Gen 3 -> GB(Gen 2) -> Gen 3, through gen3_to_gb + gb_sidecar --\n");
  FieldTally ta; tally_init(&ta);
  int gen3_files = 0;
  for (int i = 1; i < argc; i++) { run_gen3_corpus_file(&ta, argv[i]); gen3_files++; }
  if (!gen3_files) printf("  (no Gen-3 saves given on argv -- section A empty)\n");
  tally_report(&ta, "A. Gen3->GB->Gen3 (existing sidecar path)");

  printf("\n");
  test_make_legal_edge_case();

  printf("\n-- B. GB -> Gen 3 -> GB, with NO journal (gen12_convert + gen3_to_gb) --\n");
  FieldTally tb; tally_init(&tb);
  static const char* kGb1[] = { "Red.sav", "Yellow.sav" };
  static const char* kGb2[] = { "Gold.sav", "Crystal.sav" };
  char pathbuf[512];
  for (size_t i = 0; i < sizeof kGb1 / sizeof kGb1[0]; i++) {
    snprintf(pathbuf, sizeof pathbuf, "%s/%s", GB_ROMS, kGb1[i]);
    run_gen1_file(&tb, pathbuf);
  }
  for (size_t i = 0; i < sizeof kGb2 / sizeof kGb2[0]; i++) {
    snprintf(pathbuf, sizeof pathbuf, "%s/%s", GB_ROMS, kGb2[i]);
    run_gen2_file(&tb, pathbuf);
  }
  tally_report(&tb, "B. GB->Gen3->GB (no journal exists)");
  printf("     NOTE: 'held item' above is forced-lost by construction -- GB_GEN2 target\n");
  printf("     paste-down never restores an item (gen3_to_gb's item_dropped is the SAME\n");
  printf("     loss every Gen3->GB transfer takes; a Gen-2-origin item is lost a SECOND\n");
  printf("     time here because gen12_convert -> Gen 3 never wrote it back either).\n");
  printf("     Fields with NO row above but genuinely lost this direction (ability,\n");
  printf("     nature, gender, EVs/stat-exp, ribbons, met data, secret id, Gen-1\n");
  printf("     friendship/pokerus) never existed on the Game Boy side, so there is\n");
  printf("     nothing on either end to compare -- they are not 'preserved', they are\n");
  printf("     simply not observable in this direction's audit.\n");

  printf("\n-- D. BACKLOG #150 S150-8b: xr_merge_down, the RESTORE edge's own round trips --\n");
  printf("== D1. RT-1 (Gen2->Gen3->Gen2) / RT-2 (Gen1->Gen3->Gen1) / RT-3 (item + egg) ==\n");
  for (size_t i = 0; i < sizeof kGb1 / sizeof kGb1[0]; i++) {
    run_rt_gen1(kGb1[i]);
  }
  for (size_t i = 0; i < sizeof kGb2 / sizeof kGb2[0]; i++) {
    run_rt_gen2(kGb2[i]);
  }
  if (g_have_g3_sample) xr_run_egg("Crystal.sav", g_g3_sample_rec);
  else printf("  SKIP RT-3 egg (no Gen-3 corpus record on argv to stand in as g3_rec80)\n");

  test_merge_and_refuse();
  test_bank_restore_from_entry();
  test_restored_mark_is_real();
  test_nickname_unmappable_glyph();
  test_merge4_make_legal_written_level();
  test_d2_item_confirm_logic();

  printf("\n== summary: %d checks, %d fail(s) (fails mean the PIPELINE didn't run --\n"
         "   never that a round trip was lossy; see the tables above for that) ==\n",
         g_check, g_fail);
  return g_fail ? 1 : 0;
}

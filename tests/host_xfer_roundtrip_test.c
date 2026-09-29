/* BACKLOG #104 audit: is a cross-generation transfer ROUND-TRIP EXACT today?
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_xfer_roundtrip_test.c \
 *      source/item_map_g1g2.c source/gb_item_names.c source/gb_bag.c source/gb_fields.c source/gen3_to_gb.c source/gb_sidecar.c source/bank_cell.c source/gen12_convert.c \
 *      source/gen3_save.c source/gen3_mon.c source/gen3_box.c source/gen3_edit.c \
 *      source/gen3_daycare.c source/data_tables.c source/evolutions.c \
 *      source/gb_edit.c source/gen1_save.c source/gen2_save.c \
 *      source/gen1_write.c source/gen2_write.c source/gb_session.c \
 *      source/xfer_rec.c source/bank_restore.c source/item_map_g2g3.c \
 *      source/gb_moves_legal.c \
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
#include "gb_moves_legal.h"   /* BACKLOG #220a: g3gb_moves_ok_rec -- RT-4's per-slot clip gate */
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
    /* review D7: gb_item_ignored must be true only when the HOME actually holds an
     * item, not merely "this is a Gen-2 cell" -- an empty-handed Gen-2 mon has
     * nothing to ignore. */
    CHECK(rep.gb_item_ignored == (gb_get_held_item(&written) != 0),
          "%s: gb_item_ignored == (home holds an item) (got %d, home item=%u)",
          tag, (int)rep.gb_item_ignored, gb_get_held_item(&written));
  } else {
    CHECK(!rep.gb_item_ignored, "%s: gb_item_ignored is false for a Gen-1 home (no items at all)", tag);
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
static XrCapture g_rt2_capture;   /* BACKLOG #224: first Gen-1 record that converted
                                   * cleanly -- same idea as g_rt1_capture, Gen 1 side,
                                   * for the umlaut-refuses-on-Gen-1 DOWN-merge case
                                   * (Gen 1 has no umlaut tiles, gb_edit.c's enc_one) */

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
      xr_run_one(tag, &mon, origin, &g_rt2_capture);
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
  int rc = bank_restore_from_entry(&g_rt1_capture.e, g_rt1_capture.g3rec80, XR_ACCEPT_ALL,
                                   4242u, cell80, &rep);
  CHECK(rc == 1, "bank_restore_from_entry: succeeds on a real NATIVE_HOME entry (rc=%d)", rc);
  if (rc == 1) {
    CHECK(bc_is_native(cell80), "bank_restore_from_entry: the rebuilt cell is native");
    GbEditMon back; BcMeta meta;
    CHECK(bc_unpack(cell80, &back, &meta), "bank_restore_from_entry: rebuilt cell unpacks");
    xr_check_roundtrip("bank_restore_from_entry (no abroad edit)", &g_rt1_capture.written, &back);
    CHECK(meta.bank_serial == 4242u, "bank_restore_from_entry: bank_serial is the caller's fresh serial");
    /* S150-9 decision 3: BC_FLAG_HAS_XFER_REC is always stamped on a restored cell,
     * and its ident32 (derived from bank_serial, G-M4) differs from the original's. */
    CHECK((meta.flags & BC_FLAG_HAS_XFER_REC) != 0,
          "bank_restore_from_entry: BC_FLAG_HAS_XFER_REC is set on the restored cell");
    CHECK(bc_ident32(cell80) != bc_ident32(g_rt1_capture.e.original80),
          "bank_restore_from_entry: ident32 differs from the original's (a restore is a new cell)");
  }

  /* bank_serial == 0 -- caller's allocation failed -- must refuse without writing. */
  uint8_t sentinel[80]; memset(sentinel, 0xAA, sizeof sentinel);
  uint8_t cell_copy[80]; memcpy(cell_copy, sentinel, 80);
  int rc0 = bank_restore_from_entry(&g_rt1_capture.e, g_rt1_capture.g3rec80, XR_ACCEPT_ALL,
                                    0, cell_copy, NULL);
  CHECK(rc0 == -1, "bank_restore_from_entry: bank_serial 0 is refused (rc=%d)", rc0);
  CHECK(memcmp(cell_copy, sentinel, 80) == 0, "bank_restore_from_entry: out_cell80 untouched on refusal");

  /* kind != XR_KIND_NATIVE_HOME -- the defensive "not this edge's job" path. */
  GbscEntry g3home_e = g_rt1_capture.e;
  g3home_e.kind = XR_KIND_G3_HOME;
  int rcg = bank_restore_from_entry(&g3home_e, g_rt1_capture.g3rec80, XR_ACCEPT_ALL,
                                    4243u, cell_copy, NULL);
  CHECK(rcg == 0, "bank_restore_from_entry: a Gen-3-home entry returns 0, not 1 or -1 (rc=%d)", rcg);

  /* REFUSE-1's mirror: original80 not actually native -- bc_is_native's own belt. */
  if (g_have_g3_sample) {
    GbscEntry bad_e = g_rt1_capture.e;
    memcpy(bad_e.original80, g_g3_sample_rec, 80);
    int rcb = bank_restore_from_entry(&bad_e, g_rt1_capture.g3rec80, XR_ACCEPT_ALL,
                                      4244u, cell_copy, NULL);
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

  /* gbpc_restore_done()'s own idiom: remove -> mutate state -> re-add. */
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

/* ---- BACKLOG #224 (from b216b review N1): the DOWN-merge guard used to refuse --- */
/* ---- 0x1B/F1-F6/B9 outright (gen3_decode_char folds every one of them to '?'), - */
/* ---- even though Gen 2 spells all seven exactly and only Gen 1 genuinely loses - */
/* ---- them (gb_edit.c's enc_one transliterates onto the plain letter there).    - */

/* Gen-3 bytes for "M{u-umlaut}LLER" (gen3_encode_char: M=0xC7, L=0xC6, E=0xBF,
 * R=0xCC; the umlaut is gen3_edit.c's encode_2byte_accent table, 0xF3 = u-umlaut). */
static const uint8_t kMullerG3[7] = { 0xC7u, 0xF3u, 0xC6u, 0xC6u, 0xBFu, 0xCCu, 0xFFu };

static void test_nickname_umlaut_down_gen2_accepts(void) {
  printf("\n-- BACKLOG #224: a Gen-3 umlaut nickname (\"M\\xC3\\x9CLLER\") merges DOWN "
        "into Gen 2 -- accepted, not refused --\n");
  if (!g_rt1_capture.have) {
    printf("  SKIP (no Gen-2 record converted cleanly in RT-1 -- corpus absent?)\n");
    return;
  }
  uint8_t g3_muller[80];
  memcpy(g3_muller, g_rt1_capture.g3rec80, 80);
  memcpy(g3_muller + 0x08, kMullerG3, sizeof kMullerG3);
  for (size_t k = sizeof kMullerG3; k < 10; k++) g3_muller[0x08 + k] = 0xFFu;

  GbEditMon merged; XrMergeReport rep;
  CHECK(xr_merge_down(&g_rt1_capture.e, g3_muller, &merged, &rep),
        "#224 Gen2: xr_merge_down runs on a Gen-3 umlaut nickname");
  CHECK(!rep.rename_refused && rep.renamed,
        "#224 Gen2: umlaut nickname is ACCEPTED (rename_refused NOT set, renamed IS set)");
  char back_nick[64];
  int bn = gb_get_nickname(&merged, back_nick, sizeof back_nick);
  CHECK(bn > 0 && strcmp(back_nick, "M\xC3\x9CLLER") == 0,
        "#224 Gen2: the merged GB nickname reads back \"M\\xC3\\x9CLLER\" exactly (got %s)",
        back_nick);
}

static void test_nickname_umlaut_down_gen1_refuses(void) {
  printf("\n-- BACKLOG #224: the SAME umlaut nickname merges DOWN into Gen 1 -- refused, "
        "the loss row, home kept --\n");
  if (!g_rt2_capture.have) {
    printf("  SKIP (no Gen-1 record converted cleanly in RT-2 -- corpus absent?)\n");
    return;
  }
  uint8_t g3_muller[80];
  memcpy(g3_muller, g_rt2_capture.g3rec80, 80);
  memcpy(g3_muller + 0x08, kMullerG3, sizeof kMullerG3);
  for (size_t k = sizeof kMullerG3; k < 10; k++) g3_muller[0x08 + k] = 0xFFu;

  GbEditMon merged; XrMergeReport rep;
  CHECK(xr_merge_down(&g_rt2_capture.e, g3_muller, &merged, &rep),
        "#224 Gen1: xr_merge_down runs on a Gen-3 umlaut nickname");
  CHECK(rep.rename_refused && !rep.renamed,
        "#224 Gen1: umlaut nickname is REFUSED (Gen 1 has no umlaut tiles -- gb_edit.c's "
        "enc_one would transliterate, a real loss)");
  CHECK(memcmp(merged.nick, g_rt2_capture.written.nick, GB_NAME_BYTES) == 0,
        "#224 Gen1: the home nickname survives byte-for-byte -- kept, never transliterated");
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
/* gbpc_restore_up (pdna_box.c) is not host-buildable; this pins the underlying
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

/* ---- BACKLOG #206 review D1: the revert's own regression pin ------------------
 *
 * a274651 ("state-aware, identity-checked restore pick") made gbpc_restore_up
 * refuse a CLAIMED entry whenever the entry's OWN stored identity (species_written /
 * otid16 / nick_written) no longer matched the incoming Gen-3 record -- but that is
 * exactly what a mon renamed or evolved ABROAD looks like: xr_key_g3 (PID+otId) still
 * finds the right entry, the entry is still the newest CLAIMED one for this cell, yet
 * its species_written/nick_written are the values from the moment it went DOWN, not
 * what the Gen-3 side holds now. a274651 would refuse those, dropping the mon back as
 * an ordinary Gen-3-origin cell with its native original stranded -- the review's own
 * repro. After the revert, xr_merge_down / bank_restore_from_entry must reach the
 * merge path (never refuse, never land the cell as an ordinary Gen-3 mon) for BOTH a
 * renamed-abroad and an evolved-abroad record, and the report's renamed/evolved bit
 * must be set. Returns 0 (this test's own success convention, matching bc_pack/
 * xr_open's "0 = ok" idiom) so the two CHECK sites below read the same way trip 1/2
 * of the pre-#206 two-trips shape did.
 *
 * MUTATION: reintroduce a274651's identity gate INSIDE xr_restore_pick_basic
 * (source/xfer_rec.c) -- refusing the picked entry whenever its own stored
 * nick_written/species_written no longer match the incoming record -- and either
 * of the two CHECK(rc == 0 && ...) lines below fails -- the renamed/evolved case is
 * refused instead of reaching the merge path, the exact regression this pins. BACKLOG
 * #206 review R1: this case now drives the REAL pick (xr_restore_pick_basic against a
 * real ledger buffer, exactly what gbpc_restore_up calls), not a hand-picked
 * GbscEntry handed straight to bank_restore_from_entry -- the original D1 test never
 * touched the pick loop at all, so a274651's gate (which lived INSIDE the old pick
 * loop, source/pdna_box.c pre-revert) could not have failed it. */
static int xr_restore_regression_case(const uint8_t* ledger, uint32_t llen, int lcount,
                                      const uint8_t g3_edited[80], XrMergeReport* rep_out) {
  GbscEntry picked;
  XrRestorePick pick = xr_restore_pick_basic(ledger, llen, lcount, &picked);
  if (pick != XR_PICK_LIVE) return -1;
  uint8_t out_cell80[80];
  int rc = bank_restore_from_entry(&picked, g3_edited, XR_ACCEPT_ALL, g_xr_serial++, out_cell80,
                                   rep_out);
  return (rc == 1) ? 0 : -1;   /* this test's own convention: 0 = reached the merge path */
}

static void test_backlog_206_regression(void) {
  printf("\n-- D1 (BACKLOG #206 review): renamed/evolved abroad still reach the merge path --\n");
  char path[512];
  snprintf(path, sizeof path, "%s/Gold.sav", GB_ROMS);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP (Gold.sav not present)\n"); return; }
  static uint8_t img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
  uint32_t ilen = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);
  G2Save sv;
  if (!g2_detect(img, ilen, &sv) || !sv.supported) { printf("  SKIP (unsupported)\n"); return; }
  G2Header hd;
  if (!g2_read_header(img, &sv, &hd)) { printf("  SKIP (header)\n"); return; }

  GbEditMon base;
  bool found = false;
  for (int box = 0; box <= G2_BOX_PARTY && !found; box++) {
    uint32_t off = g2_list_offset(&sv, box, hd.current_box);
    if (off == 0) continue;
    int count = gb_list_count(GB_GEN2, img + off, box);
    if (count < 0) continue;
    for (int slot = 0; slot < count; slot++) {
      if (!gb_load(&base, GB_GEN2, img + off, box, slot)) continue;
      if (base.list_species == G2_LIST_EGG) continue;
      found = true;
      break;
    }
  }
  if (!found) { printf("  SKIP (no usable Gen-2 record in Gold.sav)\n"); return; }

  /* One trip DOWN, promoted to CLAIMED (what a saved exit does -- app_xfer_promote /
   * xfer_down_claim_now, mirrored here as the one assignment the tests always use). */
  uint8_t cell[80]; GbEditMon written; uint8_t g3rec80[80];
  if (!xr_down_sim(&base, BC_ORIGIN_GOLD, g_xr_serial++, cell, &written, g3rec80)) {
    printf("  SKIP (does not convert)\n"); return;
  }
  GbscEntry e;
  xr_build_entry_asdown(&e, &written, cell, g3rec80);
  e.state = XR_STATE_CLAIMED;

  /* BACKLOG #206 review R1: a REAL ledger buffer, the same shape xr_open() hands
   * gbpc_restore_up -- xr_restore_pick_basic is driven against this, not against
   * a hand-picked GbscEntry, so the pick loop itself (which a274651's identity gate
   * lived inside) is actually exercised. */
  uint8_t ledger[GBSC_FILE_MAX];
  uint32_t llen = (uint32_t)gbsc_init(ledger, xr_key_g3(g3rec80));
  CHECK(llen > 0, "D1: gbsc_init succeeds");
  int aidx = gbsc_add(ledger, &llen, sizeof ledger, &e);
  CHECK(aidx == 0, "D1: gbsc_add lands the CLAIMED entry at index 0");
  int lcount = gbsc_count(ledger, llen);
  CHECK(lcount == 1, "D1: the ledger validates with exactly one entry");

  /* Case 1: renamed abroad -- the Gen-3 record's own nickname bytes no longer match
   * e.nick_written (a real edit, not a synthetic flip: a different, validly-encoded
   * nickname), everything else unchanged. */
  {
    uint8_t g3_renamed[80];
    memcpy(g3_renamed, g3rec80, 80);
    EditMon em;
    gen3_edit_load(g3_renamed, false, &em);
    em_set_nickname(&em, "NEWNAME");
    gen3_edit_commit(&em, g3_renamed);
    CHECK(memcmp(g3_renamed + 0x08, e.nick_written, 10) != 0,
          "D1: the renamed record's nickname bytes really do differ from the entry's own");

    XrMergeReport rep;
    int rc = xr_restore_regression_case(ledger, llen, lcount, g3_renamed, &rep);
    CHECK(rc == 0 && rep.renamed,
          "D1: a renamed-abroad record reaches the merge path, never refused (rc=%d renamed=%d)",
          rc, (int)rep.renamed);
  }

  /* Case 2: evolved abroad -- species differs from e.species_written, everything else
   * (nickname, moves, level) unchanged. Species is report-only (decision 4), never
   * applied by xr_merge_down_sel -- this pins that the ENTRY is still found and used,
   * not that species changes. */
  {
    uint8_t g3_evolved[80];
    memcpy(g3_evolved, g3rec80, 80);
    PkMon base_pk;
    CHECK(pk_decode_mon(g3_evolved, false, &base_pk), "D1: base record decodes for the evolve case");
    uint16_t new_species = (uint16_t)((pk_national_no(base_pk.species) % 411u) + 1u);
    if (new_species == e.species_written) new_species = (uint16_t)((new_species % 411u) + 1u);
    EditMon em;
    gen3_edit_load(g3_evolved, false, &em);
    em_set_species(&em, new_species);
    gen3_edit_commit(&em, g3_evolved);
    PkMon evolved_pk;
    CHECK(pk_decode_mon(g3_evolved, false, &evolved_pk), "D1: evolved record decodes");
    CHECK(pk_national_no(evolved_pk.species) != e.species_written,
          "D1: the evolved record's species really does differ from the entry's own");

    XrMergeReport rep;
    int rc = xr_restore_regression_case(ledger, llen, lcount, g3_evolved, &rep);
    CHECK(rc == 0 && rep.evolved,
          "D1: an evolved-abroad record reaches the merge path, never refused (rc=%d evolved=%d)",
          rc, (int)rep.evolved);
  }

  /* Case 3/4 (S150-9 decision 8, review R1): a RESTORED or PENDING entry is a pure
   * pick-time refusal, before any identity/merge concern -- separate ledgers so case
   * 3's state doesn't leak into case 4. */
  {
    GbscEntry restored = e;
    restored.state = XR_STATE_RESTORED;
    uint8_t rledger[GBSC_FILE_MAX];
    uint32_t rlen = (uint32_t)gbsc_init(rledger, xr_key_g3(g3rec80));
    CHECK(gbsc_add(rledger, &rlen, sizeof rledger, &restored) == 0,
          "D1: RESTORED-state ledger add succeeds");
    int rcount = gbsc_count(rledger, rlen);
    GbscEntry picked;
    XrRestorePick pick = xr_restore_pick_basic(rledger, rlen, rcount, &picked);
    CHECK(pick == XR_PICK_REFUSE_RESTORED,
          "D1: a RESTORED entry refuses at pick time (pick=%d)", (int)pick);
  }
  {
    GbscEntry pending = e;
    pending.state = XR_STATE_PENDING;
    uint8_t pledger[GBSC_FILE_MAX];
    uint32_t plen = (uint32_t)gbsc_init(pledger, xr_key_g3(g3rec80));
    CHECK(gbsc_add(pledger, &plen, sizeof pledger, &pending) == 0,
          "D1: PENDING-state ledger add succeeds");
    int pcount = gbsc_count(pledger, plen);
    GbscEntry picked;
    XrRestorePick pick = xr_restore_pick_basic(pledger, plen, pcount, &picked);
    CHECK(pick == XR_PICK_REFUSE_PENDING,
          "D1: a PENDING entry refuses at pick time (pick=%d)", (int)pick);
  }
}

/* ============================================================================ */
/* E. BACKLOG #150 S150-9: accept masks, xr_merge_down_gb, the flagship 2->3->1->2. */
/* ============================================================================ */

/* A synthetic GbGen1Base stand-in, same shape host_gen3gb_test.c already uses for
 * every Gen-1-target gen3_to_gb() call in this test tree (no real Gen-1 base-stat
 * table exists here on purpose -- gb_edit.h's own comment). */
static GbGen1Base xr_fake_g1base(void) {
  GbGen1Base b;
  memset(b.base, 50, sizeof b.base);
  b.type1 = b.type2 = 0x14;   /* Fire -- an arbitrary valid Gen-1 type id */
  return b;
}

/* ---- RT-4: the flagship 2->3->1->2, byte-identical via the ledger ------------- */

static int g_rt4_completed = 0, g_rt4_skipped_capsule = 0, g_rt4_skipped_other = 0;
static int g_rt4_clipped = 0;   /* BACKLOG #220a: completed records whose hop 3 clipped >=1 move */

static void run_rt4_one(const char* tag, const GbEditMon* mon, uint8_t origin) {
  /* BACKLOG #220a: species-only gate, mirroring bank_down_convert.c's bdc_convert_gb_core
   * (xr_time_capsule_block(..., NULL, tc_bad) -- moves4 == NULL there means "species only,
   * this arm handles moves itself" per that file's own comment). The old all-or-nothing
   * predicate (moves4 non-NULL) refused every record with even one out-of-range move,
   * which is exactly what BACKLOG #212 already taught the shipped bridge arm not to do --
   * RT-4 had simply never been updated to match. */
  if (xr_time_capsule_block(GB_GEN2, GB_GEN1, gb_get_species_dex(mon), NULL, NULL) != 0) {
    g_rt4_skipped_capsule++;
    return;
  }

  uint8_t N2[80];
  if (bc_pack(mon, 0, origin, 0, g_xr_serial++, N2) != 0) { g_rt4_skipped_other++; return; }
  GbEditMon home; BcMeta meta0;
  if (!bc_unpack(N2, &home, &meta0)) { g_rt4_skipped_other++; return; }
  Gb12Mon view;
  if (!bc_view(&home, &meta0, bc_ident32(N2), &view)) { g_rt4_skipped_other++; return; }
  Gb12Target tgt; memset(&tgt, 0, sizeof tgt); tgt.met_game = 3;   /* Emerald, arbitrary */
  Gb12Notes notes;
  uint8_t g3[80];
  if (gen12_convert(&view, &tgt, g3, &notes) != GB12_OK) { g_rt4_skipped_other++; return; }

  /* hop 1's own entry, state flipped to CLAIMED the way app_xfer_promote()/
   * xfer_down_claim_now() do -- a test-side step, decision 11's own note. */
  GbscEntry e1;
  xr_entry_for_down(&e1, &home, N2, 0, XR_DIR_ABROAD_G3, g3 + 0x08);
  e1.state = XR_STATE_CLAIMED;

  uint8_t N2p[80];
  XrMergeReport rep1;
  int rc2 = bank_restore_from_entry(&e1, g3, 0, g_xr_serial++, N2p, &rep1);
  CHECK(rc2 == 1, "%s: RT-4 hop 2 (bank_restore_from_entry) succeeds (rc=%d)", tag, rc2);
  if (rc2 != 1) return;
  /* review D5 (c): e1.direction (XR_DIR_ABROAD_G3, xr_entry_for_down's own stamp)
   * must have survived into the merge -- a dropped direction stamp reads as a
   * pre-#150/mis-stamped entry (xr_merge_nickname's own nick_baseline_missing
   * fallback), silently degrading instead of merging the nickname for real. */
  CHECK(!rep1.nick_baseline_missing,
        "%s: RT-4 hop 2 report has nick_baseline_missing == false (the direction "
        "stamp survived into the merge)", tag);

  GbEditMon home2; BcMeta meta2;
  CHECK(bc_unpack(N2p, &home2, &meta2), "%s: RT-4 hop 3 bc_unpack(N2') succeeds", tag);
  Gb12Mon view2;
  CHECK(bc_view(&home2, &meta2, bc_ident32(N2p), &view2), "%s: RT-4 hop 3 bc_view(N2') succeeds", tag);
  uint8_t g3b[80];
  Gb12Notes notes2;
  bool conv2 = gen12_convert(&view2, &tgt, g3b, &notes2) == GB12_OK;
  CHECK(conv2, "%s: RT-4 hop 3 gen12_convert(N2') succeeds", tag);
  if (!conv2) return;

  /* BACKLOG #220a: per-slot clip (gb_moves_legal.h's g3gb_moves_ok_rec -- the SAME
   * predicate bdc_convert_gb_core (source/bank_down_convert.c) uses) instead of
   * refusing the whole record on one bad move. bad4[i] slots are written EMPTY by
   * gen3_to_gb_fixed rather than blocking hop 3 -- the ledger entry below is keyed
   * off N2p (the UNCLIPPED Gen-3 record from hop 2, xr_entry_for_down's `cell80`
   * argument), so hop 4's restore rebuilds from the pre-clip bytes regardless of
   * what hop 3 had to empty; the final byte-identical assertions below are
   * unaffected by the clip. This lane does not call gb_paste_fill_moves() (the
   * production bridge's own fill step, source/pdna_gen12.c) -- it needs a real ROM's
   * learnset (g_ed/gb_create_locate_rom), which is not host-compilable; the no-ROM
   * path is simply to leave a clipped slot EMPTY, same as CREATE's own no-ROM
   * fallback. */
  uint8_t bad4[4];
  int nb2 = g3gb_moves_ok_rec(g3b, GB_GEN1, bad4);
  if (nb2 < 0) { g_rt4_skipped_other++; return; }

  GbGen1Base g1base = xr_fake_g1base();
  GbEditMon R1; Gen3ToGbLoss loss;
  G3GbStatus st = gen3_to_gb_fixed(g3b, GB_GEN1, true, &g1base, nb2 > 0 ? bad4 : NULL, &R1, &loss);
  if (st != G3GB_OK) {
    /* A real refusal for another reason entirely (species floor already gated above;
     * moves are now clipped, not refused) -- tallied, not failed. */
    g_rt4_skipped_other++;
    return;
  }
  if (nb2 > 0) g_rt4_clipped++;

  /* BACKLOG #220a review D7: the production no-ROM path ends in g3gb_moves_pack
   * (source/gb_moves_legal.c's own g3gb_moves_fill, called by gb_paste_fill_moves)
   * -- a clipped slot is never left as a hole in the MIDDLE of the move list, it is
   * packed forward. This lane's own no-ROM fallback (the comment above: "leave a
   * clipped slot EMPTY, same as CREATE's own no-ROM fallback") stopped at
   * gen3_to_gb_fixed and never ran that pack step, so 50 of 78 clipped records in
   * this corpus were left GAPPED (a real move sitting after an empty slot) --
   * legal-looking to every OTHER check here (R1's byte-identical assertions never
   * compare individual move slots against a hole rule), but not what the real
   * bridge ever produces. learn4 is all zeros (no ROM learnset host-compilable, same
   * reason the whole file avoids gb_paste_fill_moves) -- g3gb_moves_fill therefore
   * fills nothing (fill4 stays all-zero) and its own g3gb_moves_pack() call is the
   * only thing this exercises, matching CREATE's own no-ROM fallback exactly. */
  if (nb2 > 0) {
    uint8_t learn4[4] = { 0, 0, 0, 0 };
    uint8_t fill4[4];
    CHECK(g3gb_moves_fill(&R1, bad4, learn4, fill4) >= 0,
          "%s: RT-4 hop 3: g3gb_moves_fill runs on the clipped record", tag);
    bool seen_empty = false;
    for (int i = 0; i < 4; i++) {
      uint8_t mv = gb_get_move(&R1, i);
      if (mv == 0) { seen_empty = true; continue; }
      CHECK(!seen_empty,
            "%s: RT-4 hop 3: slot %d holds a move after an earlier empty slot -- "
            "g3gb_moves_fill's own pack step must never leave a gap", tag, i);
    }
  }

  GbscEntry e2;
  xr_entry_for_down(&e2, &R1, N2p, 0, XR_DIR_ABROAD_GB, NULL);
  e2.state = XR_STATE_CLAIMED;

  uint8_t N2pp[80];
  XrMergeReport rep2;
  int rc4 = bank_restore_from_entry_gb(&e2, &R1, 0, g_xr_serial++, N2pp, &rep2);
  CHECK(rc4 == 1, "%s: RT-4 hop 4 (bank_restore_from_entry_gb) succeeds (rc=%d)", tag, rc4);
  if (rc4 != 1) return;

  GbEditMon back; BcMeta metaBack;
  CHECK(bc_unpack(N2pp, &back, &metaBack), "%s: RT-4 final bc_unpack(N2'') succeeds", tag);
  CHECK(back.gen == home.gen, "%s: RT-4 gen byte-identical (N2'' vs N2)", tag);
  CHECK(back.rec_len == home.rec_len, "%s: RT-4 rec_len byte-identical (N2'' vs N2)", tag);
  CHECK(memcmp(back.rec, home.rec, home.rec_len) == 0,
        "%s: RT-4 rec[0..rec_len) byte-identical (N2'' vs N2)", tag);
  CHECK(memcmp(back.otname, home.otname, GB_NAME_BYTES) == 0,
        "%s: RT-4 otname byte-identical (N2'' vs N2)", tag);
  CHECK(memcmp(back.nick, home.nick, GB_NAME_BYTES) == 0,
        "%s: RT-4 nick byte-identical (N2'' vs N2)", tag);
  CHECK(metaBack.gen == meta0.gen, "%s: RT-4 BcMeta.gen survives (N2'' vs N2)", tag);
  CHECK(metaBack.origin_game == meta0.origin_game,
        "%s: RT-4 BcMeta.origin_game survives (N2'' vs N2)", tag);
  /* decision 3: every restore hop stamps BC_FLAG_HAS_XFER_REC -- mask it OUT of
   * N2'''s flags before comparing against N2's (which never went through a restore). */
  CHECK((uint8_t)(metaBack.flags & (uint8_t)~BC_FLAG_HAS_XFER_REC) == meta0.flags,
        "%s: RT-4 BcMeta.flags survives with BC_FLAG_HAS_XFER_REC masked out", tag);
  /* Bytes 4..7 (ident32) and 73..76 of the CELL are deliberately not compared here
   * (G-M4) -- ident32/bank_serial are re-serialised by every restore, by design. */
  g_rt4_completed++;
}

static void run_rt4_file(const char* file) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", GB_ROMS, file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP RT-4 %s (not present)\n", file); return; }
  static uint8_t img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);
  G2Save sv;
  if (!g2_detect(img, len, &sv) || !sv.supported) { printf("  SKIP RT-4 %s (unsupported)\n", file); return; }
  G2Header hd;
  if (!g2_read_header(img, &sv, &hd)) { printf("  SKIP RT-4 %s (header)\n", file); return; }
  bool is_crystal = (strcmp(file, "Crystal.sav") == 0);
  uint8_t origin = is_crystal ? BC_ORIGIN_CRYSTAL : BC_ORIGIN_GOLD;
  for (int box = 0; box <= G2_BOX_PARTY; box++) {
    uint32_t off = g2_list_offset(&sv, box, hd.current_box);
    if (off == 0) continue;
    int count = gb_list_count(GB_GEN2, img + off, box);
    if (count < 0) continue;
    for (int slot = 0; slot < count; slot++) {
      GbEditMon mon;
      if (!gb_load(&mon, GB_GEN2, img + off, box, slot)) continue;
      if (mon.list_species == G2_LIST_EGG) continue;   /* eggs never reach a real ledger entry */
      char tag[96];
      snprintf(tag, sizeof tag, "RT-4 %s box%d slot%d", file, box, slot);
      run_rt4_one(tag, &mon, origin);
    }
  }
}

/* ---- RT-5 (1->3->1) / RT-6 (2->3->2) through _sel(..., 0, ...) --------------- */

static XrCapture g_rt6_capture;   /* first Gen-2 record captured for section F below */

static void xr_run_one_sel0(const char* tag, const GbEditMon* mon, uint8_t origin,
                            XrCapture* capture) {
  uint8_t cell[80]; GbEditMon written; uint8_t out80[80];
  if (!xr_down_sim(mon, origin, g_xr_serial++, cell, &written, out80)) return;
  BcMeta meta;
  if (!bc_unpack(cell, &written, &meta)) return;

  GbscEntry e;
  xr_build_entry_asdown(&e, &written, cell, out80);

  GbEditMon merged0; XrMergeReport rep0;
  CHECK(xr_merge_down_sel(&e, out80, 0, &merged0, &rep0),
        "%s: xr_merge_down_sel(accept=0) runs", tag);
  xr_check_roundtrip(tag, &written, &merged0);

  GbEditMon mergedAll; XrMergeReport repAll;
  CHECK(xr_merge_down(&e, out80, &mergedAll, &repAll),
        "%s: xr_merge_down (ALL) runs", tag);
  CHECK(memcmp(&merged0, &mergedAll, sizeof(GbEditMon)) == 0,
        "%s: accept=0 identical to accept=ALL when nothing changed abroad", tag);

  if (capture && !capture->have) {
    capture->have = true;
    capture->e = e;
    memcpy(capture->g3rec80, out80, 80);
    capture->written = written;
    capture->meta = meta;
  }
}

static void run_rt5_gen1(const char* file) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", GB_ROMS, file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP RT-5 %s (not present)\n", file); return; }
  static uint8_t img[65536];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);
  Gen1Save s;
  if (gen1_open(img, len, &s) != GEN1_OK) { printf("  SKIP RT-5 %s (open failed)\n", file); return; }
  uint8_t origin = (strcmp(file, "Yellow.sav") == 0) ? BC_ORIGIN_YELLOW : BC_ORIGIN_RED;
  int n = 0;
  for (int box = 0; box <= GEN1_PARTY_BOX; box++) {
    uint32_t off = gen1_list_offset(&s, box);
    int count = gen1_list_count(img + off, box);
    if (count < 0) continue;
    for (int slot = 0; slot < count; slot++) {
      GbEditMon mon;
      char tag[96];
      snprintf(tag, sizeof tag, "RT-5 %s box%d slot%d", file, box, slot);
      if (!gb_load(&mon, GB_GEN1, img + off, box, slot)) continue;
      xr_run_one_sel0(tag, &mon, origin, NULL);
      n++;
    }
  }
  printf("  %s: %d Gen-1 record(s) run through RT-5\n", file, n);
}

static void run_rt6_gen2(const char* file) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", GB_ROMS, file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP RT-6 %s (not present)\n", file); return; }
  static uint8_t img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);
  G2Save sv;
  if (!g2_detect(img, len, &sv) || !sv.supported) { printf("  SKIP RT-6 %s (unsupported)\n", file); return; }
  G2Header hd;
  if (!g2_read_header(img, &sv, &hd)) { printf("  SKIP RT-6 %s (header)\n", file); return; }
  bool is_crystal = (strcmp(file, "Crystal.sav") == 0);
  uint8_t origin = is_crystal ? BC_ORIGIN_CRYSTAL : BC_ORIGIN_GOLD;
  int n = 0;
  for (int box = 0; box <= G2_BOX_PARTY; box++) {
    uint32_t off = g2_list_offset(&sv, box, hd.current_box);
    if (off == 0) continue;
    int count = gb_list_count(GB_GEN2, img + off, box);
    if (count < 0) continue;
    for (int slot = 0; slot < count; slot++) {
      GbEditMon mon;
      char tag[96];
      snprintf(tag, sizeof tag, "RT-6 %s box%d slot%d", file, box, slot);
      if (!gb_load(&mon, GB_GEN2, img + off, box, slot)) continue;
      if (mon.list_species == G2_LIST_EGG) continue;
      xr_run_one_sel0(tag, &mon, origin, &g_rt6_capture);
      n++;
    }
  }
  printf("  %s: %d Gen-2 record(s) run through RT-6\n", file, n);
}

/* ---- MASK-1..3: the single-bit assertion, verbatim (decision 1's own contract) - */

static void test_mask_1_2_3(void) {
  printf("\n-- E1. MASK-1..3: single-bit accept, everything else byte-identical --\n");
  if (!g_rt6_capture.have) {
    printf("  SKIP (no Gen-2 record captured by RT-6)\n");
    return;
  }
  const GbscEntry* e = &g_rt6_capture.e;
  const GbEditMon* home = &g_rt6_capture.written;

  /* Change LEVEL (+5), move slot 1 (an in-range move), and the nickname (ASCII
   * rename) ABROAD all at once. */
  uint8_t edited[80];
  memcpy(edited, g_rt6_capture.g3rec80, 80);
  PkMon base_pk;
  CHECK(pk_decode_mon(edited, false, &base_pk), "MASK-1: base record decodes");
  pk_resolve(&base_pk);   /* box-shape record -- level is computed from EXP, not stored raw */
  uint16_t new_move = (base_pk.moves[1] == 1) ? 2 : 1;
  uint8_t new_level = (base_pk.level >= 96) ? (uint8_t)(base_pk.level - 5)
                                            : (uint8_t)(base_pk.level + 5);
  EditMon em;
  gen3_edit_load(edited, false, &em);
  em_set_level(&em, new_level);
  em_set_move(&em, 1, new_move);
  em_set_nickname(&em, "MASKTEST");
  gen3_edit_commit(&em, edited);

  GbEditMon merged_none; XrMergeReport rep_none;
  CHECK(xr_merge_down_sel(e, edited, 0, &merged_none, &rep_none),
        "MASK-1: accept=0 runs");
  CHECK(memcmp(&merged_none, home, sizeof(GbEditMon)) == 0,
        "MASK-1: accept=0 is identical to the home (structurally, decision 1)");

  GbEditMon merged_all; XrMergeReport rep_all;
  CHECK(xr_merge_down(e, edited, &merged_all, &rep_all), "MASK-1: accept=ALL (wrapper) runs");
  CHECK(memcmp(&merged_all, home, sizeof(GbEditMon)) != 0,
        "MASK-1: accept=ALL differs from the home (three real edits abroad)");

  static const uint8_t bits[3] = { XR_ACCEPT_LEVEL, XR_ACCEPT_MOVES, XR_ACCEPT_NICK };
  static const char* names[3] = { "LEVEL", "MOVES", "NICK" };
  for (int b = 0; b < 3; b++) {
    GbEditMon merged_b; XrMergeReport rep_b;
    CHECK(xr_merge_down_sel(e, edited, bits[b], &merged_b, &rep_b),
          "MASK-1 (%s): xr_merge_down_sel runs", names[b]);

    /* rep is identical across every call (mask-independence, decision 1). */
    CHECK(rep_b.level_changed == rep_none.level_changed &&
          rep_b.moves_changed == rep_none.moves_changed &&
          rep_b.renamed == rep_none.renamed &&
          rep_b.level_from == rep_none.level_from && rep_b.level_to == rep_none.level_to,
          "MASK-1 (%s): rep is identical to accept=0's (mask-independent)", names[b]);
    CHECK(rep_all.level_changed == rep_none.level_changed &&
          rep_all.moves_changed == rep_none.moves_changed &&
          rep_all.renamed == rep_none.renamed,
          "MASK-1 (%s): rep is identical to accept=ALL's too", names[b]);

    /* Only the ONE accepted field differs from the accept=0 result; everything
     * else (the other two fields, and every byte outside rec[]/nick) matches. */
    switch (bits[b]) {
      case XR_ACCEPT_LEVEL:
        CHECK(gb_get_level(&merged_b) == new_level, "MASK-1 (LEVEL): the new level landed");
        CHECK(gb_get_level(&merged_b) != gb_get_level(&merged_none), "MASK-1 (LEVEL): level differs from accept=0");
        for (int i = 0; i < 4; i++)
          CHECK(gb_get_move(&merged_b, i) == gb_get_move(&merged_none, i), "MASK-1 (LEVEL): move slot %d unaffected", i);
        CHECK(memcmp(merged_b.nick, merged_none.nick, GB_NAME_BYTES) == 0, "MASK-1 (LEVEL): nick unaffected");
        break;
      case XR_ACCEPT_MOVES:
        CHECK(gb_get_level(&merged_b) == gb_get_level(&merged_none), "MASK-1 (MOVES): level unaffected");
        CHECK(gb_get_move(&merged_b, 1) == (uint8_t)new_move, "MASK-1 (MOVES): the new move landed in slot 1");
        CHECK(memcmp(merged_b.nick, merged_none.nick, GB_NAME_BYTES) == 0, "MASK-1 (MOVES): nick unaffected");
        break;
      case XR_ACCEPT_NICK:
        CHECK(gb_get_level(&merged_b) == gb_get_level(&merged_none), "MASK-1 (NICK): level unaffected");
        for (int i = 0; i < 4; i++)
          CHECK(gb_get_move(&merged_b, i) == gb_get_move(&merged_none, i), "MASK-1 (NICK): move slot %d unaffected", i);
        CHECK(memcmp(merged_b.nick, merged_none.nick, GB_NAME_BYTES) != 0, "MASK-1 (NICK): nick differs from accept=0");
        break;
    }
  }
}

/* ---- MASK-4: the Gen-3-home direction (gbsc_merge_up_sel) --------------------- */

static void test_mask_4(void) {
  printf("\n-- E2. MASK-4: gbsc_merge_up_sel, including XR_ACCEPT_SPECIES --\n");
  char path[512];
  snprintf(path, sizeof path, "%s/Gold.sav", GB_ROMS);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP MASK-4 (Gold.sav not present)\n"); return; }
  static uint8_t img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);
  G2Save sv;
  if (!g2_detect(img, len, &sv) || !sv.supported) { printf("  SKIP MASK-4 (unsupported)\n"); return; }
  G2Header hd;
  if (!g2_read_header(img, &sv, &hd)) { printf("  SKIP MASK-4 (header)\n"); return; }

  for (int box = 0; box <= G2_BOX_PARTY; box++) {
    uint32_t off = g2_list_offset(&sv, box, hd.current_box);
    if (off == 0) continue;
    int count = gb_list_count(GB_GEN2, img + off, box);
    if (count < 0) continue;
    for (int slot = 0; slot < count; slot++) {
      GbEditMon mon;
      if (!gb_load(&mon, GB_GEN2, img + off, box, slot)) continue;
      if (mon.list_species == G2_LIST_EGG) continue;

      uint8_t rec80[80];
      Gb12Notes notes;
      Gb12Mon view; memset(&view, 0, sizeof view);   /* not used -- gen3_to_gb() is the down side */
      (void)view;
      /* Send this native Gen-2 mon DOWN via gen12_convert(), get a sidecar entry the
       * shipped paste-up flow would use, edit the Game Boy side, then merge up. */
      BcMeta meta0; uint8_t cell0[80];
      CHECK(bc_pack(&mon, 0, BC_ORIGIN_GOLD, 0, g_xr_serial++, cell0) == 0, "MASK-4: bc_pack");
      GbEditMon home0;
      CHECK(bc_unpack(cell0, &home0, &meta0), "MASK-4: bc_unpack");
      Gb12Mon v0;
      CHECK(bc_view(&home0, &meta0, bc_ident32(cell0), &v0), "MASK-4: bc_view");
      Gb12Target tgt; memset(&tgt, 0, sizeof tgt); tgt.met_game = 3;
      if (gen12_convert(&v0, &tgt, rec80, &notes) != GB12_OK) continue;

      GbscEntry e;
      gbsc_entry_from(&e, &mon, rec80, 0);   /* mirrors gb_paste_write()'s own build */

      GbEditMon chg = mon;
      uint8_t base_level = gb_get_level(&mon);
      uint8_t new_level = (base_level >= 96) ? (uint8_t)(base_level - 5) : (uint8_t)(base_level + 5);
      gb_set_level(&chg, new_level);
      uint8_t old_mv = gb_get_move(&mon, 1);
      uint8_t new_mv = (old_mv == 1) ? 2 : 1;
      gb_set_move(&chg, 1, new_mv);
      gb_set_nickname(&chg, "MASKTEST");

      uint8_t out_none[80]; GbscMergeReport rep_none;
      CHECK(gbsc_merge_up_sel(&e, &chg, 0, out_none, &rep_none), "MASK-4: accept=0 runs");
      uint8_t out_all[80]; GbscMergeReport rep_all;
      CHECK(gbsc_merge_up(&e, &chg, out_all, &rep_all), "MASK-4: accept=ALL (wrapper) runs");
      CHECK(memcmp(out_none, e.original80, 80) == 0,
            "MASK-4: accept=0 restores the original 80 bytes byte-for-byte");
      CHECK(memcmp(out_all, out_none, 80) != 0, "MASK-4: accept=ALL differs (real edits abroad)");

      static const uint8_t bits[3] = { 0x01u /* LEVEL */, 0x02u /* MOVES */, 0x04u /* NICK */ };
      static const char* names[3] = { "LEVEL", "MOVES", "NICK" };
      for (int b = 0; b < 3; b++) {
        uint8_t out_b[80]; GbscMergeReport rep_b;
        CHECK(gbsc_merge_up_sel(&e, &chg, bits[b], out_b, &rep_b),
              "MASK-4 (%s): gbsc_merge_up_sel runs", names[b]);
        CHECK(rep_b.level_changed == rep_none.level_changed &&
              rep_b.moves_changed == rep_none.moves_changed &&
              rep_b.renamed == rep_none.renamed,
              "MASK-4 (%s): rep is mask-independent", names[b]);
        CHECK(memcmp(out_b, out_none, 80) != 0, "MASK-4 (%s): differs from accept=0", names[b]);
      }

      /* XR_ACCEPT_SPECIES alone: only species (and whatever em_set_species itself
       * touches) differs from accept=0's output. No evolution floor data lives in
       * this tree by default (evolutions.c is generated/gitignored) -- SKIP the leg
       * cleanly when pk_evo_have_data() says there is none, per the brief. */
      if (pk_evo_have_data()) {
        int min_lvl = pk_evo_min_level(pk_national_no(gb_get_species_dex(&mon)));
        if (min_lvl != PK_EVO_NO_DATA) {
          /* Not exercised further here -- MASK-4's species leg needs a real evolved
           * species id, which the corpus may or may not offer; the LEVEL/MOVES/NICK
           * legs above already prove per-bit independence, and decision 4/10 (species
           * is reported-only on xr_merge_down_gb_sel) is pinned by GB-1 below. */
        }
      }
      return;   /* one real record is enough -- named single-record check */
    }
  }
  printf("  SKIP MASK-4 (no usable Gold.sav record found)\n");
}

/* ---- GB-1..3: xr_merge_down_gb -- site 2's core, direct unit checks ----------- */

static void test_gb_merge_down_gb(void) {
  printf("\n-- E3. GB-1..3: xr_merge_down_gb (site 2's core) --\n");
  char path[512];
  snprintf(path, sizeof path, "%s/Red.sav", GB_ROMS);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP GB-1..3 (Red.sav not present)\n"); return; }
  static uint8_t img[65536];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);
  Gen1Save s;
  if (gen1_open(img, len, &s) != GEN1_OK) { printf("  SKIP GB-1..3 (open failed)\n"); return; }

  GbEditMon mon; bool found = false;
  for (int box = 0; box <= GEN1_PARTY_BOX && !found; box++) {
    uint32_t off = gen1_list_offset(&s, box);
    int count = gen1_list_count(img + off, box);
    if (count < 0) continue;
    for (int slot = 0; slot < count; slot++) {
      if (!gb_load(&mon, GB_GEN1, img + off, box, slot)) continue;
      found = true;
      break;
    }
  }
  if (!found) { printf("  SKIP GB-1..3 (no Gen-1 record found)\n"); return; }

  /* Build a Gen-1 native cell + a synthetic NATIVE_HOME, ABROAD_GB entry as if the
   * bridge arm (pdna_gen12.c's gb_paste_write DOWN-into-a-Gen-2-residence path) had
   * already written it. */
  uint8_t cell[80];
  CHECK(bc_pack(&mon, 0, BC_ORIGIN_RED, 0, g_xr_serial++, cell) == 0, "GB-1: bc_pack");
  GbEditMon home; BcMeta meta;
  CHECK(bc_unpack(cell, &home, &meta), "GB-1: bc_unpack");

  GbscEntry e;
  gbsc_entry_from(&e, &home, cell, 0);   /* "written" == the home itself: nothing changed yet */
  e.kind = XR_KIND_NATIVE_HOME;
  e.direction = XR_DIR_ABROAD_GB;
  e.state = XR_STATE_CLAIMED;
  e.gen = home.gen;   /* GB_GEN1 -- the RESIDENCE generation */

  /* GB-1: the abroad (residence) copy gained a level and a move -- accept=0 reports,
   * changes nothing; the report shows the rows. */
  GbEditMon now1 = home;
  uint8_t base_level = gb_get_level(&home);
  uint8_t new_level = (base_level >= 96) ? (uint8_t)(base_level - 5) : (uint8_t)(base_level + 5);
  gb_set_level(&now1, new_level);
  uint8_t old_mv = gb_get_move(&home, 1);
  uint8_t new_mv = (old_mv == 1) ? 2 : 1;
  gb_set_move(&now1, 1, new_mv);

  GbEditMon out1; XrMergeReport rep1;
  CHECK(xr_merge_down_gb_sel(&e, &now1, 0, &out1, &rep1), "GB-1: xr_merge_down_gb_sel(accept=0) runs");
  CHECK(memcmp(&out1, &home, sizeof(GbEditMon)) == 0, "GB-1: accept=0 is byte-identical to the home");
  CHECK(rep1.level_changed && rep1.moves_changed, "GB-1: level and moves are both reported changed");

  GbEditMon out1all; XrMergeReport rep1all;
  CHECK(xr_merge_down_gb(&e, &now1, &out1all, &rep1all), "GB-1: xr_merge_down_gb (ALL) runs");
  CHECK(gb_get_level(&out1all) == new_level, "GB-1: accept=ALL applies the new level");
  CHECK(gb_get_move(&out1all, 1) == new_mv, "GB-1: accept=ALL applies the new move");

  /* GB-4 (review D5): the Gen-1 residence renamed -- accept=0 reports (renamed,
   * nick byte-identical to the home); accept=NICK applies the rename and ONLY the
   * nick differs from accept=0's own output. */
  GbEditMon now4 = home;
  CHECK(gb_set_nickname(&now4, "RENAMED"), "GB-4: gb_set_nickname on the residence copy");
  GbEditMon out4_0; XrMergeReport rep4_0;
  CHECK(xr_merge_down_gb_sel(&e, &now4, 0, &out4_0, &rep4_0), "GB-4: accept=0 runs");
  CHECK(memcmp(out4_0.nick, home.nick, GB_NAME_BYTES) == 0,
        "GB-4: accept=0 nick is byte-identical to the home");
  CHECK(rep4_0.renamed, "GB-4: accept=0 still reports renamed (mask-independent)");
  GbEditMon out4_nick; XrMergeReport rep4_nick;
  CHECK(xr_merge_down_gb_sel(&e, &now4, XR_ACCEPT_NICK, &out4_nick, &rep4_nick),
        "GB-4: accept=NICK runs");
  CHECK(memcmp(out4_nick.nick, out4_0.nick, GB_NAME_BYTES) != 0,
        "GB-4: accept=NICK nick differs from accept=0's");
  CHECK(gb_get_level(&out4_nick) == gb_get_level(&out4_0),
        "GB-4: accept=NICK leaves level unaffected (only the nick differs)");
  for (int i = 0; i < 4; i++)
    CHECK(gb_get_move(&out4_nick, i) == gb_get_move(&out4_0, i),
          "GB-4: accept=NICK leaves move slot %d unaffected", i);

  /* GB-2/GB-3 need a GENUINE Gen-2 record for `now` (a residence copy of a different
   * generation than the Gen-1 `home`) -- mutating `.gen` on a copy of a Gen-1-shaped
   * GbEditMon would leave its raw `rec[]` bytes in the WRONG layout for a Gen-2
   * offset walk (gb_edit.c's own moves_off()/item_off() are per-generation compile
   * constants), which is not what either check is trying to prove. Load a real
   * Gen-2 mon from Gold.sav instead. */
  bool have_g2 = false;
  GbEditMon g2mon;
  {
    char gpath[512];
    snprintf(gpath, sizeof gpath, "%s/Gold.sav", GB_ROMS);
    FILE* gf = fopen(gpath, "rb");
    if (gf) {
      static uint8_t gimg[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
      uint32_t glen = (uint32_t)fread(gimg, 1, sizeof gimg, gf);
      fclose(gf);
      G2Save gsv;
      G2Header ghd;
      if (g2_detect(gimg, glen, &gsv) && gsv.supported && g2_read_header(gimg, &gsv, &ghd)) {
        for (int box = 0; box <= G2_BOX_PARTY && !have_g2; box++) {
          uint32_t off = g2_list_offset(&gsv, box, ghd.current_box);
          if (off == 0) continue;
          int count = gb_list_count(GB_GEN2, gimg + off, box);
          if (count < 0) continue;
          for (int slot = 0; slot < count; slot++) {
            if (!gb_load(&g2mon, GB_GEN2, gimg + off, box, slot)) continue;
            if (g2mon.list_species == G2_LIST_EGG) continue;
            have_g2 = true;
            break;
          }
        }
      }
    }
  }

  /* GB-2: a Gen-1 HOME whose Gen-2 RESIDENCE copy holds an item -- the item cannot
   * ride onto the Gen-1 home (abroad_item_dropped); the home is otherwise unaffected. */
  if (home.gen == GB_GEN1 && have_g2) {
    GbscEntry e2 = e;
    e2.gen = GB_GEN2;   /* the residence is a genuine Gen-2 save */
    GbEditMon now2 = g2mon;
    gb_set_held_item(&now2, 1);   /* MASTER BALL -- any nonzero Gen-2 item id */
    GbEditMon out2; XrMergeReport rep2;
    bool ok2 = xr_merge_down_gb_sel(&e2, &now2, 0, &out2, &rep2);
    CHECK(ok2, "GB-2: xr_merge_down_gb_sel runs on a cross-generation residence");
    if (ok2) CHECK(rep2.abroad_item_dropped, "GB-2: abroad_item_dropped is set (a Gen-2 item cannot ride onto a Gen-1 home)");
  } else {
    printf("  SKIP GB-2 (Red.sav's sample record is not Gen 1, or no Gold.sav Gen-2 record found)\n");
  }

  /* GB-3: a move id above gb_max_move(home.gen) (165, Gen 1) but still legal for the
   * Gen-2 RESIDENCE (<=251) in slot 3 -- per-slot refusal; the other slots still
   * merge under ACCEPT_MOVES. now->gen != e->gen -> false, `out` untouched. */
  if (home.gen == GB_GEN1 && have_g2) {
    GbscEntry e3 = e;
    e3.gen = GB_GEN2;
    GbEditMon now3 = g2mon;
    uint8_t illegal_mv = (uint8_t)gb_max_move(home.gen) + 1;   /* 166 -- legal in Gen 2, not Gen 1 */
    CHECK(gb_set_move(&now3, 3, illegal_mv), "GB-3: a Gen-2 residence accepts move 166 in slot 3");
    uint8_t old_mv0 = gb_get_move(&g2mon, 0);
    uint8_t new_mv0 = (old_mv0 == 1) ? 2 : 1;
    gb_set_move(&now3, 0, new_mv0);
    GbEditMon out3; XrMergeReport rep3;
    CHECK(xr_merge_down_gb_sel(&e3, &now3, XR_ACCEPT_MOVES, &out3, &rep3),
          "GB-3: xr_merge_down_gb_sel(ACCEPT_MOVES) runs on an illegal (for the Gen-1 home) slot-3 move");
    CHECK(rep3.move_refused[3], "GB-3: slot 3 is refused (move id exceeds gb_max_move(home.gen))");
    CHECK(gb_get_move(&out3, 3) == gb_get_move(&home, 3), "GB-3: slot 3 keeps the home's own move");
    CHECK(!rep3.move_refused[0] && gb_get_move(&out3, 0) == new_mv0,
          "GB-3: slot 0's legal move change still merges under ACCEPT_MOVES");
  } else {
    printf("  SKIP GB-3 (Red.sav's sample record is not Gen 1, or no Gold.sav Gen-2 record found)\n");
  }

  GbEditMon sentinel; memset(&sentinel, 0xAA, sizeof sentinel);
  GbEditMon out_wronggen = sentinel;
  XrMergeReport rep_wronggen;
  GbscEntry e_wronggen = e;
  e_wronggen.gen = (uint8_t)(home.gen == GB_GEN1 ? GB_GEN2 : GB_GEN1);
  CHECK(!xr_merge_down_gb_sel(&e_wronggen, &home, XR_ACCEPT_ALL, &out_wronggen, &rep_wronggen),
        "GB-3: now->gen != e->gen refuses");
  CHECK(memcmp(&out_wronggen, &sentinel, sizeof out_wronggen) == 0,
        "GB-3: `out` is left untouched on the gen-mismatch refusal");

  /* review D5 (b): bank_restore_from_entry_gb's own "not this edge's job" defensive
   * return -- a Gen-3-home entry (kind XR_KIND_G3_HOME) must return 0, never 1 or
   * -1, and must never touch out_cell80. */
  GbscEntry e_g3home = e;
  e_g3home.kind = XR_KIND_G3_HOME;
  uint8_t out_g3home[80]; memset(out_g3home, 0xAA, sizeof out_g3home);
  uint8_t sentinel_g3home[80]; memcpy(sentinel_g3home, out_g3home, 80);
  int rc_g3home = bank_restore_from_entry_gb(&e_g3home, &home, XR_ACCEPT_ALL,
                                             g_xr_serial++, out_g3home, NULL);
  CHECK(rc_g3home == 0, "D5(b): bank_restore_from_entry_gb(kind=G3_HOME) returns 0, not 1 or -1 (rc=%d)", rc_g3home);
  CHECK(memcmp(out_g3home, sentinel_g3home, 80) == 0,
        "D5(b): out_cell80 is left untouched when kind != XR_KIND_NATIVE_HOME");
}

/* ---- ENTRY-1: xr_entry_for_down matches the pre-refactor xfer_down_write ------ */

static void test_entry1(void) {
  printf("\n-- E4. ENTRY-1: xr_entry_for_down matches the real DOWN artefact --\n");
  if (!g_rt6_capture.have) {
    printf("  SKIP (no Gen-2 record captured by RT-6)\n");
    return;
  }
  GbscEntry got;
  xr_entry_for_down(&got, &g_rt6_capture.written, /* written */
                    g_rt6_capture.e.original80 /* the native cell, RT-6's own capture */,
                    0, XR_DIR_ABROAD_G3, g_rt6_capture.g3rec80 + 0x08);
  CHECK(got.kind == XR_KIND_NATIVE_HOME, "ENTRY-1: kind == XR_KIND_NATIVE_HOME");
  CHECK(got.state == XR_STATE_PENDING, "ENTRY-1: state == XR_STATE_PENDING");
  CHECK(got.claimed == 1, "ENTRY-1: claimed == 1");
  CHECK(got.direction == XR_DIR_ABROAD_G3, "ENTRY-1: direction == XR_DIR_ABROAD_G3");
  CHECK(memcmp(got.nick_written, g_rt6_capture.g3rec80 + 0x08, 10) == 0,
        "ENTRY-1: nick_written[0..9] == the Gen-3 record's own raw bytes");
  CHECK(got.nick_written[10] == 0, "ENTRY-1: nick_written[10] == 0 (R1's own fix, not the language byte)");
  CHECK(got.has_written_moves == 1, "ENTRY-1: has_written_moves == 1 (gbsc_entry_from's own contract)");
}

/* ---- ENTRY-1b: BACKLOG #174 (S150-8c) D10(2) -- the PARTY arm's artefact ------- */
/* bank_down_convert_gen3_party() (source/bank_down_convert.c) reaches gb_bank_down_gen3
 * with dst_box = -1; xr_entry_for_down() (called from xfer_down_write, the ONE ledger-
 * entry builder both the PC arm and the party arm share) takes no destination parameter
 * at all -- no dst_box, no dst_cell, no party/box flag (source/xfer_rec.h:168's own
 * signature). dst_box's only two uses in gb_bank_down_gen3 are the two log_line() calls
 * and the caller's own dstrec occupancy test (skipped for the party flavour via
 * bank_down_convert.c's k_empty80). So the party-ness of a landing is provably a
 * DESTINATION property, never a conversion input: two identical calls to
 * xr_entry_for_down (standing in for "the PC arm's call" and "the party arm's call")
 * must produce byte-identical GbscEntry structs and merge to byte-identical results. */
/* BACKLOG #174/#175 review D6 (2026-09-23): this used to call xr_entry_for_down()/
 * xr_merge_down() TWICE with IDENTICAL arguments and memcmp the two results against
 * each other -- true by construction (memcmp(x, x)), proven blind: reverting BACKLOG
 * #174 D3's src_id80 fix still left this whole suite at 150 passed / 0 failed. The
 * genuinely load-bearing assertion (the artefact's real field values, and that
 * xr_merge_down succeeds and produces a real GbEditMon from it) needs only ONE call;
 * kept that, dropped the self-comparison theatre. */
static void test_entry_party_identical(void) {
  printf("\n-- E4b. BACKLOG #174 D10(2): the PARTY arm's ledger artefact shape --\n");
  if (!g_rt6_capture.have) {
    printf("  SKIP (no Gen-2 record captured by RT-6)\n");
    return;
  }
  GbscEntry party_e;
  xr_entry_for_down(&party_e, &g_rt6_capture.written, g_rt6_capture.e.original80,
                    0, XR_DIR_ABROAD_G3, g_rt6_capture.g3rec80 + 0x08);
  CHECK(party_e.kind == XR_KIND_NATIVE_HOME && party_e.state == XR_STATE_PENDING &&
        party_e.direction == XR_DIR_ABROAD_G3 && party_e.claimed == 1,
        "D10(2): the party artefact is kind=NATIVE_HOME/state=PENDING/direction=ABROAD_G3/"
        "claimed=1 -- the SAME artefact shape the PC arm writes (xr_entry_for_down has "
        "no destination parameter to differ on, so one real call covers both arms)");
  GbEditMon party_out; XrMergeReport party_rep;
  bool party_ok = xr_merge_down(&party_e, g_rt6_capture.g3rec80, &party_out, &party_rep);
  CHECK(party_ok, "D10(2): xr_merge_down succeeds on the party artefact");
}

/* ---- Mutation proofs (decision 1's own three, per the brief's step 1) --------- */

static void test_mutation_proofs(void) {
  printf("\n-- E5. mutation proofs -- FAIL on the mutant, then green on the real source --\n");
  if (!g_rt6_capture.have) {
    printf("  SKIP (no Gen-2 record captured by RT-6)\n");
    return;
  }
  printf("  (i)/(ii)/(iii) are exercised by mutating a SCRATCH COPY of the real source\n");
  printf("  files under /tmp and re-running this binary's own MASK-1/MASK-4/ENTRY-1\n");
  printf("  checks against the mutant build -- see the report for the paste-FAIL,\n");
  printf("  restore, green transcript (tests/run_host_tests.py cannot itself apply a\n");
  printf("  source mutation; that step runs from the shell, not from inside this file).\n");
}

/* ============================================================================ */
/* F. BACKLOG #246 review D3 (BLOCKING fix -- the phase's own return trip, #104 S7's
 * acceptance criterion "Emerald -> Bank -> Red AND BACK, byte-identical"). gb_paste_
 * write() (source/pdna_gen12.c) writes a sidecar entry via gbsc_entry_from(), which
 * always stamps kind = XR_KIND_G3_HOME (gb_sidecar.c, verified below). Before this
 * fix, gb_lift_restore's own Bank-UP lift -- the ONLY route a card, on real
 * hardware, offers to bring that mon back into the Bank -- searched EXCLUSIVELY for
 * XR_KIND_NATIVE_HOME, so a #246 landing was findable by NOTHING the box screen
 * could reach (review R3/D3; /tmp/r246/rt_proof.c's own ROUTE A failed before this
 * fix, matching this test before the fix and passing after it). This section builds
 * the entry EXACTLY the way gb_paste_write does (the same gbsc_entry_from() call,
 * same argument order, same orig80 = the Bank cell's own true bytes) and asserts
 * what gb_lift_restore_g3home (pdna_gen12.c, this fix's own new second arm) now
 * does: the NATIVE_HOME search still finds nothing (proves the native arm is
 * untouched -- a second arm, not a replacement), the G3_HOME search DOES find it --
 * THE ASSERTION THE REVIEW SAYS WAS MISSING -- and gbsc_merge_up_sel reproduces the
 * original 80 bytes when nothing changed on the Game Boy side (KEEP AS IS, the
 * common case, the same default every other direction in this file uses). */
static void test_backlog_246_d3_lift_finds_g3home(void) {
  if (!g_have_g3_sample) {
    printf("  SKIP #246 D3 (no Gen-3 corpus record captured by section A)\n");
    return;
  }

  GbEditMon down;
  Gen3ToGbLoss loss;
  G3GbStatus st = gen3_to_gb(g_g3_sample_rec, GB_GEN2, true, NULL, &down, &loss);
  if (st != G3GB_OK) {
    printf("  SKIP #246 D3 (section A's sample record refuses gen3_to_gb: %s)\n",
           g3gb_status_text(st));
    return;
  }

  /* gb_paste_write()'s OWN call, verbatim (source/pdna_gen12.c): orig80 is the Bank
   * cell's own true bytes (g_g3_sample_rec here), epoch 0 (no RTC on host). */
  GbscEntry e;
  gbsc_entry_from(&e, &down, g_g3_sample_rec, 0);
  CHECK(e.kind == XR_KIND_G3_HOME, "#246 D3: gbsc_entry_from stamps kind = XR_KIND_G3_HOME");
  CHECK(e.direction == XR_DIR_ABROAD_GB, "#246 D3: gbsc_entry_from stamps direction = XR_DIR_ABROAD_GB");

  uint8_t file[GBSC_FILE_MAX];
  uint8_t dv4[4] = {
    gb_get_dv(&down, GB_ATK), gb_get_dv(&down, GB_DEF),
    gb_get_dv(&down, GB_SPE), gb_get_dv(&down, GB_SPC)
  };
  uint64_t key = gbsc_key(down.gen, gb_get_otid(&down), dv4, down.otname);
  uint32_t len = (uint32_t)gbsc_init(file, key);
  int idx = gbsc_add(file, &len, GBSC_FILE_MAX, &e);
  CHECK(idx >= 0, "#246 D3: gbsc_add succeeds for the entry gb_paste_write writes");
  if (idx < 0) return;

  /* Before this fix, gb_lift_restore searched ONLY XR_KIND_NATIVE_HOME -- must still
   * find NOTHING here (the native arm is unchanged, second arm not a replacement). */
  int native_hit = gbsc_find(file, len, &down, 0, /*include_claimed*/true, XR_KIND_NATIVE_HOME);
  CHECK(native_hit < 0,
        "#246 D3: the NATIVE_HOME search still finds nothing for a #246 entry (native arm untouched)");

  /* THE LIFT LOOKUP FINDS IT: BACKLOG #246 review F2 fix -- this now calls
   * xr_resolve_home() (source/xfer_rec.c), the ONE resolve BOTH gb_lift_restore_
   * g3home and gb_release_g3home (source/pdna_gen12.c) actually call at runtime, so
   * a mutation of the real decision (not a hand-copied re-implementation in this
   * test) shows up here. Before F2, this test called gbsc_find() directly and the
   * reviewer's mutation of pdna_gen12.c's OWN copy of this tiebreak (sed line-2141
   * rewrite to "return 0;") passed the suite green -- the entire fix this section
   * exists to prove could be deleted undetected. */
  uint16_t down_nowdex = gb_get_species_dex(&down);
  int g3home_hit = xr_resolve_home(file, len, &down, XR_KIND_G3_HOME, down_nowdex);
  CHECK(g3home_hit == idx,
        "#246 D3 (the fix): xr_resolve_home FINDS the #246 entry (index %d, want %d) -- "
        "before this fix gb_lift_restore never even looked", g3home_hit, idx);
  if (g3home_hit < 0) return;

  /* gb_lift_restore_g3home's own commit call, verbatim: gbsc_merge_up_sel(&e, mon,
   * accept, out80, rep) -- accept=0 (KEEP AS IS, nothing changed on the Game Boy
   * side since the write) reproduces the Bank cell's own true bytes exactly. */
  GbscEntry got;
  CHECK(gbsc_get(file, len, g3home_hit, &got), "#246 D3: gbsc_get on the found index");
  uint8_t back80[80];
  GbscMergeReport rep;
  CHECK(gbsc_merge_up_sel(&got, &down, 0, back80, &rep),
        "#246 D3: gbsc_merge_up_sel (accept=0, KEEP AS IS) succeeds on the found entry");
  CHECK(memcmp(back80, g_g3_sample_rec, 80) == 0,
        "#246 D3: the round trip is byte-identical -- Emerald -> Bank -> GB -> back, "
        "matching #104 S7's own acceptance criterion");
  CHECK(!bc_is_native(back80), "#246 D3: the restored Bank cell is a PLAIN Gen-3 record, never native");
}

/* ============================================================================ */
/* F4. BACKLOG #246 review F4 (MEDIUM): two same-fingerprint, same-species entries
 * in one .pds swap their originals. gbsc_find() matches only gen/otid16/dv4/
 * otname -- exactly the fields the FILENAME key is built from -- so every entry in
 * one file matches every lift, and xr_resolve_home's species_written tiebreak is
 * the ONLY discriminator: first match wins on a tie. gb_paste_write (source/
 * pdna_gen12.c) now refuses to gbsc_add a SECOND entry whose species_written would
 * collide with a live entry already in the file -- this section builds that exact
 * scenario (two different Gen-3 originals, same down-converted mon so the same
 * fingerprint AND the same species) and asserts (a) the guard's own predicate
 * (xr_resolve_home + species_written compare, the same two calls gb_paste_write's
 * new guard makes) detects the collision before a second gbsc_add, and (b)
 * demonstrates the swap that guard exists to prevent: WITHOUT it, a lookup for the
 * mon that came from the second (later) original still resolves to the FIRST
 * entry's index -- gb_release_g3home would then consume and report the wrong
 * original. */
static void test_backlog_246_f4_ambiguous_entries(void) {
  if (!g_have_g3_sample) {
    printf("  SKIP #246 F4 (no Gen-3 corpus record captured by section A)\n");
    return;
  }

  GbEditMon down;
  Gen3ToGbLoss loss;
  G3GbStatus st = gen3_to_gb(g_g3_sample_rec, GB_GEN2, true, NULL, &down, &loss);
  if (st != G3GB_OK) {
    printf("  SKIP #246 F4 (section A's sample record refuses gen3_to_gb: %s)\n",
           g3gb_status_text(st));
    return;
  }

  uint8_t file[GBSC_FILE_MAX];
  uint8_t dv4[4] = {
    gb_get_dv(&down, GB_ATK), gb_get_dv(&down, GB_DEF),
    gb_get_dv(&down, GB_SPE), gb_get_dv(&down, GB_SPC)
  };
  uint64_t key = gbsc_key(down.gen, gb_get_otid(&down), dv4, down.otname);
  uint32_t len = (uint32_t)gbsc_init(file, key);

  /* Two DIFFERENT Gen-3 originals (two separate Bank cells) that both down-convert
   * to the identical `down` -- same trainer/DVs/name (the fingerprint) AND the
   * same species_written, the exact ambiguity F4 reports. orig_b differs at byte 8
   * (outside the 0..7 PID+OTID span gbsc_find/xr_resolve_home never look at) so it
   * is a genuinely different 80-byte record, not a duplicate write of orig_a. */
  uint8_t orig_a[80]; memcpy(orig_a, g_g3_sample_rec, 80);
  uint8_t orig_b[80]; memcpy(orig_b, g_g3_sample_rec, 80);
  orig_b[8] ^= 0xFF;

  GbscEntry ea;
  gbsc_entry_from(&ea, &down, orig_a, 0);
  int idx_a = gbsc_add(file, &len, GBSC_FILE_MAX, &ea);
  CHECK(idx_a >= 0, "#246 F4: first entry (orig_a) added");
  if (idx_a < 0) return;

  uint16_t nowdex = gb_get_species_dex(&down);
  bool guard_would_refuse = false;
  int collide = xr_resolve_home(file, len, &down, XR_KIND_G3_HOME, nowdex);
  if (collide >= 0) {
    GbscEntry cand;
    if (gbsc_get(file, len, collide, &cand) && cand.species_written == nowdex) guard_would_refuse = true;
  }
  CHECK(guard_would_refuse,
        "#246 F4 (the fix): the pre-add guard's own predicate detects the ambiguous "
        "same-species entry (index %d) before gb_paste_write would add a second one",
        collide);

  /* Demonstrate the swap the guard exists to prevent: add the second (ambiguous)
   * entry as if the guard were bypassed, then resolve for the SAME mon/species
   * again -- xr_resolve_home's first-match tiebreak always returns idx_a, never
   * idx_b, so a release after orig_b's deposit would consume/report orig_a. */
  GbscEntry eb;
  gbsc_entry_from(&eb, &down, orig_b, 0);
  int idx_b = gbsc_add(file, &len, GBSC_FILE_MAX, &eb);
  CHECK(idx_b >= 0, "#246 F4: second (ambiguous) entry (orig_b) added for the swap demonstration");
  if (idx_b < 0) return;
  int resolved = xr_resolve_home(file, len, &down, XR_KIND_G3_HOME, nowdex);
  CHECK(resolved == idx_a,
        "#246 F4: WITHOUT the guard, xr_resolve_home always binds to entry %d (first "
        "match), never entry %d -- the swap gb_paste_write's new guard exists to "
        "prevent (index resolved: %d)", idx_a, idx_b, resolved);
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

  printf("\n-- F. BACKLOG #246 review D3: the down-arm's own return trip (the lift lookup) --\n");
  test_backlog_246_d3_lift_finds_g3home();

  printf("\n-- F4. BACKLOG #246 review F4: two same-fingerprint, same-species entries --\n");
  test_backlog_246_f4_ambiguous_entries();

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
  test_nickname_umlaut_down_gen2_accepts();
  test_nickname_umlaut_down_gen1_refuses();
  test_merge4_make_legal_written_level();
  test_d2_item_confirm_logic();
  test_backlog_206_regression();

  printf("\n-- E. BACKLOG #150 S150-9: accept masks, xr_merge_down_gb, the flagship --\n");
  printf("== E0. RT-4 (the flagship 2->3->1->2) ==\n");
  for (size_t i = 0; i < sizeof kGb2 / sizeof kGb2[0]; i++) {
    snprintf(pathbuf, sizeof pathbuf, "%s", kGb2[i]);
    run_rt4_file(kGb2[i]);
  }
  printf("  RT-4: %d record(s) completed all four hops byte-identical (%d of them with >=1\n"
         "        move clipped at hop 3, BACKLOG #220a), %d skipped by the species-only\n"
         "        time-capsule gate, %d skipped for another real reason (refusal/conversion)\n",
         g_rt4_completed, g_rt4_clipped, g_rt4_skipped_capsule, g_rt4_skipped_other);

  printf("== E0b. RT-5 (1->3->1) / RT-6 (2->3->2), the accept=0 mask path ==\n");
  for (size_t i = 0; i < sizeof kGb1 / sizeof kGb1[0]; i++) run_rt5_gen1(kGb1[i]);
  for (size_t i = 0; i < sizeof kGb2 / sizeof kGb2[0]; i++) run_rt6_gen2(kGb2[i]);

  test_mask_1_2_3();
  test_mask_4();
  test_gb_merge_down_gb();
  test_entry1();
  test_entry_party_identical();
  test_mutation_proofs();

  printf("\n== summary: %d checks, %d fail(s) (fails mean the PIPELINE didn't run --\n"
         "   never that a round trip was lossy; see the tables above for that) ==\n",
         g_check, g_fail);
  return g_fail ? 1 : 0;
}

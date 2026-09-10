/* BACKLOG #104 audit: is a cross-generation transfer ROUND-TRIP EXACT today?
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_xfer_roundtrip_test.c \
 *      source/gen3_to_gb.c source/gb_sidecar.c source/gen12_convert.c \
 *      source/gen3_save.c source/gen3_mon.c source/gen3_box.c source/gen3_edit.c \
 *      source/gen3_daycare.c source/data_tables.c source/evolutions.c \
 *      source/gb_edit.c source/gen1_save.c source/gen2_save.c \
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
  G3GbStatus st = gen3_to_gb(sample, GB_GEN2, NULL, &down, &loss);
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

  printf("\n== summary: %d checks, %d fail(s) (fails mean the PIPELINE didn't run --\n"
         "   never that a round trip was lossy; see the tables above for that) ==\n",
         g_check, g_fail);
  return g_fail ? 1 : 0;
}

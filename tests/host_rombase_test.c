/* Host test for source/rom_gbbase.c -- Gen-1 BASE STATS read out of the user's own
 * Game Boy cartridge dump, filling the gap gb_edit.h names directly: "There is no
 * Gen-1 base-stat table in this tree and this module will not invent one." rom_gbbase
 * does all its I/O through the caller's GbReadFn, so the code under test here is
 * byte-for-byte the code that runs on the GBA.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_rombase_test.c \
 *      source/rom_gbbase.c source/rom_gbsprite.c source/gb_sprite_codec.c \
 *      source/gen3_to_gb.c \
 *      source/gen3_save.c source/gen3_mon.c source/gen3_box.c source/gen3_edit.c \
 *      source/gen3_daycare.c source/data_tables.c \
 *      source/gb_edit.c source/gb_session.c source/gen1_save.c source/gen1_write.c \
 *      source/gen2_save.c source/gen2_write.c source/gb_sidecar.c -o /tmp/hrombase
 *   /tmp/hrombase /Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/ (.sav files)
 *
 * Coverage:
 *   1) rom_gbbase_gen1 over EVERY dex 1..151, in Red.gb AND Yellow.gb: the row's own
 *      self-check dex byte, a valid Gen-1 type id on both slots (gb_edit.c's
 *      g1_type_ok predicate, mirrored below since it is file-static there), and a
 *      growth rate in 0..5;
 *   2) spot checks against the decomp's own base-stats source (assets/upstream/pokered/
 *      data/pokemon/base_stats/ NAME.asm), independent of rom_gbbase.c's own constants:
 *      Bulbasaur, Charmander (Spc=50, the value gb_edit.h says differs from Gen 3's
 *      60) and Mew (read via mew_stats in Red, the standalone row);
 *   3) negative controls: dex 0 and dex 152 refused, a NULL `out`/`gs`/`read` refused,
 *      and a corrupted dex byte caught by the self-check;
 *   4) THE INTEGRATION S5-C exists for: a handful of real Gen-3 mons (Guy's own .sav
 *      corpus, dex <= 151) run through gen3_to_gb(rec80, GB_GEN1, &rom_base, ...) with
 *      the ROM-READ base table (not the synthetic stand-in
 *      tests/host_gen3gb_test.c uses for its own broader corpus sweep) -> G3GB_OK and
 *      gb_check clean apart from stats_stale; and one of those converted records
 *      actually landing in a real Gen-1 save via gen1_write_apply + gb_verify_slot --
 *      proving a ROM-sourced Gen-1 conversion is accepted by the same engine that
 *      accepts a genuine cartridge write. The save is read into a RAM buffer and never
 *      written back to disk, so Guy's real Red.sav is untouched (same posture as
 *      tests/host_gen3gb_test.c's test_engine_gen1).
 *   5) S5-C Part B1's OWN review ask, on a SEPARATE copy of the same Red.sav image:
 *      the exact production composition PASTE (GB) runs for a Gen-1 target --
 *      gen3_to_gb(GB_GEN1, ROM base) -> gbs_insert() (the GbSession-level call
 *      pdna_gen12.c's gb_paste_write() makes, not the lower-level gen1_write_apply
 *      section 4 exercises) -> gb_load() the landed slot -> gbsc_merge_up() against a
 *      sidecar entry built from the SAME conversion, asserting the merged 80 bytes are
 *      byte-IDENTICAL to the original Gen-3 record -- the sidecar's whole promise,
 *      exercised end to end through the real session API for a Gen-1 target for the
 *      first time.
 *
 * ROMs are Guy's own dumps: they live OUTSIDE the repo and are never copied into it,
 * so a missing corpus SKIPs rather than fails.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "rom_gbbase.h"
#include "rom_gbsprite.h"
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "gen3_edit.h"
#include "data_tables.h"
#include "gen3_to_gb.h"
#include "gen1_write.h"
#include "gb_session.h"
#include "gb_sidecar.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { \
    g_check++; \
    if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
  } while (0)

/* -------------------------------------------------------------- file I/O -------- */

static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FILE* f = (FILE*)ctx;
  if (fseek(f, (long)off, SEEK_SET) != 0) return false;
  if (fread(dst, 1, len, f) != len) return false;
  return true;
}

static uint32_t file_size(const char* p) {
  FILE* f = fopen(p, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fclose(f);
  return (uint32_t)n;
}

/* gb_edit.c's g1_type_ok (source/gb_edit.c:1190) is file-static, so the same set of
 * accepted raw Gen-1 type ids is mirrored here rather than exposed just for a test.
 * 0x00-0x08 physical + 0x09 STEEL, 0x14-0x1A special + 0x1B DARK -- the predicate is a
 * Gen-1 UNION Gen-2 set (pokecrystal/constants/type_constants.asm:19,34 adds STEEL/DARK on
 * top of pokered/constants/type_constants.asm). */
static bool g1_type_ok_mirror(uint8_t t) {
  return t <= 0x09u || (t >= 0x14u && t <= 0x1Bu);
}

/* ============================================================================ */
/* 1/2/3. Every dex in Red and Yellow: self-check, type/growth validity, spot   */
/*        checks against the decomp, and negative controls.                    */
/* ============================================================================ */

static uint8_t g_scratch[4096];

static bool open_rom(const char* file, RomGbSprite* gs, FILE** f_out) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", ROMS, file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP %s (not present)\n", file); return false; }
  uint32_t sz = file_size(path);
  int ok = rom_gbsprite_open(gs, file_read, f, sz, g_scratch, sizeof g_scratch);
  if (!ok || gs->gen != GB_ROM_GEN1) {
    printf("  !! FAIL [%s] rom_gbsprite_open (Gen-1 ROM expected)\n", file);
    g_fail++; g_check++;
    fclose(f);
    return false;
  }
  *f_out = f;
  return true;
}

static void spot_check(const char* label, const RomGb1Species* sp,
                       uint8_t hp, uint8_t atk, uint8_t def, uint8_t spe, uint8_t spc,
                       uint8_t type1, uint8_t type2, uint8_t catch_rate, uint8_t growth) {
  CHECK(sp->base.base[GB_HP]  == hp,  "%s: HP == %u (got %u)",  label, hp,  sp->base.base[GB_HP]);
  CHECK(sp->base.base[GB_ATK] == atk, "%s: Atk == %u (got %u)", label, atk, sp->base.base[GB_ATK]);
  CHECK(sp->base.base[GB_DEF] == def, "%s: Def == %u (got %u)", label, def, sp->base.base[GB_DEF]);
  CHECK(sp->base.base[GB_SPE] == spe, "%s: Spe == %u (got %u)", label, spe, sp->base.base[GB_SPE]);
  CHECK(sp->base.base[GB_SPC] == spc, "%s: Spc == %u (got %u)", label, spc, sp->base.base[GB_SPC]);
  CHECK(sp->base.type1 == type1, "%s: type1 == 0x%02X (got 0x%02X)", label, type1, sp->base.type1);
  CHECK(sp->base.type2 == type2, "%s: type2 == 0x%02X (got 0x%02X)", label, type2, sp->base.type2);
  CHECK(sp->catch_rate == catch_rate, "%s: catch rate == %u (got %u)",
        label, catch_rate, sp->catch_rate);
  CHECK(sp->growth == growth, "%s: growth == %u (got %u)", label, growth, sp->growth);
}

static void test_rom(const char* file) {
  RomGbSprite gs;
  FILE* f;
  if (!open_rom(file, &gs, &f)) return;

  int ok_dex = 0;
  for (uint16_t dex = 1; dex <= 151; dex++) {
    RomGb1Species sp;
    bool ok = rom_gbbase_gen1(&gs, file_read, f, dex, &sp);
    CHECK(ok, "%s: dex %u reads", file, dex);
    if (!ok) continue;
    ok_dex++;
    CHECK(g1_type_ok_mirror(sp.base.type1), "%s: dex %u type1 0x%02X is a valid Gen-1 type id",
          file, dex, sp.base.type1);
    CHECK(g1_type_ok_mirror(sp.base.type2), "%s: dex %u type2 0x%02X is a valid Gen-1 type id",
          file, dex, sp.base.type2);
    CHECK(sp.growth <= 5, "%s: dex %u growth %u is 0..5", file, dex, sp.growth);
  }
  CHECK(ok_dex == 151, "%s: all 151 dex numbers read", file);
  printf("  %s: %d/151 base-stat rows read\n", file, ok_dex);

  /* Spot checks, independently pinned from assets/upstream/pokered/data/pokemon/
   * base_stats/{bulbasaur,charmander,mew}.asm (see this file's header). Growth 3 ==
   * GEN1_GROWTH_MEDIUM_SLOW, verified against constants/pokemon_data_constants.asm's
   * own GrowthRateTable index order (MEDIUM_FAST=0 .. SLOW=5), which gen1_write.h's
   * enum reproduces exactly. */
  RomGb1Species sp;
  if (rom_gbbase_gen1(&gs, file_read, f, 1, &sp))     /* Bulbasaur: db GRASS, POISON */
    spot_check("Bulbasaur", &sp, 45, 49, 49, 45, 65, 0x16 /* GRASS */, 0x03 /* POISON */,
               45, 3);
  if (rom_gbbase_gen1(&gs, file_read, f, 4, &sp))     /* Charmander */
    spot_check("Charmander", &sp, 39, 52, 43, 65, 50, 0x14 /* FIRE */, 0x14 /* FIRE */,
               45, 3);
  if (rom_gbbase_gen1(&gs, file_read, f, 151, &sp))   /* Mew -- via mew_stats in Red */
    spot_check("Mew", &sp, 100, 100, 100, 100, 100, 0x18 /* PSYCHIC */, 0x18 /* PSYCHIC */,
               45, 3);

  /* Negative controls. */
  CHECK(!rom_gbbase_gen1(&gs, file_read, f, 0, &sp), "%s: dex 0 refused", file);
  CHECK(!rom_gbbase_gen1(&gs, file_read, f, 152, &sp), "%s: dex 152 refused", file);
  CHECK(!rom_gbbase_gen1(NULL, file_read, f, 1, &sp), "%s: NULL gs refused", file);
  CHECK(!rom_gbbase_gen1(&gs, NULL, f, 1, &sp), "%s: NULL read refused", file);
  CHECK(!rom_gbbase_gen1(&gs, file_read, f, 1, NULL), "%s: NULL out refused", file);
  {
    RomGbSprite bad = gs;
    bad.gen = GB_ROM_GEN2;
    CHECK(!rom_gbbase_gen1(&bad, file_read, f, 1, &sp), "%s: a non-Gen-1 gs is refused", file);
  }
  {
    /* Corrupt the dex byte the row would read and confirm the self-check catches it:
     * point base_stats one row early, so entry "1" is read from what is really row 0
     * shifted -- i.e. force a self-check mismatch without touching the file. Simpler
     * and just as decisive: ask for a dex whose row, read from an OFF-BY-ONE base,
     * will not carry that dex's byte. */
    RomGbSprite shifted = gs;
    shifted.base_stats = gs.base_stats + 1; /* one byte off the real row boundary */
    RomGb1Species bad_sp;
    CHECK(!rom_gbbase_gen1(&shifted, file_read, f, 2, &bad_sp),
          "%s: an off-by-one table offset is caught by the self-check", file);
  }

  fclose(f);
}

/* ============================================================================ */
/* 4. Integration: ROM-sourced base stats feed gen3_to_gb(), and a converted    */
/*    record lands in a real Gen-1 save.                                       */
/* ============================================================================ */

static bool gb_record_clean(const GbEditMon* e) {
  GbIssues iss;
  if (gb_check(e, &iss)) return true;
  return iss.stats_stale && !iss.species_bad && !iss.list_mismatch && !iss.level_range &&
         !iss.level_exp_bad && !iss.move_empty && !iss.move_hole && !iss.move_range &&
         !iss.move_dup && !iss.pp_over && !iss.pp_on_empty && !iss.gen1_type_bad;
}

static bool     g_have_sample = false;
static int      g_converted = 0;
static uint8_t  g_sample_rec[80];
static GbEditMon g_sample_out;

static void try_convert(const RomGbSprite* gs, FILE* f, const uint8_t* rec) {
  PkMon m;
  if (!pk_decode_mon(rec, false, &m)) return;
  if (m.isBadEgg) return;
  pk_resolve(&m);
  if (m.species < 1 || m.species > 151) return;   /* gen3_to_gb would refuse anyway */

  RomGb1Species sp;
  if (!rom_gbbase_gen1(gs, file_read, f, m.species, &sp)) {
    CHECK(false, "dex %u: rom_gbbase_gen1 must succeed for every 1..151 species", m.species);
    return;
  }

  GbEditMon out;
  Gen3ToGbLoss loss;
  G3GbStatus st = gen3_to_gb(rec, GB_GEN1, &sp.base, &out, &loss);
  if (st != G3GB_OK) {
    /* A handful of species legitimately refuse for reasons unrelated to the base
     * table (a move Gen 1 does not have, an isEgg, ...); only report if the failure
     * is exactly the one this test exists to rule out. */
    CHECK(st != G3GB_ERR_NEEDS_BASE, "dex %u: NEEDS_BASE with a ROM-read table in hand "
          "(%s)", m.species, g3gb_status_text(st));
    return;
  }
  CHECK(gb_record_clean(&out), "dex %u: gb_check clean apart from stats_stale", m.species);
  CHECK(gb_get_dv(&out, GB_ATK) == (m.ivs[1] >> 1), "dex %u: DV Atk == Gen-3 Atk IV / 2", m.species);
  g_converted++;

  if (!g_have_sample) {
    g_have_sample = true;
    memcpy(g_sample_rec, rec, 80);
    g_sample_out = out;
  }
  printf("  dex %3u: %-12s converted via ROM base stats (HP %u Atk %u Def %u Spe %u Spc %u"
         " %s/%s)\n",
         m.species, m.nickname, sp.base.base[GB_HP], sp.base.base[GB_ATK],
         sp.base.base[GB_DEF], sp.base.base[GB_SPE], sp.base.base[GB_SPC],
         g1_type_ok_mirror(sp.base.type1) ? "ok" : "??", g1_type_ok_mirror(sp.base.type2) ? "ok" : "??");
}

static void test_conversion_corpus(const RomGbSprite* gs, FILE* f, int argc, char** argv) {
  printf("== integration: ROM base stats -> gen3_to_gb(GB_GEN1) ==\n");
  int examined = 0;
  for (int i = 1; i < argc && examined < 24; i++) {
    FILE* sf = fopen(argv[i], "rb");
    if (!sf) continue;
    static uint8_t save[G3_SAVE_FILE_SIZE];
    size_t n = fread(save, 1, sizeof save, sf);
    fclose(sf);
    Gen3SaveInfo info;
    if (!gen3_parse(save, (uint32_t)n, &info)) continue;

    static uint8_t sb1[G3_SAVEBLOCK1_BYTES];
    gen3_read_saveblock1(save, info.slot, sb1);
    bool frlg = false;
    PkMon party[6];
    int party_n = pk_read_party_auto(sb1, party, &frlg);
    uint16_t doff = frlg ? 0x0038 : 0x0238;
    for (int p = 0; p < party_n && examined < 24; p++) {
      const uint8_t* rec = sb1 + doff + (uint32_t)p * 100;
      try_convert(gs, f, rec);
      examined++;
    }
  }
  if (examined == 0)
    printf("  (no Gen-3 .sav given on argv -- integration section will be empty)\n");
  printf("  %d record(s) examined, %d converted via ROM base stats\n", examined, g_converted);
  CHECK(examined == 0 || g_converted > 0, "at least one record converted when a corpus was given");
}

static void test_engine_gen1(const RomGbSprite* gs) {
  (void)gs;
  printf("== engine acceptance: insert a ROM-based Gen-1 conversion into a real save ==\n");
  if (!g_have_sample) {
    printf("  SKIP (no sample conversion -- need at least one dex<=151 party mon on argv)\n");
    return;
  }

  char path[512];
  snprintf(path, sizeof path, "%s/Red.sav", ROMS);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP Red.sav (not present)\n"); return; }
  static uint8_t img[GEN1_SAVE_SIZE];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);

  Gen1Save s;
  Gen1Status gst = gen1_open(img, len, &s);
  CHECK(gst == GEN1_OK, "Red.sav: gen1_open (%s)", gen1_status_text(gst));
  if (gst != GEN1_OK) return;

  int roomy = -1;
  for (int b = 0; b < GEN1_NUM_BOXES; b++) {
    if (b == s.current_box) continue;
    int c = gen1_count(&s, b);
    if (c >= 0 && c < GEN1_BOX_CAPACITY) { roomy = b; break; }
  }
  if (roomy < 0) { printf("  Red.sav: SKIP (no box with room)\n"); return; }

  uint8_t rec[GB_MAX_REC], otname[GB_NAME_BYTES], nick[GB_NAME_BYTES], list_sp;
  CHECK(gb_commit_parts(&g_sample_out, rec, otname, nick, &list_sp),
        "Red.sav: extract the sample's raw parts");

  Gen1EditMon e;
  memset(&e, 0, sizeof e);
  e.is_party = false;
  memcpy(e.rec, rec, GEN1_BOX_REC_BYTES);
  memcpy(e.ot, otname, GEN1_NAME_BYTES);
  memcpy(e.nick, nick, GEN1_NAME_BYTES);

  Gen1Op op;
  op.kind = GEN1_OP_INSERT;
  op.box = roomy;
  op.slot = -1;
  op.mon = &e;
  static Gen1WriteScratch scratch;
  Gen1WStatus ws = gen1_write_apply(img, len, &s, &op, &scratch);
  CHECK(ws == GEN1W_OK, "Red.sav: gen1_write_apply INSERT (%s)", gen1_write_status_text(ws));
  if (ws != GEN1W_OK) return;

  Gen1Save s2;
  Gen1Status gst2 = gen1_open(img, len, &s2);
  CHECK(gst2 == GEN1_OK, "Red.sav: re-open the mutated image (%s)", gen1_status_text(gst2));
  if (gst2 == GEN1_OK) {
    uint32_t off = gen1_list_offset(&s2, roomy);
    GbEditMon back;
    CHECK(gb_load(&back, GB_GEN1, img + off, roomy, op.slot), "Red.sav: gb_load the new slot");
    CHECK(gb_verify_slot(&back, img + off, roomy, op.slot),
          "Red.sav: gb_verify_slot on the new slot");
    CHECK(memcmp(back.rec, e.rec, GEN1_BOX_REC_BYTES) == 0,
          "Red.sav: landed record bytes match what was inserted");
  }
  printf("  Red.sav: landed in box %d slot %d (in-memory copy only, never written to disk)\n",
         roomy, op.slot);
}

/* ============================================================================ */
/* 5. S5-C Part B1 review ask: the PRODUCTION composition, on a fresh Red.sav copy --
 *    gen3_to_gb(GB_GEN1, ROM base) -> gbs_insert() -> gb_load() -> gbsc_merge_up()
 *    reproduces the original80 exactly. gb_paste_write() (source/pdna_gen12.c) makes
 *    precisely these calls in precisely this order; this is that composition, minus
 *    the FatFs/UI around it. */
static void test_session_insert_merge_roundtrip(void) {
  printf("== engine acceptance: gbs_insert -> gb_load -> gbsc_merge_up (Gen 1) ==\n");
  if (!g_have_sample) {
    printf("  SKIP (no sample conversion -- need at least one dex<=151 party mon on argv)\n");
    return;
  }

  char path[512];
  snprintf(path, sizeof path, "%s/Red.sav", ROMS);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP Red.sav (not present)\n"); return; }
  static uint8_t img[GEN1_SAVE_SIZE];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);

  /* Pick a box with room the SAME way test_engine_gen1 does, over the raw image --
   * gbs_open() below does not mutate anything until a commit, so this is safe to do
   * before opening the session. */
  Gen1Save probe;
  Gen1Status pst = gen1_open(img, len, &probe);
  CHECK(pst == GEN1_OK, "Red.sav: gen1_open for box selection (%s)", gen1_status_text(pst));
  if (pst != GEN1_OK) return;
  int roomy = -1;
  for (int b = 0; b < GEN1_NUM_BOXES; b++) {
    if (b == probe.current_box) continue;
    int c = gen1_count(&probe, b);
    if (c >= 0 && c < GEN1_BOX_CAPACITY) { roomy = b; break; }
  }
  if (roomy < 0) { printf("  Red.sav: SKIP (no box with room)\n"); return; }

  static uint8_t scratch[GBS_SCRATCH_BYTES];
  GbSession sess;
  GbsStatus ost = gbs_open(&sess, img, len, scratch, sizeof scratch);
  CHECK(ost == GBS_OK, "Red.sav: gbs_open (%s)", gbs_status_text(ost));
  if (ost != GBS_OK) return;

  static uint8_t list[GBS_LIST_BYTES];
  int newslot = -1;
  GbsStatus ist = gbs_insert(&sess, roomy, &g_sample_out, &newslot, list);
  CHECK(ist == GBS_OK, "Red.sav: gbs_insert (%s)", gbs_status_text(ist));
  if (ist != GBS_OK) return;

  GbEditMon back;
  CHECK(gb_load(&back, GB_GEN1, list, roomy, newslot), "Red.sav: gb_load the inserted slot");
  CHECK(gb_verify_slot(&back, list, roomy, newslot), "Red.sav: gb_verify_slot on the inserted slot");

  /* The sidecar entry gb_paste_write() would have written FIRST, before this same
   * gbs_insert() call -- built from the identical (mon, original80) pair. */
  GbscEntry e;
  gbsc_entry_from(&e, &g_sample_out, g_sample_rec, 0);
  uint8_t out80[80];
  GbscMergeReport rep;
  bool mok = gbsc_merge_up(&e, &back, out80, &rep);
  CHECK(mok, "gbsc_merge_up succeeds against the record read back from the save");
  CHECK(memcmp(out80, g_sample_rec, 80) == 0,
        "gbsc_merge_up reproduces the original 80 bytes EXACTLY (nothing changed on the GB side)");
  CHECK(!rep.evolved && !rep.level_changed && !rep.moves_changed && !rep.renamed &&
        !rep.rename_refused && !rep.gb_item_ignored,
        "the merge report shows no changes (round-trip, not an edit)");

  printf("  Red.sav: gbs_insert -> box %d slot %d, gb_load + gbsc_merge_up round-trip exact "
         "(in-memory copy only, never written to disk)\n", roomy, newslot);
}

/* ============================================================================ */

int main(int argc, char** argv) {
  printf("== 1/2/3. rom_gbbase_gen1 over every dex, Red.gb and Yellow.gb ==\n");
  test_rom("Red.gb");
  test_rom("Yellow.gb");

  /* The integration section needs its own open RomGbSprite + FILE*, kept alive across
   * both the conversion sweep and the engine-acceptance insert. */
  RomGbSprite gs;
  FILE* f = NULL;
  if (open_rom("Red.gb", &gs, &f)) {
    test_conversion_corpus(&gs, f, argc, argv);
    test_engine_gen1(&gs);
    test_session_insert_merge_roundtrip();
    fclose(f);
  } else {
    printf("  SKIP integration sections (Red.gb not present)\n");
  }

  printf("\n%s: %d check(s), %d failure(s)\n", g_fail ? "FAIL" : "OK", g_check, g_fail);
  return g_fail ? 1 : 0;
}

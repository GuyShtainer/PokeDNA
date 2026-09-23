/* source/gb_session.c's gbs_insert_party() -- under test (BACKLOG #150 S150-7).
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_gen1party_test.c \
 *      source/gb_session.c source/gb_edit.c source/gen1_save.c source/gen1_write.c \
 *      source/gen2_save.c source/gen2_write.c source/data_tables.c \
 *      source/item_map_g2g3.c source/item_map_g1g2.c source/gb_item_names.c source/gb_bag.c source/gb_fields.c source/gen3_to_gb.c source/gb_sidecar.c source/bank_cell.c source/gen3_edit.c \
 *      source/gen3_mon.c source/gen3_box.c source/gen3_save.c source/gen3_daycare.c \
 *      source/rom_gbbase.c source/rom_gbsprite.c source/gb_sprite_codec.c \
 *      tests/gen12_fixture.c -o /tmp/hg1party && /tmp/hg1party
 *
 * Five checks, per the brief:
 *   1. THE ROUND-TRIP ORACLE -- the real proof. Every occupied party slot of Guy's own
 *      Red.sav/Yellow.sav goes: party record -> box-shape copy (gb_session.c:509-510's
 *      own recipe) -> gbs_insert_party(), with base stats read straight off the matching
 *      ROM (rom_gbbase_gen1) -- and the landed 44 bytes must match the original in every
 *      byte except possibly the ten G1R_STATS bytes. Missing corpus SKIPS; a present
 *      corpus must pass perfectly (host_gbsession_test.c's own posture). Never writes a
 *      corpus file back.
 *   2. Level from EXP, not the stale box byte (D4).
 *   3. HP / status / stored types preserved byte-for-byte (D4).
 *   4. Gen 2 unchanged -- gbs_insert_party and the pre-existing gbs_move(box->party)
 *      produce the SAME 48 bytes for the same source mon (proof the g2_slot_box_to_party
 *      factoring in the previous commit is a factoring, not a rewrite). Real corpus.
 *   5. Refusals before any byte moves: full party, Gen-2 Mail, Gen-1 g1base==NULL,
 *      mon->is_party==true.
 *
 * Checks 2/3/5 use tests/gen12_fixture.c's synthetic images (deterministic, no corpus
 * dependency) rather than Guy's real saves, so they always run.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gb_session.h"
#include "gb_edit.h"
#include "gen1_write.h"
#include "gen12_fixture.h"
#include "rom_gbbase.h"
#include "rom_gbsprite.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { \
    g_check++; \
    if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
  } while (0)

static void wr16be(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t rd16be_local(const uint8_t* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

/* ============================================================================
 * Check 1 -- the round-trip oracle against Guy's real Red.sav / Yellow.sav
 * ========================================================================== */

static uint8_t g_img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
static uint8_t g_scratch[GBS_SCRATCH_BYTES];
static uint8_t g_list[GBS_LIST_BYTES];
static uint8_t g_romscratch[4096];

static uint32_t load_sav(const char* file, uint8_t* dst, uint32_t cap) {
  char p[512];
  snprintf(p, sizeof p, "%s/%s", ROMS, file);
  FILE* f = fopen(p, "rb");
  if (!f) return 0;
  uint32_t n = (uint32_t)fread(dst, 1, cap, f);
  fclose(f);
  return n;
}

static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FILE* f = (FILE*)ctx;
  if (fseek(f, (long)off, SEEK_SET) != 0) return false;
  if (fread(dst, 1, len, f) != len) return false;
  return true;
}

static void oracle_one_slot(GbSession* s, int pb, int slot, RomGbSprite* gs, FILE* romf,
                            const char* savname) {
  Gen1EditMon orig;
  CHECK(gen1_edit_load(g_list, pb, slot, &orig), "%s slot %d: party record loads", savname, slot);
  uint8_t orig44[GEN1_PARTY_REC_BYTES];
  memcpy(orig44, orig.rec, GEN1_PARTY_REC_BYTES);

  /* Box-shape copy, exactly gb_session.c:509-510's recipe. */
  Gen1EditMon boxrec = orig;
  boxrec.is_party = false;
  boxrec.rec[G1R_BOXLEVEL] = boxrec.rec[G1R_LEVEL];

  GbEditMon mon;
  CHECK(gb_load_parts(&mon, GB_GEN1, false, boxrec.rec, boxrec.ot, boxrec.nick,
                      boxrec.rec[G1R_SPECIES]),
        "%s slot %d: box-shape GbEditMon builds", savname, slot);

  uint16_t dex = gb_get_species_dex(&mon);
  RomGb1Species sp;
  bool have_base = dex && rom_gbbase_gen1(gs, file_read, romf, dex, &sp);
  CHECK(have_base, "%s slot %d: dex %u base stats read off the ROM", savname, slot, dex);
  if (!have_base) return;

  /* Free room in the party for the re-insert -- delete the slot we just snapshotted. */
  CHECK(gbs_delete(s, pb, slot, g_list) == GBS_OK, "%s slot %d: freed for the re-insert",
        savname, slot);

  int slot_out = -1;
  GbsStatus st = gbs_insert_party(s, &mon, &sp.base, &slot_out, g_list);
  CHECK(st == GBS_OK, "%s slot %d: gbs_insert_party lands (%s)", savname, slot, gbs_status_text(st));
  if (st != GBS_OK) return;

  Gen1EditMon landed;
  CHECK(gen1_edit_load(g_list, pb, slot_out, &landed), "%s slot %d: landed record reloads",
        savname, slot);

  /* Every byte outside G1R_STATS..+10 must be identical. */
  int mismatches = 0;
  for (int i = 0; i < GEN1_PARTY_REC_BYTES; i++) {
    if (i >= G1R_STATS && i < G1R_STATS + 10) continue;
    if (landed.rec[i] != orig44[i]) {
      mismatches++;
      printf("  !! FAIL: %s slot %d: byte %d differs (orig 0x%02X, landed 0x%02X)\n",
             savname, slot, i, orig44[i], landed.rec[i]);
    }
  }
  g_check++; if (mismatches) g_fail++;
  CHECK(memcmp(landed.ot,   orig.ot,   GB_NAME_BYTES) == 0, "%s slot %d: OT name identical, incl. past the terminator", savname, slot);
  CHECK(memcmp(landed.nick, orig.nick, GB_NAME_BYTES) == 0, "%s slot %d: nickname identical, incl. past the terminator", savname, slot);

  /* Stats: must match unless the ORIGINAL was already not self-consistent with the
   * formula (a stat-hacked corpus mon) -- checked by independently re-deriving them
   * off the ORIGINAL's own inputs via the same primitives gbs_insert_party composes
   * (gb_set_gen1_base + gb_recalc_stats), never by trusting the landed record itself. */
  if (memcmp(landed.rec + G1R_STATS, orig44 + G1R_STATS, 10) != 0) {
    Gen1EditMon formula = orig;   /* party-shape, ORIGINAL level/DV/statexp */
    GbEditMon fmon;
    if (gb_load_parts(&fmon, GB_GEN1, true, formula.rec, formula.ot, formula.nick,
                      formula.rec[G1R_SPECIES]) &&
        gb_set_gen1_base(&fmon, &sp.base) && gb_recalc_stats(&fmon)) {
      bool orig_self_consistent = memcmp(fmon.rec + G1R_STATS, orig44 + G1R_STATS, 10) == 0;
      if (orig_self_consistent) {
        CHECK(false, "%s slot %d dex %u: stats differ from a SELF-CONSISTENT original "
              "(orig stats vs formula agreed, landed did not) -- D4's model of retail is wrong",
              savname, slot, dex);
      } else {
        printf("  NOTE %s slot %d dex %u: original party record is not stat-formula-"
               "consistent (stat-hacked?) -- landed record matches the FORMULA, not the "
               "stored original, which is the documented behaviour\n", savname, slot, dex);
        CHECK(memcmp(landed.rec + G1R_STATS, fmon.rec + G1R_STATS, 10) == 0,
              "%s slot %d dex %u: landed stats match the independently re-derived formula",
              savname, slot, dex);
      }
    } else {
      CHECK(false, "%s slot %d dex %u: could not independently re-derive formula stats "
            "to classify the stat mismatch", savname, slot, dex);
    }
  }
}

static void test_oracle(const char* savfile, const char* romfile) {
  uint32_t len = load_sav(savfile, g_img, sizeof g_img);
  if (!len) { printf("  SKIP %s (not present)\n", savfile); return; }

  char rompath[512];
  snprintf(rompath, sizeof rompath, "%s/%s", ROMS, romfile);
  FILE* romf = fopen(rompath, "rb");
  if (!romf) { printf("  SKIP %s (%s not present)\n", savfile, romfile); return; }
  fseek(romf, 0, SEEK_END);
  uint32_t romsz = (uint32_t)ftell(romf);
  RomGbSprite gs;
  if (!rom_gbsprite_open(&gs, file_read, romf, romsz, g_romscratch, sizeof g_romscratch, GB_ROM_NONE) ||
      gs.gen != GB_ROM_GEN1) {
    printf("  SKIP %s (%s did not open as a Gen-1 ROM)\n", savfile, romfile);
    fclose(romf);
    return;
  }

  GbSession s;
  CHECK(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: session opens", savfile);
  int pb = gbs_party_box(&s);
  CHECK(gbs_load_list(&s, pb, g_list) == GBS_OK, "%s: party loads", savfile);
  int count = gb_list_count(GB_GEN1, g_list, pb);
  CHECK(count >= 0, "%s: party count reads", savfile);
  printf("  -- oracle: %s (%d party member(s))\n", savfile, count);

  /* Snapshot the slots FIRST (deleting slot 0 repeatedly shifts the others down). */
  for (int slot = 0; slot < count; slot++) {
    /* Re-load the list fresh each time: the previous iteration's delete+insert already
     * committed to the image, and slot 0 is always "the next untested member" once the
     * earlier ones have been deleted-and-reinserted (reinsert appends at the END). */
    CHECK(gbs_load_list(&s, pb, g_list) == GBS_OK, "%s: party reloads before slot %d", savfile, slot);
    oracle_one_slot(&s, pb, 0, &gs, romf, savfile);
  }
  fclose(romf);
}

/* ============================================================================
 * Checks 2/3/5 -- synthetic fixture (deterministic, no corpus dependency)
 * ========================================================================== */

static uint8_t g_fimg[GBF_MAX_BYTES];
static uint8_t g_fsnap[GBF_MAX_BYTES];
static uint8_t g_fscratch[GBS_SCRATCH_BYTES];
static uint8_t g_flist[GBS_LIST_BYTES];

static const GbGen1Base k_fake_g1base = { {45, 49, 49, 45, 65}, 0x16, 0x03 }; /* Bulbasaur-shaped */

static void build_g1_box_rec(uint8_t rec33[GEN1_BOX_REC_BYTES], uint16_t dex, uint32_t exp,
                             uint8_t boxlevel, uint16_t hp, uint8_t status,
                             uint8_t type1, uint8_t type2) {
  memset(rec33, 0, GEN1_BOX_REC_BYTES);
  rec33[G1R_SPECIES] = gb_index_from_dex(GB_GEN1, dex);
  wr16be(rec33 + G1R_HP, hp);
  rec33[G1R_BOXLEVEL] = boxlevel;
  rec33[G1R_STATUS] = status;
  rec33[G1R_TYPE1] = type1;
  rec33[G1R_TYPE2] = type2;
  rec33[G1R_CATCH_RATE] = 45;
  wr16be(rec33 + G1R_OTID, 24601);
  rec33[G1R_EXP + 0] = (uint8_t)(exp >> 16);
  rec33[G1R_EXP + 1] = (uint8_t)(exp >> 8);
  rec33[G1R_EXP + 2] = (uint8_t)exp;
}

static void test_level_from_exp(void) {
  printf("  -- check 2: level from EXP, not the stale box byte\n");
  uint32_t len = gbf_build(GBF_RBY, g_fimg, 0);
  GbSession s; memset(&s, 0, sizeof s);
  CHECK(gbs_open(&s, g_fimg, len, g_fscratch, sizeof g_fscratch) == GBS_OK, "level test: session opens");

  const uint16_t dex = 25;   /* Pikachu */
  uint32_t exp_for_40 = gb_exp_for_level(dex, 40);
  uint8_t rec33[GEN1_BOX_REC_BYTES];
  build_g1_box_rec(rec33, dex, exp_for_40, /*boxlevel=*/10, /*hp=*/50, /*status=*/0, 0x18, 0x18);

  uint8_t ot[GB_NAME_BYTES], nick[GB_NAME_BYTES];
  memset(ot, 0x50, sizeof ot); memset(nick, 0x50, sizeof nick);
  GbEditMon mon;
  CHECK(gb_load_parts(&mon, GB_GEN1, false, rec33, ot, nick, rec33[G1R_SPECIES]),
        "level test: box mon builds");

  int pb = gbs_party_box(&s);
  int slot_out = -1;
  GbsStatus st = gbs_insert_party(&s, &mon, &k_fake_g1base, &slot_out, g_flist);
  CHECK(st == GBS_OK, "level test: insert lands (%s)", gbs_status_text(st));
  if (st != GBS_OK) return;

  Gen1EditMon landed;
  CHECK(gen1_edit_load(g_flist, pb, slot_out, &landed), "level test: landed reloads");
  uint8_t expect_level = gb_level_from_exp(dex, exp_for_40);
  CHECK(expect_level == 40, "level test: gb_level_from_exp round-trips gb_exp_for_level (got %u)", expect_level);
  CHECK(landed.rec[G1R_LEVEL] == expect_level,
        "level test: landed level is %u (from EXP), not the box byte 10 (got %u)",
        expect_level, landed.rec[G1R_LEVEL]);
  CHECK(landed.rec[G1R_LEVEL] != 10, "level test: landed level must NOT be the stale box byte");
}

static void test_hp_status_types_preserved(void) {
  printf("  -- check 3: HP / status / stored types preserved\n");
  uint32_t len = gbf_build(GBF_RBY, g_fimg, 0);
  GbSession s; memset(&s, 0, sizeof s);
  CHECK(gbs_open(&s, g_fimg, len, g_fscratch, sizeof g_fscratch) == GBS_OK, "hp test: session opens");

  const uint16_t dex = 25;   /* Pikachu */
  uint32_t exp_for_30 = gb_exp_for_level(dex, 30);
  uint8_t rec33[GEN1_BOX_REC_BYTES];
  /* HP well under any computed max, a non-zero status, and stored types that disagree
   * with k_fake_g1base's Bulbasaur-shaped types (0x16/0x03). */
  build_g1_box_rec(rec33, dex, exp_for_30, /*boxlevel=*/30, /*hp=*/17, /*status=*/0x08,
                   /*type1=*/0x18, /*type2=*/0x18);

  uint8_t ot[GB_NAME_BYTES], nick[GB_NAME_BYTES];
  memset(ot, 0x50, sizeof ot); memset(nick, 0x50, sizeof nick);
  GbEditMon mon;
  CHECK(gb_load_parts(&mon, GB_GEN1, false, rec33, ot, nick, rec33[G1R_SPECIES]),
        "hp test: box mon builds");

  int pb = gbs_party_box(&s);
  int slot_out = -1;
  GbsStatus st = gbs_insert_party(&s, &mon, &k_fake_g1base, &slot_out, g_flist);
  CHECK(st == GBS_OK, "hp test: insert lands (%s)", gbs_status_text(st));
  if (st != GBS_OK) return;

  Gen1EditMon landed;
  CHECK(gen1_edit_load(g_flist, pb, slot_out, &landed), "hp test: landed reloads");
  CHECK(rd16be_local(landed.rec + G1R_HP) == 17, "hp test: current HP preserved byte-for-byte (got %u)",
        rd16be_local(landed.rec + G1R_HP));
  CHECK(landed.rec[G1R_STATUS] == 0x08, "hp test: status byte preserved (got 0x%02X)", landed.rec[G1R_STATUS]);
  CHECK(landed.rec[G1R_TYPE1] == 0x18 && landed.rec[G1R_TYPE2] == 0x18,
        "hp test: stored types kept (0x18/0x18), NOT the ROM row's 0x16/0x03 (got 0x%02X/0x%02X)",
        landed.rec[G1R_TYPE1], landed.rec[G1R_TYPE2]);
}

/* ---- check 4: Gen 2 unchanged -- gbs_insert_party vs gbs_move(box->party) --------- */

static uint8_t g_g2img_a[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
static uint8_t g_g2img_b[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
static uint8_t g_g2scr_a[GBS_SCRATCH_BYTES], g_g2scr_b[GBS_SCRATCH_BYTES];
static uint8_t g_g2list_a[GBS_LIST_BYTES], g_g2list_a2[GBS_LIST_BYTES], g_g2list_b[GBS_LIST_BYTES];

static void test_gen2_unchanged(const char* savfile) {
  uint32_t len = load_sav(savfile, g_g2img_a, sizeof g_g2img_a);
  if (!len) { printf("  SKIP %s (check 4)\n", savfile); return; }
  memcpy(g_g2img_b, g_g2img_a, len);

  GbSession sa, sb;
  CHECK(gbs_open(&sa, g_g2img_a, len, g_g2scr_a, sizeof g_g2scr_a) == GBS_OK, "%s check4: session A opens", savfile);
  CHECK(gbs_open(&sb, g_g2img_b, len, g_g2scr_b, sizeof g_g2scr_b) == GBS_OK, "%s check4: session B opens", savfile);

  /* Pick the first occupied non-party box slot as the shared source mon. */
  int nb = gbs_nboxes(&sa), src_box = -1, src_slot = -1;
  for (int b = 0; b < nb; b++) {
    if (gb_box_is_party(GB_GEN2, b)) continue;
    if (gbs_load_list(&sa, b, g_g2list_a) != GBS_OK) continue;
    int c = gb_list_count(GB_GEN2, g_g2list_a, b);
    if (c > 0) { src_box = b; src_slot = 0; break; }
  }
  if (src_box < 0) { printf("  SKIP %s (no occupied storage box for check 4)\n", savfile); return; }

  int pb = gbs_party_box(&sa);
  /* Both of Guy's real Gen-2 saves keep a full 6/6 party -- free slot 0 in BOTH
   * independent copies first (same slot, same species, so the comparison below is
   * unaffected) exactly as host_gbsession_test.c's s3_gen2_box_to_party does. */
  CHECK(gbs_load_list(&sa, pb, g_g2list_a2) == GBS_OK, "%s check4: party A loads for room check", savfile);
  if (gb_list_count(GB_GEN2, g_g2list_a2, pb) >= gb_list_capacity(GB_GEN2, pb)) {
    CHECK(gbs_delete(&sa, pb, 0, g_g2list_a2) == GBS_OK, "%s check4: freed party room in A", savfile);
    CHECK(gbs_delete(&sb, pb, 0, g_g2list_b) == GBS_OK, "%s check4: freed party room in B", savfile);
    CHECK(gbs_load_list(&sa, src_box, g_g2list_a) == GBS_OK, "%s check4: source box reloads for A", savfile);
  }

  /* Path A: gbs_insert_party() straight off a GbEditMon built from the box slot. */
  GbEditMon mon;
  CHECK(gb_load(&mon, GB_GEN2, g_g2list_a, src_box, src_slot) == true, "%s check4: source mon loads", savfile);
  int slot_a = -1;
  GbsStatus sta = gbs_insert_party(&sa, &mon, NULL, &slot_a, g_g2list_a2);
  CHECK(sta == GBS_OK, "%s check4: gbs_insert_party ran (%s)", savfile, gbs_status_text(sta));
  if (sta != GBS_OK) return;

  /* Path B: the pre-existing gbs_move(box -> party), on an independent copy. */
  int to_slot_b = -1;
  GbsStatus stb = gbs_move(&sb, src_box, src_slot, pb, &to_slot_b, g_g2list_b, g_flist);
  CHECK(stb == GBS_OK, "%s check4: gbs_move(box->party) ran (%s)", savfile, gbs_status_text(stb));
  if (sta != GBS_OK || stb != GBS_OK) return;

  uint8_t reca[GBS_LIST_BYTES], recb[GBS_LIST_BYTES];
  CHECK(gbs_load_list(&sa, pb, reca) == GBS_OK, "%s check4: party A reloads", savfile);
  CHECK(gbs_load_list(&sb, pb, recb) == GBS_OK, "%s check4: party B reloads", savfile);
  int offa = gb_off_record(GB_GEN2, pb, slot_a);
  int offb = gb_off_record(GB_GEN2, pb, to_slot_b);
  CHECK(offa >= 0 && offb >= 0, "%s check4: both records locate in their lists", savfile);
  if (offa < 0 || offb < 0) return;
  CHECK(memcmp(reca + offa, recb + offb, gb_rec_size(GB_GEN2, true)) == 0,
        "%s check4: gbs_insert_party and gbs_move(box->party) produce the SAME 48 bytes",
        savfile);
}

/* ---- check 5: refusals before any byte moves --------------------------------- */

static void test_refusals(void) {
  printf("  -- check 5: refusals before any byte moves\n");

  /* Gen-1 full party: fill the fixture's 3-member party to capacity via legitimate
   * inserts off existing box mons, then the next one must refuse FULL untouched. */
  {
    uint32_t len = gbf_build(GBF_RBY, g_fimg, 0);
    GbSession s; memset(&s, 0, sizeof s);
    CHECK(gbs_open(&s, g_fimg, len, g_fscratch, sizeof g_fscratch) == GBS_OK, "G1 full: session opens");
    int pb = gbs_party_box(&s);
    int cap = gb_list_capacity(GB_GEN1, pb);
    CHECK(gbs_load_list(&s, pb, g_flist) == GBS_OK, "G1 full: party loads");
    int have = gb_list_count(GB_GEN1, g_flist, pb);

    int box0 = -1;
    for (int b = 0; b < gbs_nboxes(&s); b++) {
      if (gb_box_is_party(GB_GEN1, b)) continue;
      if (gbs_load_list(&s, b, g_flist) == GBS_OK && gb_list_count(GB_GEN1, g_flist, b) > 0) { box0 = b; break; }
    }
    CHECK(box0 >= 0, "G1 full: a source storage box exists");

    while (have < cap && box0 >= 0) {
      CHECK(gbs_load_list(&s, box0, g_flist) == GBS_OK, "G1 full: source box reloads");
      GbEditMon donor;
      CHECK(gb_load(&donor, GB_GEN1, g_flist, box0, 0) == true, "G1 full: donor loads");
      int slot_out = -1;
      GbsStatus st = gbs_insert_party(&s, &donor, &k_fake_g1base, &slot_out, g_flist);
      CHECK(st == GBS_OK, "G1 full: filler insert %d/%d ok (%s)", have + 1, cap, gbs_status_text(st));
      if (st != GBS_OK) break;
      have++;
    }
    CHECK(have == cap, "G1 full: party is now at capacity (%d/%d)", have, cap);

    memcpy(g_fsnap, g_fimg, len);
    GbEditMon donor;
    CHECK(gbs_load_list(&s, box0, g_flist) == GBS_OK && gb_load(&donor, GB_GEN1, g_flist, box0, 0),
          "G1 full: one more donor loads");
    int slot_out = -1;
    GbsStatus st = gbs_insert_party(&s, &donor, &k_fake_g1base, &slot_out, g_flist);
    CHECK(st == GBS_ERR_FULL, "G1 full: the party-full insert is refused (got %s)", gbs_status_text(st));
    CHECK(memcmp(g_fimg, g_fsnap, len) == 0, "G1 full: the refused insert touched nothing");
  }

  /* Gen-2 full party: same shape. */
  {
    uint32_t len = gbf_build(GBF_GS, g_fimg, 0);
    GbSession s; memset(&s, 0, sizeof s);
    CHECK(gbs_open(&s, g_fimg, len, g_fscratch, sizeof g_fscratch) == GBS_OK, "G2 full: session opens");
    int pb = gbs_party_box(&s);
    int cap = gb_list_capacity(GB_GEN2, pb);
    CHECK(gbs_load_list(&s, pb, g_flist) == GBS_OK, "G2 full: party loads");
    int have = gb_list_count(GB_GEN2, g_flist, pb);

    int box0 = -1;
    for (int b = 0; b < gbs_nboxes(&s); b++) {
      if (gb_box_is_party(GB_GEN2, b)) continue;
      if (gbs_load_list(&s, b, g_flist) == GBS_OK && gb_list_count(GB_GEN2, g_flist, b) > 0) { box0 = b; break; }
    }
    CHECK(box0 >= 0, "G2 full: a source storage box exists");

    while (have < cap && box0 >= 0) {
      CHECK(gbs_load_list(&s, box0, g_flist) == GBS_OK, "G2 full: source box reloads");
      GbEditMon donor;
      CHECK(gb_load(&donor, GB_GEN2, g_flist, box0, 0) == true, "G2 full: donor loads");
      int slot_out = -1;
      GbsStatus st = gbs_insert_party(&s, &donor, NULL, &slot_out, g_flist);
      CHECK(st == GBS_OK, "G2 full: filler insert %d/%d ok (%s)", have + 1, cap, gbs_status_text(st));
      if (st != GBS_OK) break;
      have++;
    }
    CHECK(have == cap, "G2 full: party is now at capacity (%d/%d)", have, cap);

    memcpy(g_fsnap, g_fimg, len);
    GbEditMon donor;
    CHECK(gbs_load_list(&s, box0, g_flist) == GBS_OK && gb_load(&donor, GB_GEN2, g_flist, box0, 0),
          "G2 full: one more donor loads");
    int slot_out = -1;
    GbsStatus st = gbs_insert_party(&s, &donor, NULL, &slot_out, g_flist);
    CHECK(st == GBS_ERR_FULL, "G2 full: the party-full insert is refused (got %s)", gbs_status_text(st));
    CHECK(memcmp(g_fimg, g_fsnap, len) == 0, "G2 full: the refused insert touched nothing");
  }

  /* Gen-2 Mail: mirrors host_xfergate_test.c's (L) case. */
  {
    uint32_t len = gbf_build(GBF_GS, g_fimg, 0);
    GbSession s; memset(&s, 0, sizeof s);
    CHECK(gbs_open(&s, g_fimg, len, g_fscratch, sizeof g_fscratch) == GBS_OK, "G2 mail: session opens");
    int pb = gbs_party_box(&s);
    CHECK(gbs_load_list(&s, pb, g_flist) == GBS_OK, "G2 mail: party loads");
    GbEditMon mon;
    CHECK(gb_load(&mon, GB_GEN2, g_flist, pb, 1) == true, "G2 mail: slot 1 loads");
    CHECK(gb_set_held_item(&mon, 0xB5u) == true, "G2 mail: item set to a Mail id");
    CHECK(gb_commit(&mon, g_flist, pb, 1) == true, "G2 mail: committed into the list");
    CHECK(gbs_commit_list(&s, pb, g_flist) == GBS_OK, "G2 mail: list committed");

    int box0 = -1;
    for (int b = 0; b < gbs_nboxes(&s); b++) {
      if (gb_box_is_party(GB_GEN2, b)) continue;
      if (gbs_load_list(&s, b, g_flist) == GBS_OK && gb_list_count(GB_GEN2, g_flist, b) > 0) { box0 = b; break; }
    }
    CHECK(box0 >= 0, "G2 mail: a source storage box exists");
    memcpy(g_fsnap, g_fimg, len);
    GbEditMon donor;
    CHECK(gbs_load_list(&s, box0, g_flist) == GBS_OK && gb_load(&donor, GB_GEN2, g_flist, box0, 0),
          "G2 mail: donor loads");
    int slot_out = -1;
    GbsStatus st = gbs_insert_party(&s, &donor, NULL, &slot_out, g_flist);
    CHECK(st == GBS_ERR_MAIL, "G2 mail: refused with GBS_ERR_MAIL (got %s)", gbs_status_text(st));
    CHECK(memcmp(g_fimg, g_fsnap, len) == 0, "G2 mail: the refused insert touched nothing");
  }

  /* Gen-1 g1base == NULL. */
  {
    uint32_t len = gbf_build(GBF_RBY, g_fimg, 0);
    GbSession s; memset(&s, 0, sizeof s);
    CHECK(gbs_open(&s, g_fimg, len, g_fscratch, sizeof g_fscratch) == GBS_OK, "G1 no-base: session opens");
    int box0 = -1;
    for (int b = 0; b < gbs_nboxes(&s); b++) {
      if (gb_box_is_party(GB_GEN1, b)) continue;
      if (gbs_load_list(&s, b, g_flist) == GBS_OK && gb_list_count(GB_GEN1, g_flist, b) > 0) { box0 = b; break; }
    }
    CHECK(box0 >= 0, "G1 no-base: a source storage box exists");
    memcpy(g_fsnap, g_fimg, len);
    GbEditMon donor;
    CHECK(gbs_load_list(&s, box0, g_flist) == GBS_OK && gb_load(&donor, GB_GEN1, g_flist, box0, 0),
          "G1 no-base: donor loads");
    int slot_out = -1;
    GbsStatus st = gbs_insert_party(&s, &donor, NULL, &slot_out, g_flist);
    CHECK(st == GBS_ERR_NEEDS_BASE, "G1 no-base: refused with GBS_ERR_NEEDS_BASE (got %s)", gbs_status_text(st));
    CHECK(memcmp(g_fimg, g_fsnap, len) == 0, "G1 no-base: the refused insert touched nothing");
  }

  /* mon->is_party == true. */
  {
    uint32_t len = gbf_build(GBF_RBY, g_fimg, 0);
    GbSession s; memset(&s, 0, sizeof s);
    CHECK(gbs_open(&s, g_fimg, len, g_fscratch, sizeof g_fscratch) == GBS_OK, "is_party arg: session opens");
    int pb = gbs_party_box(&s);
    CHECK(gbs_load_list(&s, pb, g_flist) == GBS_OK, "is_party arg: party loads");
    GbEditMon donor;
    CHECK(gb_load(&donor, GB_GEN1, g_flist, pb, 0) == true, "is_party arg: party mon loads (is_party true)");
    memcpy(g_fsnap, g_fimg, len);
    int slot_out = -1;
    GbsStatus st = gbs_insert_party(&s, &donor, &k_fake_g1base, &slot_out, g_flist);
    CHECK(st == GBS_ERR_ARG, "is_party arg: refused with GBS_ERR_ARG (got %s)", gbs_status_text(st));
    CHECK(memcmp(g_fimg, g_fsnap, len) == 0, "is_party arg: the refused insert touched nothing");
  }
}

int main(void) {
  printf("host_gen1party_test: gbs_insert_party (BACKLOG #150 S150-7)\n");

  test_oracle("Red.sav", "Red.gb");
  test_oracle("Yellow.sav", "Yellow.gb");
  test_gen2_unchanged("Gold.sav");
  test_gen2_unchanged("Crystal.sav");
  test_level_from_exp();
  test_hp_status_types_preserved();
  test_refusals();

  printf("%s: %d/%d checks passed\n", g_fail ? "FAIL" : "ok", g_check - g_fail, g_check);
  return g_fail ? 1 : 0;
}

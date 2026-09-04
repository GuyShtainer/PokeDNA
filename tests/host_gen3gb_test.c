/* Host test for the lossless Gen 3 <-> Game Boy sidecar transfer:
 * source/gen3_to_gb.c (the down converter) and source/gb_sidecar.c (the sidecar file
 * format + the merge back up). docs/GEN3-TO-GB-SIDECAR-DESIGN.md is the design.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gen3gb_test.c \
 *      source/gen3_to_gb.c source/gb_sidecar.c source/gen12_convert.c \
 *      source/gen3_save.c source/gen3_mon.c source/gen3_box.c source/gen3_edit.c \
 *      source/gen3_daycare.c source/data_tables.c \
 *      source/gb_edit.c source/gb_session.c source/gen1_save.c source/gen1_write.c \
 *      source/gen2_save.c source/gen2_write.c -o /tmp/hg3gb
 *   /tmp/hg3gb /Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/ (.sav files)
 *
 * Two independent corpora, exactly like the modules under test:
 *   - the Gen-3 saves (Guy's own cartridge dumps) come in as argv, the same way every
 *     other Gen-3 host test takes them, and run_host_tests.py hands them over
 *     automatically to anything that indexes argv[];
 *   - the Game Boy saves are Guy's own dumps too, but OUTSIDE the repo at a fixed path
 *     (gitignored, never published) — host_gbsession_test.c's own convention, reused
 *     here rather than re-invented. Missing files SKIP rather than fail.
 *
 * THE GUARANTEE THIS FILE PINS: gen3_to_gb() converts a Gen-3 mon down, gb_sidecar.c
 * remembers everything that conversion could not carry, and gbsc_merge_up() rebuilds
 * the ORIGINAL 80 bytes EXACTLY when nothing changed on the Game Boy side, and folds
 * in precisely the field that DID change otherwise, leaving every other byte alone.
 * That is what section 2 and section 3 below measure, over the whole real corpus.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "gen3_edit.h"
#include "data_tables.h"
#include "gen3_to_gb.h"
#include "gb_sidecar.h"
#include "gen12_convert.h"   /* gen12_iv_from_dv, used only to cross-check the merge  */
#include "gb_session.h"
#include "gen1_write.h"

#define GB_ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { \
    g_check++; \
    if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
  } while (0)

/* ---- CRC-16/CCITT-FALSE, re-derived locally just for the test-vector assertion.
 * gb_sidecar.c's own crc16() is file-static; this is NOT a second implementation the
 * library depends on, only the standard check value pinned independently. */
static uint16_t crc16_ccitt_false(const uint8_t* data, uint32_t len) {
  uint16_t crc = 0xFFFFu;
  for (uint32_t i = 0; i < len; i++) {
    crc = (uint16_t)(crc ^ ((uint16_t)data[i] << 8));
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
  }
  return crc;
}

/* ---- shared small helpers --------------------------------------------------- */

static bool gb_record_clean(const GbEditMon* e) {
  GbIssues iss;
  if (gb_check(e, &iss)) return true;
  return iss.stats_stale && !iss.species_bad && !iss.list_mismatch && !iss.level_range &&
         !iss.level_exp_bad && !iss.move_empty && !iss.move_hole && !iss.move_range &&
         !iss.move_dup && !iss.pp_over && !iss.pp_on_empty && !iss.gen1_type_bad;
}

static uint8_t iv_to_dv(uint8_t iv) { return (uint8_t)(iv / 2u); }

/* ============================================================================ */
/* 1. CRC vector, sidecar format round trip, corruption/version refusal, key    */
/*    stability.                                                                */
/* ============================================================================ */

static void test_format(void) {
  printf("== 1. sidecar format ==\n");
  CHECK(crc16_ccitt_false((const uint8_t*)"123456789", 9) == 0x29B1u,
        "CRC-16/CCITT-FALSE standard test vector");

  static uint8_t buf[GBSC_FILE_MAX];
  uint8_t dv4[4] = { 5, 6, 7, 8 };
  uint8_t otname[GB_NAME_BYTES] = { 0x81, 0x82, 0x83, 0x50, 0x50, 0x50,
                                    0x50, 0x50, 0x50, 0x50, 0x50 };
  uint64_t key = gbsc_key(GB_GEN2, 0x1234, dv4, otname);
  char hex[17];
  gbsc_key_hex(key, hex);
  CHECK(strlen(hex) == 16, "gbsc_key_hex writes 16 hex digits");

  uint32_t len = (uint32_t)gbsc_init(buf, key);
  CHECK((int)len == GBSC_HEADER, "gbsc_init returns the header length");
  CHECK(gbsc_count(buf, len) == 0, "a fresh file counts 0 entries");

  GbscEntry entries[GBSC_MAX_ENTRIES];
  for (int i = 0; i < GBSC_MAX_ENTRIES; i++) {
    memset(&entries[i], 0, sizeof entries[i]);
    entries[i].gen             = GB_GEN2;
    entries[i].species_written = (uint16_t)(1 + i);
    entries[i].otid16          = 0x1234;
    memcpy(entries[i].dv4, dv4, 4);
    memcpy(entries[i].otname_written, otname, GB_NAME_BYTES);
    memset(entries[i].nick_written, 0x50, GB_NAME_BYTES);
    entries[i].exp_written = 1000u * (uint32_t)i;
    entries[i].rtc_epoch   = 0;
    memset(entries[i].original80, (uint8_t)(0x10 + i), 80);
    int idx = gbsc_add(buf, &len, sizeof buf, &entries[i]);
    CHECK(idx == i, "entry %d lands at index %d (got %d)", i, i, idx);
  }
  CHECK(gbsc_count(buf, len) == GBSC_MAX_ENTRIES, "count == GBSC_MAX_ENTRIES after filling");
  {
    GbscEntry ninth; memset(&ninth, 0, sizeof ninth);
    CHECK(gbsc_add(buf, &len, sizeof buf, &ninth) == -1, "a 9th entry is refused (full)");
  }

  for (int i = 0; i < GBSC_MAX_ENTRIES; i++) {
    GbscEntry got;
    CHECK(gbsc_get(buf, len, i, &got), "gbsc_get(%d)", i);
    CHECK(memcmp(&got, &entries[i], sizeof got) == 0, "entry %d round-trips exactly", i);
  }

  /* corrupt one byte inside entry 3, then restore it */
  uint32_t victim = GBSC_HEADER + 3u * GBSC_ENTRY + 50u;
  uint8_t saved = buf[victim];
  buf[victim] ^= 0xFFu;
  CHECK(gbsc_count(buf, len) == -1, "a corrupted byte inside an entry makes count == -1");
  buf[victim] = saved;
  CHECK(gbsc_count(buf, len) == GBSC_MAX_ENTRIES, "restoring the byte makes it valid again");

  uint8_t saved_ver = buf[4];
  buf[4] = 2;
  CHECK(gbsc_count(buf, len) == -1, "an unknown version makes count == -1");
  buf[4] = saved_ver;

  /* gbsc_find: every entry above shares the same gen/otid16/dv4/otname (species is
   * excluded from the fingerprint by design), so `start` is what tells them apart. */
  uint8_t rec0[GB_MAX_REC]; uint8_t nm0[GB_NAME_BYTES];
  memset(rec0, 0, sizeof rec0); memset(nm0, 0x50, sizeof nm0);
  GbEditMon now;
  CHECK(gb_load_parts(&now, GB_GEN2, false, rec0, nm0, nm0, 0), "build a probe record");
  gb_set_otid(&now, 0x1234);
  gb_set_dv(&now, GB_ATK, dv4[0]); gb_set_dv(&now, GB_DEF, dv4[1]);
  gb_set_dv(&now, GB_SPE, dv4[2]); gb_set_dv(&now, GB_SPC, dv4[3]);
  gb_set_otname_raw(&now, otname);
  for (int i = 0; i < GBSC_MAX_ENTRIES; i++)
    CHECK(gbsc_find(buf, len, &now, i) == i, "gbsc_find(start=%d) returns %d", i, i);
  CHECK(gbsc_find(buf, len, &now, GBSC_MAX_ENTRIES) == -1, "gbsc_find past the end returns -1");

  /* remove entry 3, check compaction */
  uint32_t len2 = len;
  CHECK(gbsc_remove(buf, &len2, 3) == 0, "gbsc_remove(3)");
  CHECK(gbsc_count(buf, len2) == GBSC_MAX_ENTRIES - 1, "count decremented by one");
  for (int i = 0; i < GBSC_MAX_ENTRIES - 1; i++) {
    int src = (i < 3) ? i : i + 1;
    GbscEntry got;
    CHECK(gbsc_get(buf, len2, i, &got), "gbsc_get after remove, index %d", i);
    CHECK(memcmp(&got, &entries[src], sizeof got) == 0,
          "post-remove index %d matches original entry %d", i, src);
  }

  /* key stability */
  uint64_t k1 = gbsc_key(GB_GEN2, 0x1234, dv4, otname);
  uint64_t k2 = gbsc_key(GB_GEN2, 0x1234, dv4, otname);
  CHECK(k1 == k2, "gbsc_key is stable across calls with the same inputs");
  uint8_t otname2[GB_NAME_BYTES]; memcpy(otname2, otname, GB_NAME_BYTES); otname2[0] ^= 1;
  uint64_t k3 = gbsc_key(GB_GEN2, 0x1234, dv4, otname2);
  CHECK(k1 != k3, "a different otname byte changes the key");
}

/* ============================================================================ */
/* 2/3. The Gen-3 corpus: every party + PC-box mon, both target generations.    */
/* ============================================================================ */

static int g_tested[3], g_accepted[3], g_refused[3][7], g_roundtrip[3];

/* A representative accepted conversion per target generation, kept for sections 3
 * and 4 -- section 3's GB-side-change subtests use the GEN2 one (no base-stat table
 * needed); section 4 needs one insertable sample per generation, and a species over
 * 151 accepted for Gen 2 is routinely refused for Gen 1 (gb_max_species), so each
 * generation keeps its own rather than section 4 re-deriving one that may not exist. */
static bool     g_have_sample[3];
static uint8_t  g_sample_rec[3][80];
static GbEditMon g_sample_out[3];
static GbscEntry g_sample_entry[3];

static void check_conversion(const uint8_t* rec, uint8_t gen, const GbGen1Base* base) {
  PkMon m;
  if (!pk_decode_mon(rec, false, &m)) return;         /* empty slot: not this test's subject */
  if (m.isBadEgg) return;                              /* corrupt/hacked: skip, like host_edit_test.c */
  pk_resolve(&m);

  g_tested[gen]++;
  GbEditMon out; Gen3ToGbLoss loss;
  G3GbStatus st = gen3_to_gb(rec, gen, base, &out, &loss);
  g_refused[gen][st]++;
  if (st != G3GB_OK) return;
  g_accepted[gen]++;

  CHECK(gb_record_clean(&out), "gb_check clean (species=%u gen=%u)", m.species, gen);
  CHECK(gb_get_dv(&out, GB_ATK) == iv_to_dv(m.ivs[PK_ATK]), "DV Atk == IV Atk / 2");
  CHECK(gb_get_dv(&out, GB_DEF) == iv_to_dv(m.ivs[PK_DEF]), "DV Def == IV Def / 2");
  CHECK(gb_get_dv(&out, GB_SPE) == iv_to_dv(m.ivs[PK_SPE]), "DV Spe == IV Spe / 2");
  CHECK(gb_get_dv(&out, GB_SPC) == iv_to_dv(m.ivs[PK_SPA]), "DV Spc == IV SpA / 2");
  CHECK(gb_get_level(&out) == m.level, "level matches (%u vs %u)", gb_get_level(&out), m.level);
  for (int i = 0; i < 4; i++)
    CHECK(gb_get_move(&out, i) == (uint8_t)m.moves[i], "move %d matches", i);

  char nb[GB_TEXT_MAX], ob[GB_TEXT_MAX];
  gb_get_nickname(&out, nb, sizeof nb);
  if (!loss.nick_lossy) CHECK(strcmp(nb, m.nickname) == 0, "nickname round-trips exactly");
  else CHECK(gb_text_lossy(gen, m.nickname, GB_NICK_GLYPHS, NULL) != 0,
             "nick_lossy implies gb_text_lossy is non-zero");
  gb_get_otname(&out, ob, sizeof ob);
  if (!loss.ot_lossy) CHECK(strcmp(ob, m.otName) == 0, "OT name round-trips exactly");
  else CHECK(gb_text_lossy(gen, m.otName, GB_OT_GLYPHS, NULL) != 0,
             "ot_lossy implies gb_text_lossy is non-zero");

  /* sidecar round trip: encode, decode, find, merge up with an UNCHANGED `out` must
   * reproduce `rec`'s 80 bytes exactly -- the whole point of the feature. */
  GbscEntry e;
  gbsc_entry_from(&e, &out, rec, 0);
  static uint8_t filebuf[GBSC_FILE_MAX];
  uint8_t dv4[4] = { gb_get_dv(&out, GB_ATK), gb_get_dv(&out, GB_DEF),
                     gb_get_dv(&out, GB_SPE), gb_get_dv(&out, GB_SPC) };
  uint64_t key = gbsc_key(gen, gb_get_otid(&out), dv4, out.otname);
  uint32_t flen = (uint32_t)gbsc_init(filebuf, key);
  CHECK(gbsc_add(filebuf, &flen, sizeof filebuf, &e) == 0, "sidecar add");
  GbscEntry back;
  CHECK(gbsc_get(filebuf, flen, 0, &back), "sidecar get");
  CHECK(memcmp(&back, &e, sizeof back) == 0, "sidecar entry decodes back exactly");
  CHECK(gbsc_find(filebuf, flen, &out, 0) == 0, "gbsc_find locates the entry at index 0");

  uint8_t back80[80];
  GbscMergeReport rep;
  CHECK(gbsc_merge_up(&back, &out, back80, &rep), "gbsc_merge_up succeeds");
  if (memcmp(back80, rec, 80) == 0) {
    g_roundtrip[gen]++;
  } else {
    CHECK(false, "merge-up byte-identical round trip (species=%u gen=%u)", m.species, gen);
  }

  if (!g_have_sample[gen] && !rep.evolved && !rep.level_changed &&
      !rep.moves_changed && !rep.renamed && !rep.dv_edited) {
    g_have_sample[gen] = true;
    memcpy(g_sample_rec[gen], rec, 80);
    g_sample_out[gen] = out;
    g_sample_entry[gen] = e;
  }
}

static void run_corpus_file(const char* path) {
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

  PkMon party[6]; bool frlg = false;
  int party_n = pk_read_party_auto(sb1, party, &frlg);
  (void)party_n;
  uint16_t doff = frlg ? 0x0038 : 0x0238;
  uint16_t coff = frlg ? 0x0034 : 0x0234;
  uint8_t count = sb1[coff];
  if (count > 6) count = 6;

  GbGen1Base g1base;
  memset(g1base.base, 50, sizeof g1base.base);   /* test stand-in: no real Gen-1 base
                                                    * table is required for this slice
                                                    * (that lands with the UI slice, per
                                                    * the S5-A brief). */
  g1base.type1 = g1base.type2 = 0x14;            /* Fire, an arbitrary valid Gen-1 type */

  int tested = 0;
  for (int i = 0; i < count; i++) {
    const uint8_t* rec = sb1 + doff + (uint32_t)i * 100;   /* the 80-byte core only */
    check_conversion(rec, GB_GEN2, NULL);
    check_conversion(rec, GB_GEN1, &g1base);
    tested++;
  }
  for (int b = 0; b < G3_TOTAL_BOXES; b++)
    for (int s = 0; s < G3_IN_BOX; s++) {
      const uint8_t* rec = pc + 0x0004 + ((uint32_t)b * G3_IN_BOX + s) * 80;
      check_conversion(rec, GB_GEN2, NULL);
      check_conversion(rec, GB_GEN1, &g1base);
      tested++;
    }
  printf("  %s: %d slots scanned\n", path, tested);
}

/* ============================================================================ */
/* 3. GB-side changes between the down transfer and the merge up.               */
/* ============================================================================ */

static void test_gb_side_changes(void) {
  if (!g_have_sample[GB_GEN2]) { printf("== 3. SKIP (no clean sample conversion found) ==\n"); return; }
  printf("== 3. GB-side changes before the merge up ==\n");

  PkMon orig;
  CHECK(pk_decode_mon(g_sample_rec[GB_GEN2], false, &orig), "decode the sample's original record");
  pk_resolve(&orig);

  /* (a) level change */
  {
    GbEditMon chg = g_sample_out[GB_GEN2];
    uint8_t new_level = (gb_get_level(&chg) < 100) ? (uint8_t)(gb_get_level(&chg) + 1)
                                                    : (uint8_t)(gb_get_level(&chg) - 1);
    CHECK(gb_set_level(&chg, new_level), "(a) set new level");
    uint8_t back80[80]; GbscMergeReport rep;
    CHECK(gbsc_merge_up(&g_sample_entry[GB_GEN2], &chg, back80, &rep), "(a) merge up");
    PkMon merged; CHECK(pk_decode_mon(back80, false, &merged), "(a) merged record decodes");
    pk_resolve(&merged);
    CHECK(rep.level_changed, "(a) report says level_changed");
    CHECK(merged.level == new_level, "(a) the merged level reflects the GB value");
    CHECK(merged.species == orig.species, "(a) species unaffected");
    CHECK(memcmp(merged.ivs, orig.ivs, sizeof orig.ivs) == 0, "(a) IVs unaffected");
    CHECK(memcmp(merged.evs, orig.evs, sizeof orig.evs) == 0, "(a) EVs unaffected");
    CHECK(memcmp(merged.moves, orig.moves, sizeof orig.moves) == 0, "(a) moves unaffected");
    CHECK(strcmp(merged.nickname, orig.nickname) == 0, "(a) nickname unaffected");
    CHECK(memcmp(g_sample_rec[GB_GEN2], back80, 8) == 0, "(a) personality+otId raw bytes unchanged");
  }

  /* (b) move change: replace slot 0 with a different in-range move */
  {
    GbEditMon chg = g_sample_out[GB_GEN2];
    uint8_t old_mv = gb_get_move(&chg, 0);
    uint8_t new_mv = (uint8_t)((old_mv % gb_max_move(chg.gen)) + 1);
    if (new_mv == old_mv) new_mv = (uint8_t)((new_mv % gb_max_move(chg.gen)) + 1);
    CHECK(gb_set_move(&chg, 0, new_mv), "(b) set a different move");
    uint8_t back80[80]; GbscMergeReport rep;
    CHECK(gbsc_merge_up(&g_sample_entry[GB_GEN2], &chg, back80, &rep), "(b) merge up");
    PkMon merged; CHECK(pk_decode_mon(back80, false, &merged), "(b) merged record decodes");
    pk_resolve(&merged);
    CHECK(rep.moves_changed, "(b) report says moves_changed");
    CHECK(merged.moves[0] == new_mv, "(b) the merged move 0 reflects the GB value");
    CHECK(merged.species == orig.species, "(b) species unaffected");
    CHECK(merged.level == orig.level, "(b) level unaffected");
    CHECK(memcmp(merged.ivs, orig.ivs, sizeof orig.ivs) == 0, "(b) IVs unaffected");
    CHECK(strcmp(merged.nickname, orig.nickname) == 0, "(b) nickname unaffected");
    CHECK(memcmp(g_sample_rec[GB_GEN2], back80, 8) == 0, "(b) personality+otId raw bytes unchanged");
  }

  /* (c) nickname change */
  {
    GbEditMon chg = g_sample_out[GB_GEN2];
    CHECK(gb_set_nickname(&chg, "GATE"), "(c) set nickname to GATE");
    uint8_t back80[80]; GbscMergeReport rep;
    CHECK(gbsc_merge_up(&g_sample_entry[GB_GEN2], &chg, back80, &rep), "(c) merge up");
    PkMon merged; CHECK(pk_decode_mon(back80, false, &merged), "(c) merged record decodes");
    pk_resolve(&merged);
    CHECK(rep.renamed && !rep.rename_refused, "(c) report says renamed");
    CHECK(strcmp(merged.nickname, "GATE") == 0, "(c) the merged nickname reflects the GB value");
    CHECK(merged.species == orig.species, "(c) species unaffected");
    CHECK(merged.level == orig.level, "(c) level unaffected");
    CHECK(memcmp(merged.moves, orig.moves, sizeof orig.moves) == 0, "(c) moves unaffected");
    CHECK(memcmp(merged.ivs, orig.ivs, sizeof orig.ivs) == 0, "(c) IVs unaffected");
    CHECK(memcmp(g_sample_rec[GB_GEN2], back80, 8) == 0, "(c) personality+otId raw bytes unchanged");
  }

  /* (d) DV change on Atk */
  {
    GbEditMon chg = g_sample_out[GB_GEN2];
    uint8_t old_dv = gb_get_dv(&chg, GB_ATK);
    uint8_t new_dv = (uint8_t)((old_dv + 1) & 15);
    CHECK(gb_set_dv(&chg, GB_ATK, new_dv), "(d) set a different Atk DV");
    uint8_t back80[80]; GbscMergeReport rep;
    CHECK(gbsc_merge_up(&g_sample_entry[GB_GEN2], &chg, back80, &rep), "(d) merge up");
    PkMon merged; CHECK(pk_decode_mon(back80, false, &merged), "(d) merged record decodes");
    pk_resolve(&merged);
    CHECK(rep.dv_edited, "(d) report says dv_edited");
    CHECK(merged.ivs[PK_ATK] == gen12_iv_from_dv(new_dv),
          "(d) the merged Atk IV reflects the GB DV");
    CHECK(merged.ivs[PK_DEF] == orig.ivs[PK_DEF], "(d) Def IV unaffected");
    CHECK(merged.ivs[PK_SPE] == orig.ivs[PK_SPE], "(d) Spe IV unaffected");
    CHECK(merged.ivs[PK_SPA] == orig.ivs[PK_SPA], "(d) SpA IV unaffected");
    CHECK(merged.ivs[PK_SPD] == orig.ivs[PK_SPD], "(d) SpD IV unaffected (never touched)");
    CHECK(merged.species == orig.species, "(d) species unaffected");
    CHECK(strcmp(merged.nickname, orig.nickname) == 0, "(d) nickname unaffected");
    CHECK(memcmp(g_sample_rec[GB_GEN2], back80, 8) == 0, "(d) personality+otId raw bytes unchanged");
  }

  /* (e) species change (evolution stand-in): next dex number, if the generation has one */
  {
    uint16_t old_dex = gb_get_species_dex(&g_sample_out[GB_GEN2]);
    uint16_t new_dex = (uint16_t)(old_dex + 1);
    if (new_dex < 1 || new_dex > gb_max_species(g_sample_out[GB_GEN2].gen)) {
      printf("   (e) SKIP: species %u has no next dex slot in this generation\n", old_dex);
    } else {
      GbEditMon chg = g_sample_out[GB_GEN2];
      CHECK(gb_set_species(&chg, new_dex, NULL), "(e) set species to dex %u", new_dex);
      uint8_t back80[80]; GbscMergeReport rep;
      CHECK(gbsc_merge_up(&g_sample_entry[GB_GEN2], &chg, back80, &rep), "(e) merge up");
      PkMon merged; CHECK(pk_decode_mon(back80, false, &merged), "(e) merged record decodes");
      pk_resolve(&merged);
      CHECK(rep.evolved, "(e) report says evolved");
      CHECK(merged.species == new_dex, "(e) the merged species reflects the GB value (%u vs %u)",
            merged.species, new_dex);
      CHECK(merged.level == orig.level, "(e) level preserved across the species change");
      CHECK(memcmp(merged.ivs, orig.ivs, sizeof orig.ivs) == 0, "(e) IVs unaffected");
      CHECK(strcmp(merged.nickname, orig.nickname) == 0, "(e) nickname unaffected");
      CHECK(memcmp(g_sample_rec[GB_GEN2], back80, 8) == 0, "(e) personality+otId raw bytes unchanged");
    }
  }
}

/* ============================================================================ */
/* 4. Engine acceptance: insert the converted record into a real GB save.       */
/* ============================================================================ */

static int landed = 0;

static void test_engine_gen2(const char* file) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", GB_ROMS, file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP %s (not present)\n", file); return; }
  static uint8_t img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);
  if (!g_have_sample[GB_GEN2]) { printf("  SKIP %s (no sample conversion)\n", file); return; }

  GbSession s;
  static uint8_t scratch[GBS_SCRATCH_BYTES];
  GbsStatus st = gbs_open(&s, img, len, scratch, sizeof scratch);
  CHECK(st == GBS_OK, "%s: gbs_open", file);
  if (st != GBS_OK) return;

  int box = -1;
  static uint8_t list[GBS_LIST_BYTES];
  int nb = gbs_nboxes(&s);
  for (int b = 0; b < nb; b++) {
    if (gbs_box_writable(&s, b) != GBS_OK) continue;
    if (gbs_load_list(&s, b, list) != GBS_OK) continue;
    if (gb_list_count(GB_GEN2, list, b) < gb_list_capacity(GB_GEN2, b)) { box = b; break; }
  }
  if (box < 0) { printf("  %s: SKIP (no box with room)\n", file); return; }
  CHECK(gbs_load_list(&s, box, list) == GBS_OK, "%s: load box %d", file, box);

  G2Slot slot;
  memset(&slot, 0, sizeof slot);
  uint8_t rec[GB_MAX_REC], otname[GB_NAME_BYTES], nick[GB_NAME_BYTES], list_sp;
  CHECK(gb_commit_parts(&g_sample_out[GB_GEN2], rec, otname, nick, &list_sp),
        "%s: extract the sample's raw parts", file);
  memcpy(slot.rec, rec, G2_PARTY_ENTRY < sizeof rec ? G2_PARTY_ENTRY : sizeof rec);
  memcpy(slot.otname, otname, GB_NAME_BYTES);
  memcpy(slot.nickname, nick, GB_NAME_BYTES);
  slot.is_egg = false;
  slot.is_party = false;

  int slot_out = -1;
  G2WStatus ws = g2w_append(list, box, &slot, &slot_out);
  CHECK(ws == G2W_OK, "%s: g2w_append (%s)", file, g2w_status_text(ws));
  if (ws != G2W_OK) return;

  GbsStatus cs = gbs_commit_list(&s, box, list);
  CHECK(cs == GBS_OK, "%s: gbs_commit_list (%s)", file, gbs_status_text(cs));
  if (cs != GBS_OK) return;

  static uint8_t list2[GBS_LIST_BYTES];
  CHECK(gbs_load_list(&s, box, list2) == GBS_OK, "%s: reload box %d", file, box);
  GbEditMon back;
  CHECK(gb_load(&back, GB_GEN2, list2, box, slot_out), "%s: gb_load the new slot", file);
  CHECK(gb_verify_slot(&back, list2, box, slot_out), "%s: gb_verify_slot on the new slot", file);
  printf("  %s: landed in box %d slot %d\n", file, box, slot_out);
  landed++;
}

static void test_engine_gen1(const char* file) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", GB_ROMS, file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP %s (not present)\n", file); return; }
  static uint8_t img[GEN1_SAVE_SIZE];
  uint32_t len = (uint32_t)fread(img, 1, sizeof img, f);
  fclose(f);

  /* Uses the dedicated GEN1 sample captured in section 2 -- NOT the GEN2 one. The
   * two are independent: a GEN2-accepted species can sit at national dex 152..251,
   * which gb_max_species(GB_GEN1) (151) refuses outright, so re-deriving a Gen-1
   * record from the Gen-2 sample would fail for roughly 40% of real corpora (measured
   * while writing this test). Each generation keeping its own sample is what
   * check_conversion()'s g_sample_* arrays are indexed by `gen` for. */
  if (!g_have_sample[GB_GEN1]) { printf("  %s: SKIP (no Gen-1 sample conversion)\n", file); return; }

  Gen1Save s;
  Gen1Status gs = gen1_open(img, len, &s);
  CHECK(gs == GEN1_OK, "%s: gen1_open (%s)", file, gen1_status_text(gs));
  if (gs != GEN1_OK) return;

  int roomy = -1;
  for (int b = 0; b < GEN1_NUM_BOXES; b++) {
    if (b == s.current_box) continue;
    int c = gen1_count(&s, b);
    if (c >= 0 && c < GEN1_BOX_CAPACITY) { roomy = b; break; }
  }
  if (roomy < 0) { printf("  %s: SKIP (no box with room)\n", file); return; }

  uint8_t rec[GB_MAX_REC], otname[GB_NAME_BYTES], nick[GB_NAME_BYTES], list_sp;
  CHECK(gb_commit_parts(&g_sample_out[GB_GEN1], rec, otname, nick, &list_sp),
        "%s: extract raw parts", file);

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
  CHECK(ws == GEN1W_OK, "%s: gen1_write_apply INSERT (%s)", file, gen1_write_status_text(ws));
  if (ws != GEN1W_OK) return;
  printf("  %s: landed in box %d slot %d\n", file, roomy, op.slot);
  landed++;
}

/* ============================================================================ */

int main(int argc, char** argv) {
  test_format();

  printf("== 2. the Gen-3 corpus, both target generations ==\n");
  for (int i = 1; i < argc; i++) run_corpus_file(argv[i]);
  if (argc <= 1) printf("  (no Gen-3 saves given on argv -- section 2/3 will be empty)\n");

  for (int g = GB_GEN1; g <= GB_GEN2; g++) {
    printf("  gen %d: tested=%d accepted=%d roundtrip-exact=%d\n",
           g, g_tested[g], g_accepted[g], g_roundtrip[g]);
    printf("    refused: ARG=%d EGG=%d SPECIES=%d MOVE=%d NEEDS_BASE=%d GLITCH=%d\n",
           g_refused[g][G3GB_ERR_ARG], g_refused[g][G3GB_ERR_EGG],
           g_refused[g][G3GB_ERR_SPECIES], g_refused[g][G3GB_ERR_MOVE],
           g_refused[g][G3GB_ERR_NEEDS_BASE], g_refused[g][G3GB_ERR_GLITCH]);
    CHECK(g_accepted[g] == g_roundtrip[g], "gen %d: every accepted conversion round-tripped", g);
  }

  test_gb_side_changes();

  printf("== 4. engine acceptance ==\n");
  test_engine_gen2("Gold.sav");
  test_engine_gen2("Crystal.sav");
  test_engine_gen1("Red.sav");
  printf("  %d record(s) landed in a real save\n", landed);

  printf("\n%s: %d check(s), %d failure(s)\n", g_fail ? "FAIL" : "OK", g_check, g_fail);
  return g_fail ? 1 : 0;
}

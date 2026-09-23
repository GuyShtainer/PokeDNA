/* source/gb_boxnames.c -- the pure-C Gen-2 box-name core -- under test.
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_gbboxnames_test.c \
 *      source/gb_boxnames.c source/gb_fields.c source/gb_session.c source/gb_edit.c \
 *      source/gen1_save.c source/gen1_write.c source/gen2_save.c source/gen2_write.c \
 *      source/data_tables.c source/item_map_g2g3.c source/item_map_g1g2.c source/gb_item_names.c source/gb_bag.c source/gen3_to_gb.c source/gb_sidecar.c source/bank_cell.c source/gen3_edit.c \
 *      source/gen3_mon.c source/gen3_box.c source/gen3_save.c source/gen3_daycare.c \
 *      -o /tmp/hgbn && /tmp/hgbn
 *
 * Corpus: Guy's own cartridge dumps (gitignored, gba-toolkit/roms/gb/). A missing
 * corpus SKIPS rather than fails.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gb_boxnames.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_fail = 0, g_check = 0, g_ran = 0;
#define CHECKF(c, ...) do { g_check++; if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static uint8_t g_img[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
static uint8_t g_orig[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
static uint8_t g_scratch[GBS_SCRATCH_BYTES];

static uint32_t load(const char* file) {
  char p[512];
  snprintf(p, sizeof p, "%s/%s", ROMS, file);
  FILE* f = fopen(p, "rb");
  if (!f) return 0;
  uint32_t n = (uint32_t)fread(g_img, 1, sizeof g_img, f);
  fclose(f);
  memcpy(g_orig, g_img, n);
  return n;
}

/* ---- A: Gen 1 is not applicable ---- */

static void gen1_na(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  CHECKF(!gbbn_supported(&s), "%s: Gen 1 must not be supported", file);
  char out[GB_TEXT_MAX];
  CHECKF(!gbbn_read(&s, 0, out, sizeof out), "%s: gbbn_read must refuse on Gen 1", file);
  GbsStatus st = gbbn_rename(&s, 0, "TEST");
  CHECKF(st == GBS_ERR_ARG, "%s: gbbn_rename must refuse on Gen 1 (got %s)", file,
        gbs_status_text(st));
}

/* ---- B: default names read "BOX1".."BOX14" (untouched Gold.sav/Crystal.sav) ---- */

static void default_names(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  CHECKF(gbbn_supported(&s), "%s: Gen 2 must be supported", file);

  /* cross-check gbf_off/len(GBF_BOXNAMES) against gen2_save.c's own g2_offsets, so the
   * two never silently drift apart (see gb_boxnames.h's own header note). */
  GbGame g = (s.g2w.sv.version == G2_VER_CRYSTAL) ? GBF_G_CRYSTAL : GBF_G_GS;
  G2Offsets o;
  CHECKF(g2_offsets(s.g2w.sv.version, &o), "%s: g2_offsets", file);
  CHECKF(gbf_off(g, GBF_BOXNAMES) == o.box_names, "%s: GBF_BOXNAMES 0x%04X != "
        "g2_offsets.box_names 0x%04X", file, gbf_off(g, GBF_BOXNAMES), o.box_names);
  CHECKF(gbf_len(g, GBF_BOXNAMES) == 126, "%s: GBF_BOXNAMES len != 126", file);

  for (int b = 0; b < G2_NUM_BOXES; b++) {
    char out[GB_TEXT_MAX];
    CHECKF(gbbn_read(&s, b, out, sizeof out), "%s: box %d gbbn_read", file, b);
    char want[16];
    snprintf(want, sizeof want, "BOX%d", b + 1);
    CHECKF(strcmp(out, want) == 0, "%s: box %d name '%s' != '%s'", file, b, out, want);
    /* F1b (BACKLOG #94 DO-NOT-SHIP fix): gbsrc_get_raw_name_impl (source/
     * pdna_gen12.c) delegates straight to gbbn_read() -- the box-rename editor's
     * seed value -- specifically because pdna_gen12_box_name(), the DISPLAY
     * formatter the banner draws with, prefixes every name with "GB " (source/
     * pdna_gen12.c's own header note). If gbbn_read() ever started returning that
     * decoration itself, seeding the editor from it would silently write "GB "
     * into the save on an unedited confirm again -- assert the raw read never
     * carries it. */
    CHECKF(strncmp(out, "GB ", 3) != 0,
          "%s: box %d gbbn_read returned '%s' -- the RAW read must never carry "
          "the display formatter's \"GB \" prefix (F1b)", file, b, out);
  }
  char scratch[GB_TEXT_MAX];
  CHECKF(!gbbn_read(&s, -1, scratch, sizeof scratch), "%s: negative box refused", file);
  CHECKF(!gbbn_read(&s, G2_NUM_BOXES, scratch, sizeof scratch), "%s: box == count refused",
        file);
}

/* ---- C: no-op rename is a zero-byte diff ---- */

static void noop_rename(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  char cur[GB_TEXT_MAX];
  CHECKF(gbbn_read(&s, 0, cur, sizeof cur), "%s: read box 0", file);
  GbsStatus st = gbbn_rename(&s, 0, cur);
  CHECKF(st == GBS_OK, "%s: no-op gbbn_rename status %s", file, gbs_status_text(st));
  uint32_t diff = 0, first = 0;
  for (uint32_t i = 0; i < len; i++)
    if (g_img[i] != g_orig[i]) { if (!diff) first = i; diff++; }
  CHECKF(diff == 0, "%s: no-op rename changed %u byte(s), first at 0x%04X (0x%02X -> 0x%02X)",
        file, diff, first, g_orig[first], g_img[first]);
}

/* ---- D: rename box 0, verify it changed AND every other box's name is untouched, then
 * round-trip through a fresh session ---- */

static void rename_roundtrip(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);

  char before[G2_NUM_BOXES][GB_TEXT_MAX];
  for (int b = 0; b < G2_NUM_BOXES; b++) CHECKF(gbbn_read(&s, b, before[b], GB_TEXT_MAX),
                                                "%s: pre-read box %d", file, b);

  GbsStatus st = gbbn_rename(&s, 0, "GENS");
  CHECKF(st == GBS_OK, "%s: gbbn_rename status %s", file, gbs_status_text(st));

  char after0[GB_TEXT_MAX];
  CHECKF(gbbn_read(&s, 0, after0, sizeof after0), "%s: post-read box 0", file);
  CHECKF(strcmp(after0, "GENS") == 0, "%s: box 0 name '%s' != 'GENS'", file, after0);

  for (int b = 1; b < G2_NUM_BOXES; b++) {
    char now[GB_TEXT_MAX];
    CHECKF(gbbn_read(&s, b, now, sizeof now), "%s: re-read box %d", file, b);
    CHECKF(strcmp(now, before[b]) == 0, "%s: box %d name changed ('%s' -> '%s')", file,
          b, before[b], now);
  }

  /* refuse an over-length name (9 glyphs, one past GB_BOXNAME_GLYPHS) before any byte
   * moves */
  uint32_t snap_len = len;
  uint8_t snap[sizeof g_img];
  memcpy(snap, g_img, snap_len);
  GbsStatus st_long = gbbn_rename(&s, 1, "TOOLONGG");   /* 8 glyphs -- exactly the cap,
                                                          * should be accepted */
  CHECKF(st_long == GBS_OK, "%s: an exactly-8-glyph name must be accepted (got %s)",
        file, gbs_status_text(st_long));
  GbsStatus st_over = gbbn_rename(&s, 1, "TOOLONGGG");  /* 9 glyphs -- one over the cap */
  CHECKF(st_over == GBS_ERR_ARG, "%s: a 9-glyph name must be refused (got %s)", file,
        gbs_status_text(st_over));

  /* re-open on the same buffer and read back */
  GbSession s2;
  CHECKF(gbs_open(&s2, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: reopen", file);
  char final0[GB_TEXT_MAX];
  CHECKF(gbbn_read(&s2, 0, final0, sizeof final0), "%s: final read box 0", file);
  CHECKF(strcmp(final0, "GENS") == 0, "%s: box 0 name did not stick across reopen", file);
}

/* ---- E: P1a review D4 -- a sequence-safe name (an {XX} escape) round-trips byte-
 * identically, and the unchanged guard recognises it as unchanged. We can't reach a
 * name with <PK>/<MN>/an accented letter from a plain UTF-8 gbbn_rename() call (there
 * is no keyboard input path in this core), so this plants the RAW bytes directly and
 * exercises gbbn_read()/gbbn_rename()'s DECODE side, which is exactly what P1a review
 * D4 was about (gbbn_read used to go through the lossy display decoder). ---- */

static void sequence_safe_roundtrip(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  GbGame g = (s.g2w.sv.version == G2_VER_CRYSTAL) ? GBF_G_CRYSTAL : GBF_G_GS;
  uint32_t off = gbf_off(g, GBF_BOXNAMES) + 2u * GB_BOXNAME_BYTES;   /* box 2, untouched */

  /* E1 E2 7F E1 E2 50 ... : two <PK>-ish glyphs (0xE1/0xE2 are Gen 2's own <PK>/<MN>
   * single-byte glyphs per pokecrystal's charmap), a space, repeated, then the
   * terminator -- a name the lossy display decoder collapses to blank runs, and the
   * exact class of byte g2w_encode_text's enc_step() cannot spell via UTF-8 at all. */
  uint8_t planted[GB_BOXNAME_BYTES] = { 0xE1, 0xE2, 0x7F, 0xE1, 0xE2, 0x50, 0x50, 0x50, 0x50 };
  CHECKF(gbs_write_field(&s, off, planted, GB_BOXNAME_BYTES) == GBS_OK,
        "%s: plant sequence bytes", file);
  CHECKF(gbs_finish(&s) == GBS_OK, "%s: finish after planting", file);
  memcpy(g_orig, g_img, len);   /* the planted, checksummed state is the new baseline */

  char decoded[GB_TEXT_MAX];
  CHECKF(gbbn_read(&s, 2, decoded, sizeof decoded), "%s: read planted box 2", file);
  CHECKF(strlen(decoded) > 0 && decoded[0] != ' ', "%s: sequence-safe decode must not "
        "collapse to blank spaces (got '%s')", file, decoded);

  /* renaming to the SAME decoded string must be recognised as unchanged (the
   * sequence-safe unchanged guard, not the lossy one) -- zero bytes moved. */
  GbsStatus st = gbbn_rename(&s, 2, decoded);
  CHECKF(st == GBS_OK, "%s: no-op rename of the decoded sequence status %s", file,
        gbs_status_text(st));
  uint32_t diff = 0;
  for (uint32_t i = 0; i < len; i++) if (g_img[i] != g_orig[i]) diff++;
  CHECKF(diff == 0, "%s: the unchanged guard must recognise the sequence-safe decode "
        "and not rewrite it (moved %u byte(s))", file, diff);

  /* and it must survive a fresh gb_name_encode/gb_name_decode round trip through the
   * SAME string, byte for byte. */
  GbSession s2;
  CHECKF(gbs_open(&s2, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: reopen", file);
  char decoded2[GB_TEXT_MAX];
  CHECKF(gbbn_read(&s2, 2, decoded2, sizeof decoded2), "%s: re-read box 2", file);
  CHECKF(strcmp(decoded, decoded2) == 0, "%s: sequence-safe decode is not stable "
        "('%s' != '%s')", file, decoded, decoded2);
}

/* ---- F: P1a review D5 -- an empty / all-space name is refused ---- */

static void empty_name_refused(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);

  GbsStatus st_empty = gbbn_rename(&s, 3, "");
  CHECKF(st_empty == GBS_ERR_ARG, "%s: an empty name must be refused (got %s)", file,
        gbs_status_text(st_empty));
  GbsStatus st_spaces = gbbn_rename(&s, 3, "   ");
  CHECKF(st_spaces == GBS_ERR_ARG, "%s: an all-space name must be refused (got %s)", file,
        gbs_status_text(st_spaces));

  uint32_t diff = 0;
  for (uint32_t i = 0; i < len; i++) if (g_img[i] != g_orig[i]) diff++;
  CHECKF(diff == 0, "%s: a refused empty/space rename must not move any byte", file);
}

int main(void) {
  printf("== A: Gen 1 has no box names ==\n");
  gen1_na("Red.sav");
  gen1_na("Yellow.sav");

  printf("== B: default names 'BOX1'..'BOX14' on the untouched corpus ==\n");
  default_names("Gold.sav");
  default_names("Crystal.sav");

  printf("== C: no-op rename is a zero-byte diff ==\n");
  noop_rename("Gold.sav");
  noop_rename("Crystal.sav");

  printf("== D: rename box 0, other boxes untouched, round trip ==\n");
  rename_roundtrip("Gold.sav");
  rename_roundtrip("Crystal.sav");

  printf("== E: a sequence-safe name ({<PK>}{<MN>}...) round-trips (P1a review D4) ==\n");
  sequence_safe_roundtrip("Gold.sav");
  sequence_safe_roundtrip("Crystal.sav");

  printf("== F: an empty / all-space name is refused (P1a review D5) ==\n");
  empty_name_refused("Gold.sav");
  empty_name_refused("Crystal.sav");

  printf("\n%d checks, %d failed, %d file(s) exercised\n", g_check, g_fail, g_ran);
  if (g_ran == 0) { printf("NOTE: corpus not found at %s -- every case skipped\n", ROMS); }
  return g_fail ? 1 : 0;
}

/* source/gb_fly.c -- the pure-C fly-destination bitfield core -- under test.
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_gbfly_test.c \
 *      source/gb_fly.c source/gb_fields.c source/gb_session.c source/gb_edit.c \
 *      source/gen1_save.c source/gen1_write.c source/gen2_save.c source/gen2_write.c \
 *      source/data_tables.c source/gen3_to_gb.c source/gb_sidecar.c source/bank_cell.c source/gen3_edit.c \
 *      source/gen3_mon.c source/gen3_box.c source/gen3_save.c source/gen3_daycare.c \
 *      -o /tmp/hgbf && /tmp/hgbf
 *
 * Corpus: Guy's own cartridge dumps (gitignored, gba-toolkit/roms/gb/). A missing
 * corpus SKIPS rather than fails.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gb_fly.h"

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

/* ---- A: read matches the corpus's raw bytes, bit for bit ---- */

static void read_matches(const char* file, int nbytes, const uint8_t* expect_bytes) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);

  GbGame g = gbfy_game(&s);
  /* P1a review D7: gbfy_count() is the GAME's real destination count (11/NUM_CITY_MAPS
   * for Gen 1, 28/NUM_SPAWNS for Gen 2), NOT the field's own bit width (nbytes*8 ==
   * 16/32) -- the field has trailing unused bits past the real destinations on both
   * generations (see gb_fly.c's own citations). */
  int want_count = (g == GBF_G_GS || g == GBF_G_CRYSTAL) ? 28 : 11;
  CHECKF(gbfy_count(g) == want_count, "%s: count %d != %d", file, gbfy_count(g), want_count);
  CHECKF(gbfy_count(g) <= nbytes * 8, "%s: count %d must not exceed the field's own bit "
        "width %d", file, gbfy_count(g), nbytes * 8);

  uint8_t bits[4] = { 0 };
  CHECKF(gbfy_read(&s, bits, (int)sizeof bits), "%s: gbfy_read", file);
  for (int i = 0; i < nbytes; i++)
    CHECKF(bits[i] == expect_bytes[i], "%s: byte %d 0x%02X != 0x%02X", file, i, bits[i],
          expect_bytes[i]);

  /* per-bit spot check against the same bytes */
  for (int i = 0; i < nbytes * 8; i++) {
    bool want = (expect_bytes[i / 8] & (1u << (i % 8))) != 0;
    CHECKF(gbfy_get(&s, i) == want, "%s: bit %d gbfy_get mismatch", file, i);
  }
  CHECKF(!gbfy_get(&s, nbytes * 8), "%s: out-of-range index must read false", file);
}

/* ---- B: set/clear one bit, only that byte moves, no-op is a zero-byte diff ---- */

static void set_clear_roundtrip(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  GbGame g = gbfy_game(&s);
  int n = gbfy_count(g);

  /* P1a review D6/D7: pick the first REAL destination (0..n-1, n now the game's actual
   * count) that reads unvisited -- Guy's own Red/Yellow corpus has every one of its 11
   * real destinations already visited (bits 11-15 are clear, but those are past the
   * real count and gbfy_set() must refuse them, so they cannot be used here either),
   * so this SKIPS rather than silently testing nothing (the old idx = n-1 = 15/31
   * picked a bit past the real destinations before D7's fix, making this whole case
   * vacuous). */
  int idx = -1;
  for (int i = 0; i < n; i++) if (!gbfy_get(&s, i)) { idx = i; break; }
  if (idx < 0) {
    printf("  SKIP %s (every real destination 0..%d is already visited in this corpus)\n",
          file, n - 1);
    return;
  }

  /* B0: a no-op set (already false, ask for false) must not touch a byte */
  GbsStatus st0 = gbfy_set(&s, idx, false);
  CHECKF(st0 == GBS_OK, "%s: no-op gbfy_set status %s", file, gbs_status_text(st0));
  uint32_t diff0 = 0;
  for (uint32_t i = 0; i < len; i++) if (g_img[i] != g_orig[i]) diff0++;
  CHECKF(diff0 == 0, "%s: no-op gbfy_set changed %u byte(s)", file, diff0);

  /* B1: set it true, verify exactly the owning byte moved */
  uint32_t off = gbf_off(g, gbf_off(g, GBF_FLY_FLAGS) ? GBF_FLY_FLAGS : GBF_FLY_FLAGS_G2);
  uint32_t byte_off = off + (uint32_t)(idx / 8);
  GbsStatus st1 = gbfy_set(&s, idx, true);
  CHECKF(st1 == GBS_OK, "%s: gbfy_set(true) status %s", file, gbs_status_text(st1));
  CHECKF(gbfy_get(&s, idx), "%s: bit did not read back set", file);
  uint32_t diff1 = 0, first = 0;
  for (uint32_t i = 0; i < len; i++)
    if (g_img[i] != g_orig[i]) { if (!diff1) first = i; diff1++; }
  CHECKF(diff1 >= 1, "%s: setting a bit must change at least one byte", file);
  CHECKF(first == byte_off || (s.gen == GB_GEN2 && diff1 >= 1),
        "%s: first changed byte 0x%04X, expected the owning byte 0x%04X (or its Gen-2 "
        "mirror/checksum)", file, first, byte_off);
  /* the owning byte itself must be among the changed bytes even if a Gen-2 mirror/
   * checksum also moved */
  CHECKF(g_img[byte_off] != g_orig[byte_off], "%s: owning byte 0x%04X did not change",
        file, byte_off);

  /* B2: clear it back, re-open and re-read on the fresh baseline */
  memcpy(g_orig, g_img, len);
  GbsStatus st2 = gbfy_set(&s, idx, false);
  CHECKF(st2 == GBS_OK, "%s: gbfy_set(false) status %s", file, gbs_status_text(st2));
  GbSession s2;
  CHECKF(gbs_open(&s2, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: reopen", file);
  CHECKF(!gbfy_get(&s2, idx), "%s: bit did not clear back", file);
}

/* ---- C: P1a review D7 -- an index past the real count, but still within the field's
 * own bit width, is refused (the exact bug: gbfy_count() used to answer the field
 * width, letting a caller "set" a bit that is not a real destination at all) ---- */

static void set_past_real_count_refused(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  GbGame g = gbfy_game(&s);
  int n = gbfy_count(g);   /* 11 or 28 */

  GbsStatus st = gbfy_set(&s, n, true);   /* one past the real count */
  CHECKF(st == GBS_ERR_ARG, "%s: gbfy_set(%d) (past the real count) must refuse (got %s)",
        file, n, gbs_status_text(st));
  uint32_t diff = 0;
  for (uint32_t i = 0; i < len; i++) if (g_img[i] != g_orig[i]) diff++;
  CHECKF(diff == 0, "%s: a refused out-of-count set must not move any byte", file);
}

int main(void) {
  static const uint8_t red_bytes[2] = { 0xFF, 0x07 };
  static const uint8_t g2_bytes[4]  = { 0xBC, 0xFF, 0xFD, 0x07 };

  printf("== A: read matches the corpus, byte for byte and bit for bit ==\n");
  read_matches("Red.sav", 2, red_bytes);
  read_matches("Yellow.sav", 2, red_bytes);   /* Yellow's own fixture may differ; see below */
  read_matches("Gold.sav", 4, g2_bytes);
  read_matches("Crystal.sav", 4, g2_bytes);

  printf("== B: set/clear one bit, minimal diff, round trip ==\n");
  set_clear_roundtrip("Red.sav");
  set_clear_roundtrip("Gold.sav");
  set_clear_roundtrip("Crystal.sav");

  printf("== C: setting past the real count is refused, not silently accepted (P1a "
        "review D7) ==\n");
  set_past_real_count_refused("Red.sav");
  set_past_real_count_refused("Yellow.sav");
  set_past_real_count_refused("Gold.sav");
  set_past_real_count_refused("Crystal.sav");

  printf("\n%d checks, %d failed, %d file(s) exercised\n", g_check, g_fail, g_ran);
  if (g_ran == 0) { printf("NOTE: corpus not found at %s -- every case skipped\n", ROMS); }
  return g_fail ? 1 : 0;
}

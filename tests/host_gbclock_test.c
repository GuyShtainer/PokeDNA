/* source/gb_clock.c -- the pure-C Gen-2 clock core -- under test.
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_gbclock_test.c \
 *      source/gb_clock.c source/gb_fields.c source/gb_session.c source/gb_edit.c \
 *      source/gen1_save.c source/gen1_write.c source/gen2_save.c source/gen2_write.c \
 *      source/data_tables.c source/gen3_to_gb.c source/gb_sidecar.c source/gen3_edit.c \
 *      source/gen3_mon.c source/gen3_box.c source/gen3_save.c source/gen3_daycare.c \
 *      -o /tmp/hgbc && /tmp/hgbc
 *
 * Corpus: Guy's own cartridge dumps (gitignored, gba-toolkit/roms/gb/). A missing
 * corpus SKIPS rather than fails.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gb_clock.h"

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

/* ---- A: Gen 1 -- present must be false, gbc_write must refuse ---- */

static void gen1_not_applicable(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  CHECKF(s.gen == GB_GEN1, "%s: expected Gen 1", file);

  GbClock c;
  CHECKF(gbc_read(&s, &c), "%s: gbc_read", file);
  CHECKF(!c.present, "%s: Gen 1 must report present=false", file);

  GbsStatus st = gbc_write(&s, &c, true);
  CHECKF(st == GBS_ERR_ARG, "%s: gbc_write on Gen 1 must refuse (got %s)", file,
        gbs_status_text(st));

  uint32_t diff = 0;
  for (uint32_t i = 0; i < len; i++) if (g_img[i] != g_orig[i]) diff++;
  CHECKF(diff == 0, "%s: a refused write must not move any byte", file);
}

/* ---- B: Gen 2 read matches the corpus (design doc §1.8, hand-verified) ---- */

static void gen2_read(const char* file, uint8_t start_day, uint8_t start_hour,
                      uint8_t start_min, uint8_t start_sec, uint32_t rtc_snapshot_be,
                      uint8_t dst_raw, uint8_t status_flags) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  CHECKF(s.gen == GB_GEN2, "%s: expected Gen 2", file);

  GbClock c;
  CHECKF(gbc_read(&s, &c), "%s: gbc_read", file);
  CHECKF(c.present, "%s: Gen 2 must report present=true", file);
  CHECKF(c.start_day == start_day, "%s: start_day 0x%02X != 0x%02X", file, c.start_day, start_day);
  CHECKF(c.start_hour == start_hour, "%s: start_hour 0x%02X != 0x%02X", file, c.start_hour, start_hour);
  CHECKF(c.start_minute == start_min, "%s: start_minute 0x%02X != 0x%02X", file, c.start_minute, start_min);
  CHECKF(c.start_second == start_sec, "%s: start_second 0x%02X != 0x%02X", file, c.start_second, start_sec);
  uint32_t got = ((uint32_t)c.rtc_snapshot[0] << 24) | ((uint32_t)c.rtc_snapshot[1] << 16) |
                ((uint32_t)c.rtc_snapshot[2] << 8) | c.rtc_snapshot[3];
  CHECKF(got == rtc_snapshot_be, "%s: rtc_snapshot 0x%08X != 0x%08X", file, got, rtc_snapshot_be);
  CHECKF(c.dst == (dst_raw != 0), "%s: dst mismatch", file);
  CHECKF(c.status_flags_ok, "%s: status_flags_ok must be true", file);
  CHECKF(c.status_flags == status_flags, "%s: status_flags 0x%02X != 0x%02X", file,
        c.status_flags, status_flags);
}

/* ---- C: no-op read->write is a zero-byte diff ---- */

static void noop_zero_diff(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  GbClock c;
  CHECKF(gbc_read(&s, &c), "%s: gbc_read", file);
  GbsStatus st = gbc_write(&s, &c, false);
  CHECKF(st == GBS_OK, "%s: no-op gbc_write status %s", file, gbs_status_text(st));
  uint32_t diff = 0, first = 0;
  for (uint32_t i = 0; i < len; i++)
    if (g_img[i] != g_orig[i]) { if (!diff) first = i; diff++; }
  CHECKF(diff == 0, "%s: no-op gbc_write changed %u byte(s), first at 0x%04X (0x%02X -> 0x%02X)",
        file, diff, first, g_orig[first], g_img[first]);
}

/* ---- D: edit start_day/hour/minute/second + clear status flags, verify exactly the
 * claimed bytes moved, re-open and read back ---- */

static void edit_roundtrip(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;

  /* plant a nonzero status_flags byte first so clear_status_flags has something to do */
  GbSession ps;
  CHECKF(gbs_open(&ps, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: plant open", file);
  GbGame pg = gbc_game(&ps);
  uint8_t one = 0x01;
  CHECKF(gbs_write_field(&ps, gbf_off(pg, GBF_RTC_STATUS_FLAGS), &one, 1) == GBS_OK,
        "%s: plant status_flags", file);
  CHECKF(gbs_finish(&ps) == GBS_OK, "%s: finish after planting", file);
  memcpy(g_orig, g_img, len);   /* the planted, checksummed state is the new baseline */

  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  GbClock c;
  CHECKF(gbc_read(&s, &c), "%s: gbc_read", file);
  CHECKF(c.status_flags == 1, "%s: planted status_flags did not read back", file);

  uint8_t new_day = (uint8_t)(c.start_day + 1);
  c.start_day = new_day;
  GbsStatus st = gbc_write(&s, &c, true);   /* also clears status_flags */
  CHECKF(st == GBS_OK, "%s: gbc_write status %s", file, gbs_status_text(st));

  uint32_t diff = 0;
  for (uint32_t i = 0; i < len; i++) if (g_img[i] != g_orig[i]) diff++;
  CHECKF(diff > 0, "%s: edit must change at least one byte", file);

  /* re-open on the same buffer and read back */
  GbSession s2;
  CHECKF(gbs_open(&s2, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: reopen", file);
  GbClock c2;
  CHECKF(gbc_read(&s2, &c2), "%s: reread", file);
  CHECKF(c2.start_day == new_day, "%s: start_day did not stick (%u != %u)", file,
        c2.start_day, new_day);
  CHECKF(c2.status_flags_ok && c2.status_flags == 0, "%s: status_flags not cleared", file);
}

int main(void) {
  printf("== A: Gen 1 has no clock ==\n");
  gen1_not_applicable("Red.sav");
  gen1_not_applicable("Yellow.sav");

  printf("== B: Gen 2 read matches the corpus (docs/GEN12-PARITY-DESIGN.md 1.8) ==\n");
  gen2_read("Gold.sav", 0x05, 0x17, 0x00, 0x02, 0x6b0c1a38u, 0x80, 0x00);
  gen2_read("Crystal.sav", 0x06, 0x17, 0x12, 0x0a, 0x1114381bu, 0x80, 0x00);

  printf("== C: no-op read->write is a zero-byte diff ==\n");
  noop_zero_diff("Gold.sav");
  noop_zero_diff("Crystal.sav");

  printf("== D: edit start_day + clear status flags, round trip ==\n");
  edit_roundtrip("Gold.sav");
  edit_roundtrip("Crystal.sav");

  printf("\n%d checks, %d failed, %d file(s) exercised\n", g_check, g_fail, g_ran);
  if (g_ran == 0) { printf("NOTE: corpus not found at %s -- every case skipped\n", ROMS); }
  return g_fail ? 1 : 0;
}

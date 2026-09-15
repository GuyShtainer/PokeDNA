/* source/gb_hof.c + gen1_write_outside_sum (source/gen1_write.c) -- the Hall of Fame
 * core and its Gen-1 allowlist -- under test (BACKLOG #89).
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_gbhof_test.c \
 *      source/gb_hof.c source/gb_fields.c source/gb_session.c source/gb_edit.c \
 *      source/gen1_save.c source/gen1_write.c source/gen2_save.c source/gen2_write.c \
 *      source/data_tables.c source/gen3_to_gb.c source/gb_sidecar.c source/bank_cell.c source/gen3_edit.c \
 *      source/gen3_mon.c source/gen3_box.c source/gen3_save.c source/gen3_daycare.c \
 *      -o /tmp/hgbhof && /tmp/hgbhof
 *
 * Corpus: Guy's own cartridge dumps (gitignored, gba-toolkit/roms/gb/). A missing
 * corpus SKIPS rather than fails. Four saves: Red, Yellow, Gold, Crystal.
 *
 * ON THE "CHUNK-ROLLBACK INJECTION" the brief asks for: gen1_write_outside_sum's
 * per-chunk snapshot/restore (GEN1W_ERR_VERIFY branch) can only fire when a write
 * makes the image stop parsing (gen1_open fails) -- and every byte this allowlist can
 * ever touch (the HoF blob, [GEN1_OFF_HOF, GEN1_OFF_HOF+GEN1_HOF_BYTES)) sits entirely
 * outside GEN1_SUM_FIRST..LAST, the party blob and both box blobs -- literally nothing
 * gen1_open inspects. So a mid-loop transition from "parses" to "does not parse"
 * caused BY one of this function's own chunk writes is structurally unreachable, not
 * merely untested: there is no legal `buf` content that could trigger it. What IS
 * reachable, and what test G below proves, is the one failure this API can actually
 * hit -- an ALREADY-invalid image -- refusing before a single byte moves (0 chunks
 * committed, not "committed then rolled back"). Test H then proves the layer this
 * design deliberately delegates multi-chunk atomicity to (gb_session.h:152's
 * pristine-copy convention, the same one every other multi-step gb_session edit
 * relies on) actually round-trips byte-exact after a REAL multi-chunk clear.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gb_hof.h"
#include "gb_fields.h"
#include "gen1_write.h"

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

static uint32_t diff_count(const uint8_t* a, const uint8_t* b, uint32_t n, uint32_t* first) {
  uint32_t d = 0;
  for (uint32_t i = 0; i < n; i++)
    if (a[i] != b[i]) { if (!d && first) *first = i; d++; }
  return d;
}

/* ---- A: decode the real corpus teams, print them (Guy cross-checks against the
 * real game's League PC / PC capture by eye -- this test's own bar is "decodes
 * without crashing, every present mon has a plausible dex/level, no read past the
 * decoded count") ---- */

static void decode_real(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);

  int count = gbh_count(&s);
  int present = gbh_team_count_present(&s);
  printf("  %-12s lifetime count=%d, teams present=%d\n", file, count, present);
  CHECKF(present >= 0 && present <= 50, "%s: present %d out of range", file, present);
  CHECKF(count >= 0 && count <= 255, "%s: count %d out of range", file, count);
  /* present must never exceed what the count implies is possible to have STORED
   * (min(count,capacity)) -- an independent-scan overshoot would mean this scan is
   * reading garbage past what SaveHallOfFameTeams/AddHallOfFameEntry ever wrote. */
  int cap = (s.gen == GB_GEN1) ? 50 : 30;
  int implied = count < cap ? count : cap;
  CHECKF(present <= implied, "%s: present %d > count-implied max %d", file, present, implied);

  for (int i = 0; i < present; i++) {
    GbHofTeam t;
    CHECKF(gbh_team(&s, i, &t), "%s: gbh_team(%d)", file, i);
    printf("    #%d  %d mon(s)%s\n", i, t.n, s.gen == GB_GEN2 ? "" : "");
    CHECKF(t.n >= 1 && t.n <= 6, "%s: team %d mon count %d out of [1,6]", file, i, t.n);
    for (int m = 0; m < t.n; m++) {
      GbHofMon* mn = &t.mon[m];
      CHECKF(mn->present, "%s: team %d mon %d must be present (n=%d)", file, i, m, t.n);
      uint16_t maxdex = (s.gen == GB_GEN1) ? 151 : 251;
      CHECKF(mn->dex >= 1 && mn->dex <= maxdex, "%s: team %d mon %d dex %d out of [1,%d]",
            file, i, m, mn->dex, maxdex);
      CHECKF(mn->level >= 1 && mn->level <= 100, "%s: team %d mon %d level %d out of [1,100]",
            file, i, m, mn->level);
      printf("      dex=%3d lv=%3d nick='%s'\n", mn->dex, mn->level, mn->nick);
    }
    for (int m = t.n; m < 6; m++)
      CHECKF(!t.mon[m].present, "%s: team %d mon %d past n=%d must read absent",
            file, i, m, t.n);
  }
}

/* ---- B/C/D: clear empties everything, confines its diff to the blob+count(+mirror/
 * checksum on Gen 2), and (Gen 2 only) never touches sHallOfFameEnd ---- */

static void clear_confined(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);

  GbGame g = (s.gen == GB_GEN1) ? GBF_G_RED
                                  : ((s.g2w.sv.version == G2_VER_CRYSTAL) ? GBF_G_CRYSTAL : GBF_G_GS);
  uint32_t blob_off = gbf_off(g, GBF_HOF_TEAMS);
  uint32_t blob_len = gbf_len(g, GBF_HOF_TEAMS);
  uint32_t count_off = gbf_off(g, GBF_HOF_COUNT);
  CHECKF(blob_off && blob_len && count_off, "%s: HOF fields resolved", file);

  GbsStatus st = gbh_clear(&s);
  CHECKF(st == GBS_OK, "%s: gbh_clear status %s", file, gbs_status_text(st));

  CHECKF(gbh_count(&s) == 0, "%s: post-clear count != 0", file);
  CHECKF(gbh_team_count_present(&s) == 0, "%s: post-clear present != 0", file);

  /* Blob itself must be all-zero. */
  uint32_t nz = 0, first_nz = 0;
  for (uint32_t i = 0; i < blob_len; i++)
    if (g_img[blob_off + i] != 0) { if (!nz) first_nz = i; nz++; }
  CHECKF(nz == 0, "%s: %u non-zero byte(s) remain in the blob, first at blob+0x%04X",
        file, nz, first_nz);

  /* G/S: sHallOfFameEnd (blob_off+blob_len, the backup-checksum run's own start) and
   * everything at/after it must be untouched -- the boundary the brief calls out
   * explicitly (0x3D96 on GS). This is exact enough that an off-by-one widening of
   * the clear's own span would flip this CHECKF (the mutation the brief asks for). */
  if (g == GBF_G_GS || g == GBF_G_CRYSTAL) {
    uint32_t end_off = blob_off + blob_len;
    CHECKF(g_img[end_off] == g_orig[end_off], "%s: byte AT sHallOfFameEnd (0x%04X) "
          "changed -- the clear overran its own boundary by at least 1 byte",
          file, end_off);
    for (int k = 0; k < 8; k++)
      CHECKF(g_img[end_off + k] == g_orig[end_off + k],
            "%s: byte sHallOfFameEnd+%d (0x%04X) changed", file, k, end_off + k);
  }

  /* Whole-file diff confined to: the blob, the count byte, and (Gen 2 only) whatever
   * g2w_finish's own mirror(s) + both stored checksums touch -- everything ELSE must
   * be byte-identical to the original. Named footprint, not a blanket allowance. */
  uint32_t csum1 = 0, csum2 = 0;
  const G2MirrorRegion* mirrors = NULL;
  int nmirrors = 0;
  if (s.gen == GB_GEN2) {
    csum1 = g2_checksum_primary_off(s.g2w.sv.version);
    csum2 = g2_checksum_backup_off(s.g2w.sv.version);
    nmirrors = g2_mirror_map(s.g2w.sv.version, &mirrors);
  }
  uint32_t stray = 0, first_stray = 0;
  for (uint32_t i = 0; i < len; i++) {
    if (g_img[i] == g_orig[i]) continue;
    if (i >= blob_off && i < blob_off + blob_len) continue;
    if (i == count_off) continue;
    /* Gen 1: the main checksum byte legitimately changes -- the count byte it covers
     * just changed. gen1_write_fix_main_checksum's own footprint, named. */
    if (s.gen == GB_GEN1 && i == GEN1_OFF_CHECKSUM) continue;
    if (s.gen == GB_GEN2) {
      if (i == csum1 || i == csum1 + 1 || i == csum2 || i == csum2 + 1) continue;
      bool in_mirror = false;
      for (int k = 0; k < nmirrors; k++) {
        uint32_t span = mirrors[k].to - mirrors[k].from + 1u;
        if (i >= mirrors[k].dest && i < mirrors[k].dest + span) { in_mirror = true; break; }
      }
      if (in_mirror) continue;
    }
    if (!stray) first_stray = i;
    stray++;
  }
  CHECKF(stray == 0, "%s: %u byte(s) changed OUTSIDE the blob/count/checksum footprint, "
        "first at 0x%04X (0x%02X -> 0x%02X)", file, stray, first_stray,
        g_orig[first_stray], g_img[first_stray]);
}

/* ---- E: a second clear on an already-cleared save is a true no-op (0 bytes) ---- */

static void clear_noop(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  CHECKF(gbh_clear(&s) == GBS_OK, "%s: first clear", file);
  memcpy(g_orig, g_img, len);   /* re-baseline: the FIRST clear's diff is not this test's */
  CHECKF(gbh_clear(&s) == GBS_OK, "%s: second clear", file);
  uint32_t first = 0;
  uint32_t d = diff_count(g_img, g_orig, len, &first);
  CHECKF(d == 0, "%s: no-op clear changed %u byte(s), first at 0x%04X", file, d, first);
}

/* ---- F: count set + clamp (Gen 1 <= teams present up to GBH_G1_CAPACITY, D1;
 * Gen 2 <=200) ---- */

static void count_clamp(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);

  /* Gen 1's real cap under D1 is the teams present on this corpus save (can be < 3
   * on a lightly-played save like Yellow's 1 team); pick a target already within
   * that cap so "set 3" stays a meaningful mid-range check on every save. */
  int present = gbh_team_count_present(&s);
  int want3 = (s.gen == GB_GEN1 && present < 3) ? present : 3;

  CHECKF(gbh_set_count(&s, want3) == GBS_OK, "%s: set %d", file, want3);
  CHECKF(gbh_count(&s) == want3, "%s: readback %d, got %d", file, want3, gbh_count(&s));

  CHECKF(gbh_set_count(&s, want3) == GBS_OK, "%s: no-op set %d", file, want3);
  CHECKF(gbh_count(&s) == want3, "%s: still %d, got %d", file, want3, gbh_count(&s));

  int over = 9000;
  CHECKF(gbh_set_count(&s, over) == GBS_OK, "%s: set overflow", file);
  /* D1: Gen 1's cap is the teams actually present (never past GBH_G1_CAPACITY), NOT
   * a flat 255 -- a count past the stored teams would make the real League PC decode
   * empty slots. Gen 2 keeps the flat 200 (its own viewer re-derives the count). */
  int want = (s.gen == GB_GEN1) ? gbh_team_count_present(&s) : 200;
  CHECKF(gbh_count(&s) == want, "%s: clamp to %d, got %d", file, want, gbh_count(&s));

  CHECKF(gbh_set_count(&s, -5) == GBS_OK, "%s: set negative", file);
  CHECKF(gbh_count(&s) == 0, "%s: negative clamps to 0, got %d", file, gbh_count(&s));
}

/* ---- G: gen1_write_outside_sum -- the allowlist itself ---- */

static void gen1_allowlist(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  GbSession s;
  if (gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) != GBS_OK || s.gen != GB_GEN1) {
    printf("  SKIP %s (not a Gen-1 save)\n", file);
    return;
  }
  g_ran++;

  uint8_t buf[16] = {0};
  uint8_t snap[GBS_SCRATCH_BYTES];

  /* Fully outside the blob: refused, 0 bytes changed. */
  Gen1WStatus st = gen1_write_outside_sum(g_img, len, &s.g1, GEN1_OFF_HOF - 16, buf,
                                          sizeof buf, snap, sizeof snap);
  CHECKF(st == GEN1W_ERR_RANGE, "%s: fully-before refused, got %d", file, (int)st);

  /* Straddling the boundary (starts inside, ends past): refused. */
  st = gen1_write_outside_sum(g_img, len, &s.g1,
                              GEN1_OFF_HOF + GEN1_HOF_BYTES - 8, buf, sizeof buf,
                              snap, sizeof snap);
  CHECKF(st == GEN1W_ERR_RANGE, "%s: straddling-end refused, got %d", file, (int)st);

  /* Inside the checksummed window (a plain gen1_write_range_ex span): the allowlist
   * refuses it too -- this is NOT gen1_write_range_ex's job under a different name. */
  st = gen1_write_outside_sum(g_img, len, &s.g1, GEN1_SUM_FIRST, buf, sizeof buf,
                              snap, sizeof snap);
  CHECKF(st == GEN1W_ERR_RANGE, "%s: checksummed span refused via the allowlist too, "
        "got %d", file, (int)st);

  uint32_t d = diff_count(g_img, g_orig, len, NULL);
  CHECKF(d == 0, "%s: every refused call above left the image untouched (%u byte diff)",
        file, d);
}

/* ---- H: an already-invalid image refuses before a single byte moves (the one
 * failure this API can actually reach -- see the file header comment) ---- */

static void gen1_invalid_upfront(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  GbSession s;
  if (gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) != GBS_OK || s.gen != GB_GEN1) {
    printf("  SKIP %s (not a Gen-1 save)\n", file);
    return;
  }
  g_ran++;

  /* Break the main checksum -- outside the blob, so this alone proves nothing about
   * the allowlist's own bound; it exists purely to make gen1_open fail. */
  g_img[GEN1_OFF_CHECKSUM] ^= 0xFF;

  uint8_t zeros[96] = {0};
  uint8_t snap[GBS_SCRATCH_BYTES];
  Gen1WStatus st = gen1_write_outside_sum(g_img, len, &s.g1, GEN1_OFF_HOF, zeros,
                                          sizeof zeros, snap, sizeof snap);
  CHECKF(st == GEN1W_ERR_SAVE, "%s: pre-corrupted image refused with GEN1W_ERR_SAVE, "
        "got %d", file, (int)st);

  uint32_t nz = 0, first_nz = 0;
  for (uint32_t i = GEN1_OFF_HOF; i < GEN1_OFF_HOF + 96; i++)
    if (g_img[i] != g_orig[i]) { if (!nz) first_nz = i; nz++; }
  CHECKF(nz == 0, "%s: blob touched (%u byte(s), first at 0x%04X) despite the upfront "
        "refusal -- 0 chunks should ever have run", file, nz, first_nz);

  g_img[GEN1_OFF_CHECKSUM] ^= 0xFF;   /* undo, so later tests in this file see a clean save */
}

/* ---- I: the layer THIS design delegates multi-chunk atomicity to (a caller's own
 * pristine-copy + whole-image restore) round-trips byte-exact after a REAL
 * multi-chunk clear -- the actual mechanism gb_hof.h documents for "restore every
 * chunk written so far" ---- */

static void pristine_rollback_after_real_clear(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);

  static uint8_t pristine[G2_SAVE_SIZE + G2_MAX_RTC_TAIL];
  memcpy(pristine, g_img, len);

  CHECKF(gbh_clear(&s) == GBS_OK, "%s: clear", file);
  uint32_t changed = diff_count(g_img, pristine, len, NULL);
  CHECKF(changed > 0 || gbh_count(&s) == 0,
        "%s: clear on an already-empty save is fine (0 diff), otherwise expected >0",
        file);

  /* The screen's own convention on any failure: whole-image restore. Prove it is
   * byte-exact here on a SUCCESS path too (the mechanism has to work regardless of
   * why it was invoked). */
  memcpy(g_img, pristine, len);
  uint32_t d = diff_count(g_img, g_orig, len, NULL);
  CHECKF(d == 0, "%s: pristine restore did not reproduce the original image (%u byte "
        "diff)", file, d);
}

/* ---- J: D1, set-count clamped to teams present on Gen 1 ---- */

static void count_clamp_to_present_g1(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  GbSession s;
  if (gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) != GBS_OK || s.gen != GB_GEN1) {
    printf("  SKIP %s (not a Gen-1 save)\n", file);
    return;
  }
  g_ran++;

  /* D1, cleared: gbh_clear -> gbh_set_count(5) must stay at 0 (0 teams present). */
  CHECKF(gbh_clear(&s) == GBS_OK, "%s: clear", file);
  CHECKF(gbh_set_count(&s, 5) == GBS_OK, "%s: set 5 on a cleared HoF", file);
  CHECKF(gbh_count(&s) == 0, "%s: cleared HoF clamps set_count(5) to 0, got %d",
        file, gbh_count(&s));

  /* D1, un-cleared: reload, then gbh_set_count(40) must clamp to the teams present
   * (Guy's real Red.sav corpus: 9), not 40 and not a flat 255. */
  len = load(file);
  CHECKF(len != 0, "%s: reload after clear", file);
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: reopen", file);
  int present = gbh_team_count_present(&s);
  CHECKF(gbh_set_count(&s, 40) == GBS_OK, "%s: set 40", file);
  CHECKF(gbh_count(&s) == present, "%s: set_count(40) clamps to present=%d, got %d",
        file, present, gbh_count(&s));
}

/* ---- K: D2, present count clamped to gbh_count on virgin/noise SRAM ---- */

static void present_clamped_to_count_on_noise(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  GbSession s;
  if (gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) != GBS_OK || s.gen != GB_GEN1) {
    printf("  SKIP %s (not a Gen-1 save)\n", file);
    return;
  }
  g_ran++;

  /* D2: pokered never initialises sHallOfFame, so virgin SRAM can hold noise that
   * looks like occupied slots while gbh_count() (the real byte) still reads 0 --
   * simulate that directly: force count=0, then splat noise ONLY at each record's
   * species+level bytes (offsets 0/1 of every 16-byte mon-0 record, stride 96 per
   * team) so hof_slot_present() would read every slot as "occupied" if the
   * count-based clamp were missing.
   *
   * b89 re-verify A2-LEAK caveat: hof_slot_present() now ALSO rejects a slot whose
   * pad bytes (+13..+15) are non-zero or whose level byte is 0 (test M below) --
   * splatting the WHOLE blob with 0x42 (as this test did before A2-LEAK) would
   * make the pad-byte check reject every slot on its own, so gbh_slots_in_blob()
   * would already read 0 with NO help from D2's count-clamp, and this test would
   * stop discriminating D2 at all. Keep the pad bytes (+13..+15) explicitly ZERO
   * here so the RAW scan still reports every slot present (raw = 50, proven
   * below) and it is ONLY D2's gbh_count()-clamp that brings
   * gbh_team_count_present() down to 0 -- the thing this test actually exists to
   * prove. */
  CHECKF(gbh_set_count(&s, 0) == GBS_OK, "%s: force count 0", file);
  for (int i = 0; i < GBH_G1_CAPACITY; i++) {
    uint32_t team_off = GEN1_OFF_HOF + (uint32_t)i * GEN1_HOF_TEAM_BYTES;
    g_img[team_off + 0] = 0x42;   /* species: neither $00 nor $FF -- looks occupied */
    g_img[team_off + 1] = 0x42;   /* level: non-zero -- passes the new level check */
    g_img[team_off + 13] = 0;     /* pad bytes stay zero -- passes the new pad check */
    g_img[team_off + 14] = 0;
    g_img[team_off + 15] = 0;
  }

  CHECKF(gbh_count(&s) == 0, "%s: forced count reads back 0", file);
  int raw = gbh_slots_in_blob(&s);
  CHECKF(raw == GBH_G1_CAPACITY, "%s: raw scan must still see every slot as "
        "present (species+level noise, pad bytes zero) -- got %d, want %d",
        file, raw, GBH_G1_CAPACITY);
  int present = gbh_team_count_present(&s);
  CHECKF(present == 0, "%s: present clamps to count=0 on noise SRAM (D2), got %d",
        file, present);
}

/* ---- M: A2-LEAK, hof_slot_present()'s pad-byte/level discriminator on
 * fully-noise Gen-1 SRAM (species AND level AND pad bytes all garbage) -- the
 * scenario the species-only test could not tell apart from a real record. ---- */

static void raw_scan_rejects_full_noise(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  GbSession s;
  if (gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) != GBS_OK || s.gen != GB_GEN1) {
    printf("  SKIP %s (not a Gen-1 save)\n", file);
    return;
  }
  g_ran++;

  /* Virgin/noise SRAM: force count=0, then splat 0x42 across the WHOLE blob (every
   * byte of every record, pad bytes included) -- pokered never runs an Erase on
   * sHallOfFame, so this is what real power-on noise can look like. */
  CHECKF(gbh_set_count(&s, 0) == GBS_OK, "%s: force count 0", file);
  for (uint32_t i = GEN1_OFF_HOF; i < GEN1_OFF_HOF + GEN1_HOF_BYTES; i++)
    g_img[i] = 0x42;

  CHECKF(gbh_count(&s) == 0, "%s: forced count reads back 0", file);
  int raw = gbh_slots_in_blob(&s);
  CHECKF(raw == 0, "%s: hof_slot_present()'s pad/level discriminator must reject "
        "every full-noise slot on its own (no help from D2's count-clamp), got %d",
        file, raw);
  GbsStatus st = gbh_set_count(&s, 9000);
  CHECKF(st == GBS_OK, "%s: set_count(9000) on full-noise SRAM", file);
  CHECKF(gbh_count(&s) == 0, "%s: set_count(9000) on full-noise SRAM must leave "
        "the count at 0 (the ceiling is 0, not GBH_G1_CAPACITY), got %d",
        file, gbh_count(&s));
}

/* ---- L: R1, the D1<->D2 ratchet -- SET COUNT's own ceiling must be the raw,
 * unclamped slot scan (hof_raw_slots/gbh_slots_in_blob), never
 * gbh_team_count_present() (which D2 clamps to gbh_count() on Gen 1): deriving a
 * WRITE ceiling from a READ that clamps to the very value being written makes
 * SET COUNT a one-way ratchet -- lower the count once, and it can never go back
 * up, even though every team is still sitting untouched in the blob. Also proves
 * Gen 2 is NOT clamped the same way (pokecrystal's LoadHOFTeam bails on each
 * record's own win-count byte, not wHallOfFameCount) -- lowering the Gen-2 count
 * must not make gbh_team_count_present() under-report the teams LoadHOFTeam would
 * still show on the real PC. ---- */

static void ratchet_gen1(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  GbSession s;
  if (gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) != GBS_OK || s.gen != GB_GEN1) {
    printf("  SKIP %s (not a Gen-1 save)\n", file);
    return;
  }
  g_ran++;
  int raw = gbh_slots_in_blob(&s);   /* Guy's real Red.sav corpus: 9; Yellow's: 1 */
  /* A "lower than raw" midpoint that exists on every corpus save, not just Red's 9
   * (Yellow's own corpus save has only 1 team present, so a hardcoded 3 would
   * itself get clamped to 1 by D1 before this test ever reaches the ratchet it's
   * trying to prove -- same trap count_clamp()/F hit and fixed the same way). */
  int mid = (raw > 3) ? 3 : ((raw > 0) ? raw - 1 : 0);

  /* Down to mid, then back up to raw: must land on raw, not stay stuck at mid. */
  CHECKF(gbh_set_count(&s, mid) == GBS_OK, "%s: set %d", file, mid);
  CHECKF(gbh_count(&s) == mid, "%s: readback %d after set %d, got %d",
        file, mid, mid, gbh_count(&s));
  CHECKF(gbh_set_count(&s, raw) == GBS_OK, "%s: set %d (back up)", file, raw);
  CHECKF(gbh_count(&s) == raw, "%s: set_count(%d) after a lower set must land on "
        "%d (not ratcheted down to %d), got %d", file, raw, raw, mid, gbh_count(&s));

  /* Down to 0 (D4's "nothing to clear" trigger on the COUNT alone must not also
   * strand the real teams from ever being set back), then back up to raw. */
  CHECKF(gbh_set_count(&s, 0) == GBS_OK, "%s: set 0", file);
  CHECKF(gbh_count(&s) == 0, "%s: readback 0 after set 0, got %d", file, gbh_count(&s));
  CHECKF(gbh_slots_in_blob(&s) == raw, "%s: set_count(0) must not erase the blob -- "
        "%d team(s) still physically present, gbh_slots_in_blob() says %d",
        file, raw, gbh_slots_in_blob(&s));
  CHECKF(gbh_set_count(&s, raw) == GBS_OK, "%s: set %d (recover from 0)", file, raw);
  CHECKF(gbh_count(&s) == raw, "%s: set_count(%d) after set 0 must land on %d (not "
        "ratcheted to 0), got %d", file, raw, raw, gbh_count(&s));
}

static void gen2_not_clamped_by_count(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  GbSession s;
  if (gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) != GBS_OK || s.gen != GB_GEN2) {
    printf("  SKIP %s (not a Gen-2 save)\n", file);
    return;
  }
  g_ran++;
  int raw = gbh_slots_in_blob(&s);   /* Guy's real Crystal.sav corpus: 6 */

  CHECKF(gbh_set_count(&s, 2) == GBS_OK, "%s: set count to 2", file);
  CHECKF(gbh_count(&s) == 2, "%s: readback 2, got %d", file, gbh_count(&s));
  /* R1: the real HoF PC (pokecrystal's LoadHOFTeam) bails on each record's own
   * win-count byte, not wHallOfFameCount -- a lowered count must not make
   * gbh_team_count_present() under-report the teams the real game would still
   * show. This is what D2's Gen-1-only guard (b89 re-verify) exists to preserve. */
  CHECKF(gbh_team_count_present(&s) == raw, "%s: gbh_team_count_present() after "
        "set_count(2) must stay at the real slot count %d (not clamp to 2 like "
        "Gen 1 would), got %d", file, raw, gbh_team_count_present(&s));
}

int main(void) {
  const char* saves[] = { "Red.sav", "Yellow.sav", "Gold.sav", "Crystal.sav" };
  printf("== A: decode real corpus teams ==\n");
  for (size_t i = 0; i < sizeof saves / sizeof saves[0]; i++) decode_real(saves[i]);

  printf("== B/C/D: clear confined to blob+count(+checksum/mirror), G/S boundary ==\n");
  for (size_t i = 0; i < sizeof saves / sizeof saves[0]; i++) clear_confined(saves[i]);

  printf("== E: second clear is a true no-op ==\n");
  for (size_t i = 0; i < sizeof saves / sizeof saves[0]; i++) clear_noop(saves[i]);

  printf("== F: count set/clamp ==\n");
  for (size_t i = 0; i < sizeof saves / sizeof saves[0]; i++) count_clamp(saves[i]);

  printf("== G: gen1_write_outside_sum allowlist ==\n");
  for (size_t i = 0; i < sizeof saves / sizeof saves[0]; i++) gen1_allowlist(saves[i]);

  printf("== H: already-invalid image refuses upfront, 0 bytes touched ==\n");
  for (size_t i = 0; i < sizeof saves / sizeof saves[0]; i++) gen1_invalid_upfront(saves[i]);

  printf("== I: pristine-copy restore round-trips after a real multi-chunk clear ==\n");
  for (size_t i = 0; i < sizeof saves / sizeof saves[0]; i++)
    pristine_rollback_after_real_clear(saves[i]);

  printf("== J: D1 set-count clamps to teams present (Gen 1) ==\n");
  for (size_t i = 0; i < sizeof saves / sizeof saves[0]; i++)
    count_clamp_to_present_g1(saves[i]);

  printf("== K: D2 present clamps to count on noise SRAM ==\n");
  for (size_t i = 0; i < sizeof saves / sizeof saves[0]; i++)
    present_clamped_to_count_on_noise(saves[i]);

  printf("== L: R1 the D1<->D2 ratchet (Gen 1 SET COUNT can move back up; Gen 2 "
        "gbh_team_count_present() is not clamped by the count) ==\n");
  for (size_t i = 0; i < sizeof saves / sizeof saves[0]; i++)
    ratchet_gen1(saves[i]);
  for (size_t i = 0; i < sizeof saves / sizeof saves[0]; i++)
    gen2_not_clamped_by_count(saves[i]);

  printf("== M: A2-LEAK -- full-noise Gen-1 slots are rejected by the pad-byte/"
        "level discriminator, gbh_set_count(9000) leaves the count at 0 ==\n");
  for (size_t i = 0; i < sizeof saves / sizeof saves[0]; i++)
    raw_scan_rejects_full_noise(saves[i]);

  if (g_ran == 0) printf("  (no corpus present -- structural checks only, none ran)\n");
  printf("\n%d checks, %d failed (%d save(s) loaded)\n", g_check, g_fail, g_ran);
  return g_fail ? 1 : 0;
}

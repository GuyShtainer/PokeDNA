/* source/gb_clock.c -- the pure-C Gen-2 clock core -- under test.
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_gbclock_test.c \
 *      source/gb_clock.c source/gb_fields.c source/gb_session.c source/gb_edit.c \
 *      source/gen1_save.c source/gen1_write.c source/gen2_save.c source/gen2_write.c \
 *      source/data_tables.c source/gen3_to_gb.c source/gb_sidecar.c source/bank_cell.c source/gen3_edit.c \
 *      source/gen3_mon.c source/gen3_box.c source/gen3_save.c source/gen3_daycare.c \
 *      -o /tmp/hgbc && /tmp/hgbc
 *
 * Corpus: Guy's own cartridge dumps (gitignored, gba-toolkit/roms/gb/). A missing
 * corpus SKIPS rather than fails.
 *
 * P1a review D1: this core no longer claims the in-game clock is an absolute time it
 * can set -- wStartDay/Hour/Minute/Second are OFFSETS added to the hardware RTC by
 * FixTime, so the write surface is gbc_shift() (a signed delta), gbc_request_time_
 * reset() (asks the game to re-run its own clock-set prompt) and gbc_clear_status_
 * flags() (dismisses the error banner only). See gb_clock.h's header note.
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

static uint32_t diff_count(uint32_t len) {
  uint32_t d = 0;
  for (uint32_t i = 0; i < len; i++) if (g_img[i] != g_orig[i]) d++;
  return d;
}

/* ---- A: Gen 1 -- present must be false, every write op must refuse ---- */

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

  GbsStatus st = gbc_shift(&s, 1, 0, 0, 0);
  CHECKF(st == GBS_ERR_ARG, "%s: gbc_shift on Gen 1 must refuse (got %s)", file,
        gbs_status_text(st));
  CHECKF(diff_count(len) == 0, "%s: a refused shift must not move any byte", file);

  st = gbc_request_time_reset(&s);
  CHECKF(st == GBS_ERR_ARG, "%s: gbc_request_time_reset on Gen 1 must refuse (got %s)",
        file, gbs_status_text(st));
  CHECKF(diff_count(len) == 0, "%s: a refused reset must not move any byte", file);

  st = gbc_clear_status_flags(&s);
  CHECKF(st == GBS_ERR_ARG, "%s: gbc_clear_status_flags on Gen 1 must refuse (got %s)",
        file, gbs_status_text(st));
  CHECKF(diff_count(len) == 0, "%s: a refused clear must not move any byte", file);
}

/* ---- B: Gen 2 read matches the corpus (design doc §1.8, hand-verified) ---- */

static void gen2_read(const char* file, uint8_t off_day, uint8_t off_hour,
                      uint8_t off_min, uint8_t off_sec, uint32_t rtc_snapshot_be,
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
  CHECKF(c.rtc_offset_day == off_day, "%s: rtc_offset_day 0x%02X != 0x%02X", file,
        c.rtc_offset_day, off_day);
  CHECKF(c.rtc_offset_hour == off_hour, "%s: rtc_offset_hour 0x%02X != 0x%02X", file,
        c.rtc_offset_hour, off_hour);
  CHECKF(c.rtc_offset_minute == off_min, "%s: rtc_offset_minute 0x%02X != 0x%02X", file,
        c.rtc_offset_minute, off_min);
  CHECKF(c.rtc_offset_second == off_sec, "%s: rtc_offset_second 0x%02X != 0x%02X", file,
        c.rtc_offset_second, off_sec);
  uint32_t got = ((uint32_t)c.rtc_snapshot[0] << 24) | ((uint32_t)c.rtc_snapshot[1] << 16) |
                ((uint32_t)c.rtc_snapshot[2] << 8) | c.rtc_snapshot[3];
  CHECKF(got == rtc_snapshot_be, "%s: rtc_snapshot 0x%08X != 0x%08X", file, got, rtc_snapshot_be);
  CHECKF(c.dst == (dst_raw != 0), "%s: dst mismatch", file);
  CHECKF(c.status_flags_ok, "%s: status_flags_ok must be true", file);
  CHECKF(c.status_flags == status_flags, "%s: status_flags 0x%02X != 0x%02X", file,
        c.status_flags, status_flags);
}

/* ---- C: no-op is a zero-byte diff (shift by nothing; clear an already-0 flag) ---- */

static void noop_zero_diff(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);

  GbsStatus st = gbc_shift(&s, 0, 0, 0, 0);
  CHECKF(st == GBS_OK, "%s: no-op gbc_shift status %s", file, gbs_status_text(st));
  CHECKF(diff_count(len) == 0, "%s: a zero-delta shift must not move any byte", file);

  GbClock c;
  CHECKF(gbc_read(&s, &c), "%s: gbc_read", file);
  if (c.status_flags_ok && c.status_flags == 0) {
    st = gbc_clear_status_flags(&s);
    CHECKF(st == GBS_OK, "%s: clearing an already-0 flag status %s", file,
          gbs_status_text(st));
    CHECKF(diff_count(len) == 0, "%s: clearing an already-0 flag must not move any byte",
          file);
  }
}

/* ---- D: shift by +2h wraps correctly and moves ONLY the hour byte (no borrow) ---- */

static void shift_plus_2h(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;
  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  GbClock before;
  CHECKF(gbc_read(&s, &before), "%s: gbc_read before", file);

  GbsStatus st = gbc_shift(&s, 0, 2, 0, 0);
  CHECKF(st == GBS_OK, "%s: gbc_shift(+2h) status %s", file, gbs_status_text(st));

  GbGame g = gbc_game(&s);
  uint8_t new_hour_raw;
  CHECKF(gbs_read_field(&s, gbf_off(g, GBF_RTC_START_HOUR), &new_hour_raw, 1) == GBS_OK,
        "%s: read back new hour", file);
  uint8_t want_hour = (uint8_t)((before.rtc_offset_hour + 2) % 24);
  CHECKF(new_hour_raw == want_hour, "%s: hour offset moved by exactly 2 with wrap "
        "(%u -> %u, want %u)", file, before.rtc_offset_hour, new_hour_raw, want_hour);

  /* No carry expected into the day byte when the hour did not wrap past 23. */
  bool hour_wrapped = (before.rtc_offset_hour + 2) >= 24;
  uint8_t new_day_raw;
  CHECKF(gbs_read_field(&s, gbf_off(g, GBF_RTC_START_DAY), &new_day_raw, 1) == GBS_OK,
        "%s: read back day", file);
  uint8_t want_day = hour_wrapped ? (uint8_t)(before.rtc_offset_day + 1) : before.rtc_offset_day;
  CHECKF(new_day_raw == want_day, "%s: day offset carry (%u -> %u, want %u, wrapped=%d)",
        file, before.rtc_offset_day, new_day_raw, want_day, hour_wrapped);

  /* re-open and read back through the public API too */
  GbSession s2;
  CHECKF(gbs_open(&s2, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: reopen", file);
  GbClock after;
  CHECKF(gbc_read(&s2, &after), "%s: reread", file);
  CHECKF(after.rtc_offset_hour == want_hour, "%s: gbc_read hour did not stick", file);
  CHECKF(after.rtc_offset_minute == before.rtc_offset_minute,
        "%s: minute must be untouched by an hour-only shift", file);
  CHECKF(after.rtc_offset_second == before.rtc_offset_second,
        "%s: second must be untouched by an hour-only shift", file);
}

/* ---- E: request_time_reset writes exactly 0x0C60 = 0x80, no other byte ---- */

static void request_reset(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;

  /* Prime the checksums first, same discipline clear_flags() below already uses: a
   * genuine gbs_finish() cycle can "fix" a stale BACKUP checksum some corpus saves
   * carry from years of untouched retail play (the same class of staleness
   * gb_retail_gate.py's own mirror-rescue case documents; Guy's own Gold.sav has one).
   * Diffing against a freshly-finished baseline isolates what THIS call moves instead
   * of also catching that one-time, pre-existing fix. */
  GbSession ps;
  CHECKF(gbs_open(&ps, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: prime open", file);
  GbGame pg = gbc_game(&ps);
  uint8_t cur_flags = 0;
  CHECKF(gbs_read_field(&ps, gbf_off(pg, GBF_RTC_STATUS_FLAGS), &cur_flags, 1) == GBS_OK,
        "%s: prime read", file);
  CHECKF(gbs_write_field(&ps, gbf_off(pg, GBF_RTC_STATUS_FLAGS), &cur_flags, 1) == GBS_OK,
        "%s: prime write-back (same value)", file);
  CHECKF(gbs_finish(&ps) == GBS_OK, "%s: prime finish", file);
  memcpy(g_orig, g_img, len);   /* the primed, freshly-checksummed state is the baseline */

  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);

  GbsStatus st = gbc_request_time_reset(&s);
  CHECKF(st == GBS_OK, "%s: gbc_request_time_reset status %s", file, gbs_status_text(st));

  uint32_t diff = 0, only_off = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < len; i++)
    if (g_img[i] != g_orig[i]) { diff++; only_off = i; }
  CHECKF(diff == 1, "%s: request_time_reset must move exactly 1 byte (moved %u)", file, diff);
  CHECKF(only_off == 0x0C60u, "%s: the moved byte must be sRTCStatusFlags at 0x0C60 "
        "(moved 0x%04X)", file, only_off);
  CHECKF(g_img[0x0C60] == 0x80u, "%s: 0x0C60 must read 0x80 (RTC_RESET), got 0x%02X",
        file, g_img[0x0C60]);

  /* idempotent: a second call on the same state is a true no-op */
  st = gbc_request_time_reset(&s);
  CHECKF(st == GBS_OK, "%s: second request_time_reset status %s", file, gbs_status_text(st));
  diff = 0;
  for (uint32_t i = 0; i < len; i++) if (g_img[i] != g_orig[i]) diff++;
  CHECKF(diff == 1, "%s: a repeat request_time_reset must not move any further byte "
        "(now %u total)", file, diff);
}

/* ---- F: clear_status_flags zeroes a nonzero flag and nothing else ---- */

static void clear_flags(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;

  /* plant a nonzero status_flags byte first */
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

  GbsStatus st = gbc_clear_status_flags(&s);
  CHECKF(st == GBS_OK, "%s: gbc_clear_status_flags status %s", file, gbs_status_text(st));

  uint32_t diff = 0, only_off = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < len; i++)
    if (g_img[i] != g_orig[i]) { diff++; only_off = i; }
  CHECKF(diff == 1, "%s: clear_status_flags must move exactly 1 byte (moved %u)", file, diff);
  CHECKF(only_off == 0x0C60u, "%s: the moved byte must be sRTCStatusFlags (moved 0x%04X)",
        file, only_off);
  CHECKF(g_img[0x0C60] == 0, "%s: 0x0C60 must read 0 after clear, got 0x%02X", file,
        g_img[0x0C60]);
}

/* ---- G: the carry chain BOTH ways -- a backward shift across midnight AND a day
 * boundary, in one call, exercising every `d_X += carry` propagation line and the
 * negative branch of wrap_add()'s ternary at all four levels (BACKLOG #86/#108,
 * P1a review D1's own "the carry chain both ways" requirement -- test D above only
 * ever proved the forward, no-borrow case). Plants a known 00:00:00 on day 5, then
 * shifts by exactly -1 second: seconds borrow from minutes, minutes borrow from
 * hours, hours borrow from days -- 59:59:23 on day 4, the same wrap a real clock
 * ticking backward across midnight would show. */
static void shift_minus_1s_cascades(const char* file) {
  uint32_t len = load(file);
  if (!len) { printf("  SKIP %s (not present)\n", file); return; }
  g_ran++;

  GbSession ps;
  CHECKF(gbs_open(&ps, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: plant open", file);
  GbGame pg = gbc_game(&ps);
  uint8_t zero = 0, day5 = 5;
  CHECKF(gbs_write_field(&ps, gbf_off(pg, GBF_RTC_START_DAY), &day5, 1) == GBS_OK, "%s: plant day", file);
  CHECKF(gbs_write_field(&ps, gbf_off(pg, GBF_RTC_START_HOUR), &zero, 1) == GBS_OK, "%s: plant hour", file);
  CHECKF(gbs_write_field(&ps, gbf_off(pg, GBF_RTC_START_MINUTE), &zero, 1) == GBS_OK, "%s: plant minute", file);
  CHECKF(gbs_write_field(&ps, gbf_off(pg, GBF_RTC_START_SECOND), &zero, 1) == GBS_OK, "%s: plant second", file);
  CHECKF(gbs_finish(&ps) == GBS_OK, "%s: finish after planting", file);
  memcpy(g_orig, g_img, len);   /* the planted, checksummed 5d 00:00:00 is the baseline */

  GbSession s;
  CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK, "%s: open", file);
  GbGame g = gbc_game(&s);

  GbsStatus st = gbc_shift(&s, 0, 0, 0, -1);
  CHECKF(st == GBS_OK, "%s: gbc_shift(-1s) status %s", file, gbs_status_text(st));

  uint8_t d, h, m, sec;
  CHECKF(gbs_read_field(&s, gbf_off(g, GBF_RTC_START_DAY), &d, 1) == GBS_OK, "%s: read day", file);
  CHECKF(gbs_read_field(&s, gbf_off(g, GBF_RTC_START_HOUR), &h, 1) == GBS_OK, "%s: read hour", file);
  CHECKF(gbs_read_field(&s, gbf_off(g, GBF_RTC_START_MINUTE), &m, 1) == GBS_OK, "%s: read minute", file);
  CHECKF(gbs_read_field(&s, gbf_off(g, GBF_RTC_START_SECOND), &sec, 1) == GBS_OK, "%s: read second", file);
  CHECKF(d == 4, "%s: day borrows 5 -> 4 (got %u)", file, d);
  CHECKF(h == 23, "%s: hour wraps 0 -> 23 (got %u)", file, h);
  CHECKF(m == 59, "%s: minute wraps 0 -> 59 (got %u)", file, m);
  CHECKF(sec == 59, "%s: second wraps 0 -> 59 (got %u)", file, sec);
  /* Not an exact byte-diff count here (unlike test E/F's single-byte flag writes): all
   * four offset bytes move PLUS whichever checksum bytes gbs_finish() recomputes over
   * them, so an exact count would just re-encode the checksum span's own width. The
   * four field values above are the real assertion -- get any one of them wrong (a
   * dropped `+= carry`, or wrap_add()'s negative branch never taken) and this test
   * fails on that value, not on a byte-count proxy for it. */
}

/* ---- H: nine hand-derived deltas off Crystal's OWN corpus baseline (06 17 12 0a),
 * BACKLOG #108 R2 -- the reviewer's own table. Test G above only ever starts from a
 * planted, all-zero 00:00:00 baseline, so a second->minute or minute->hour carry
 * always lands on the same boundary value (59) regardless of which modulus the code
 * actually used; several of these nine deltas exercise a NON-boundary minute/hour
 * result (case 2 lands on minute=59 via a genuine -19 delta, not a lucky -1; case 9
 * cancels three fields to exactly zero without ever going negative) so a transcribed
 * modulus (e.g. minutes wrapping at 24 instead of 60) or an off-by-one in the
 * exact-zero path can no longer hide behind the boundary coincidence test G's own
 * single case has. Each row reloads the corpus file fresh (load() re-reads from disk)
 * so the nine cases are independent, not cumulative. */
static void nine_deltas_table(const char* file, uint8_t base_day, uint8_t base_hour,
                              uint8_t base_min, uint8_t base_sec) {
  static const struct {
    int32_t dd, dh, dm, ds;
    uint8_t want_day, want_hour, want_min, want_sec;
  } kCase[] = {
    /* dd   dh   dm   ds  | day hour min sec */
    {  0,   0,   0, -11,    6,  23,  17,  59 },  /* second->minute borrow, non-boundary */
    {  0,   0, -19,   0,    6,  22,  59,  10 },  /* minute->hour borrow, non-boundary   */
    {  0, -24,   0,   0,    5,  23,  18,  10 },  /* exact -24h: whole negative branch   */
    { -1,   0,   0,   0,    5,  23,  18,  10 },  /* plain day decrement (cross-check)   */
    {  0,   0,   0,  -1,    6,  23,  18,   9 },  /* second-only, no borrow              */
    {  0,   1,   0,   0,    7,   0,  18,  10 },  /* hour->day carry, forward            */
    {  0,   0,   0,  50,    6,  23,  19,   0 },  /* second->minute carry, forward       */
    {  1,   0,   0,   0,    7,  23,  18,  10 },  /* plain day increment                 */
    {  0, -23, -18, -10,    6,   0,   0,   0 },  /* cancels to exact 0:00:00, no borrow */
  };

  for (size_t i = 0; i < sizeof kCase / sizeof kCase[0]; i++) {
    uint32_t len = load(file);
    if (!len) { printf("  SKIP %s (not present)\n", file); return; }
    g_ran++;
    GbSession s;
    CHECKF(gbs_open(&s, g_img, len, g_scratch, sizeof g_scratch) == GBS_OK,
          "%s: case %zu open", file, i);
    GbGame g = gbc_game(&s);

    /* Sanity: every row's own hand-derived expectation assumes this exact baseline. */
    uint8_t bd, bh, bm, bs;
    CHECKF(gbs_read_field(&s, gbf_off(g, GBF_RTC_START_DAY), &bd, 1) == GBS_OK &&
          bd == base_day, "%s: case %zu baseline day", file, i);
    CHECKF(gbs_read_field(&s, gbf_off(g, GBF_RTC_START_HOUR), &bh, 1) == GBS_OK &&
          bh == base_hour, "%s: case %zu baseline hour", file, i);
    CHECKF(gbs_read_field(&s, gbf_off(g, GBF_RTC_START_MINUTE), &bm, 1) == GBS_OK &&
          bm == base_min, "%s: case %zu baseline minute", file, i);
    CHECKF(gbs_read_field(&s, gbf_off(g, GBF_RTC_START_SECOND), &bs, 1) == GBS_OK &&
          bs == base_sec, "%s: case %zu baseline second", file, i);

    GbsStatus st = gbc_shift(&s, kCase[i].dd, kCase[i].dh, kCase[i].dm, kCase[i].ds);
    CHECKF(st == GBS_OK, "%s: case %zu gbc_shift(%d,%d,%d,%d) status %s", file, i,
          kCase[i].dd, kCase[i].dh, kCase[i].dm, kCase[i].ds, gbs_status_text(st));

    uint8_t d, h, m, sec;
    CHECKF(gbs_read_field(&s, gbf_off(g, GBF_RTC_START_DAY), &d, 1) == GBS_OK, "%s: case %zu read day", file, i);
    CHECKF(gbs_read_field(&s, gbf_off(g, GBF_RTC_START_HOUR), &h, 1) == GBS_OK, "%s: case %zu read hour", file, i);
    CHECKF(gbs_read_field(&s, gbf_off(g, GBF_RTC_START_MINUTE), &m, 1) == GBS_OK, "%s: case %zu read minute", file, i);
    CHECKF(gbs_read_field(&s, gbf_off(g, GBF_RTC_START_SECOND), &sec, 1) == GBS_OK, "%s: case %zu read second", file, i);
    CHECKF(d == kCase[i].want_day, "%s: case %zu (%d,%d,%d,%d) day %u != %u", file, i,
          kCase[i].dd, kCase[i].dh, kCase[i].dm, kCase[i].ds, d, kCase[i].want_day);
    CHECKF(h == kCase[i].want_hour, "%s: case %zu (%d,%d,%d,%d) hour %u != %u", file, i,
          kCase[i].dd, kCase[i].dh, kCase[i].dm, kCase[i].ds, h, kCase[i].want_hour);
    CHECKF(m == kCase[i].want_min, "%s: case %zu (%d,%d,%d,%d) minute %u != %u", file, i,
          kCase[i].dd, kCase[i].dh, kCase[i].dm, kCase[i].ds, m, kCase[i].want_min);
    CHECKF(sec == kCase[i].want_sec, "%s: case %zu (%d,%d,%d,%d) second %u != %u", file, i,
          kCase[i].dd, kCase[i].dh, kCase[i].dm, kCase[i].ds, sec, kCase[i].want_sec);
  }
}

int main(void) {
  printf("== A: Gen 1 has no clock ==\n");
  gen1_not_applicable("Red.sav");
  gen1_not_applicable("Yellow.sav");

  printf("== B: Gen 2 read matches the corpus (docs/GEN12-PARITY-DESIGN.md 1.8) ==\n");
  gen2_read("Gold.sav", 0x05, 0x17, 0x00, 0x02, 0x6b0c1a38u, 0x80, 0x00);
  gen2_read("Crystal.sav", 0x06, 0x17, 0x12, 0x0a, 0x1114381bu, 0x80, 0x00);

  printf("== C: no-op is a zero-byte diff ==\n");
  noop_zero_diff("Gold.sav");
  noop_zero_diff("Crystal.sav");

  printf("== D: shift +2h wraps correctly, no unexpected carry ==\n");
  shift_plus_2h("Gold.sav");
  shift_plus_2h("Crystal.sav");

  printf("== E: request_time_reset writes exactly 0x0C60 = 0x80 ==\n");
  request_reset("Gold.sav");
  request_reset("Crystal.sav");

  printf("== F: clear_status_flags zeroes exactly the one byte ==\n");
  clear_flags("Gold.sav");
  clear_flags("Crystal.sav");

  printf("== G: the carry chain both ways (backward, cascading through all 4 bytes) ==\n");
  shift_minus_1s_cascades("Gold.sav");
  shift_minus_1s_cascades("Crystal.sav");

  printf("== H: nine hand-derived deltas off Crystal's own baseline (BACKLOG #108 R2) ==\n");
  nine_deltas_table("Crystal.sav", 0x06, 0x17, 0x12, 0x0a);

  printf("\n%d checks, %d failed, %d file(s) exercised\n", g_check, g_fail, g_ran);
  if (g_ran == 0) { printf("NOTE: corpus not found at %s -- every case skipped\n", ROMS); }
  return g_fail ? 1 : 0;
}

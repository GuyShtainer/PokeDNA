/* Host test for gen3_stars.c's ACH_MUSEUM branch of pk_star_ach_set -- BACKLOG #105.
 * Build + run (repo root):
 *   cc -std=c11 -O2 -Wall -Wextra -I source tests/host_stars_test.c \
 *     source/gen3_stars.c source/gen3_flags.c source/gen3_dex.c \
 *     source/gen3_frontier.c source/gen3_trainer.c source/gen3_contest.c \
 *     source/gen3_save.c source/gen3_mon.c source/data_tables.c \
 *     source/gen3_daycare.c source/gen3_edit.c \
 *     -o /tmp/hstars && /tmp/hstars
 *
 * The bug this pins down: museum_fill() used to write the museum slot's byte +10
 * as the plain 0..4 contest category. That byte is not the category -- it is a
 * "painting caption id" = 3*category + a 0..2 flavor-text variant (gc_museum_get/
 * gc_museum_set's own header comment in gen3_contest.h, cross-derived from
 * pokeemerald src/contest.c and pokeruby src/contest_2.c). Slot 0 (Cool, category
 * 0) happened to look right either way (0*3+0 == 0), which is exactly how the bug
 * hid for that one slot while corrupting every other slot's in-game caption/sprite.
 * The fix makes museum_fill call gc_museum_set (the contest module's own writer)
 * instead of hand-writing bytes, so there is one place that knows the encoding.
 *
 * Checks, for both Emerald and Ruby/Sapphire (different museum SB1 offsets):
 *   1) pk_star_ach_set(..., ACH_MUSEUM index, on=true, ...) fills all 5 slots;
 *   2) gc_museum_get() reads back category == k for every slot k;
 *   3) the RAW byte at +10 equals 3*k+0 (this fill's variant, chosen by
 *      gc_museum_set: 0 is a real value the game itself can roll, not a
 *      sentinel) and stays under 5*3=15 -- fails for k>=1 on the old `w[10]=cat`
 *      code (variant 0 only coincides with plain category at k==0), confirmed
 *      by hand-reverting museum_fill and re-running before committing this fix.
 *
 * Takes no .sav argument -- SB1/SB2 are synthetic buffers, sized and laid out
 * only as far as the museum branch and its trainer-identity reads need.
 */
#include <stdio.h>
#include <string.h>
#include "gen3_stars.h"
#include "gen3_contest.h"
#include "gen3_save.h"
#include "gen3_edit.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); fails++; } } while (0)

static uint8_t g_sb1[G3_SAVEBLOCK1_BYTES];
static uint8_t g_sb2[0x100];

/* Emerald star index 2 and RS star index 3 are "All 5 museum paintings" per
 * gen3_stars.h's own documented per-game achievement list -- not re-derived here,
 * just used as the public contract this test exercises. */
static void run_game(PkGame g, int museum_i, const char* label) {
  printf("-- %s --\n", label);
  memset(g_sb1, 0, sizeof(g_sb1));
  memset(g_sb2, 0, sizeof(g_sb2));

  /* Trainer identity the fill will copy: OT id + a short Gen-3 encoded name. */
  static const char* NAME = "TESTER";
  for (int k = 0; NAME[k]; k++) g_sb2[k] = gen3_encode_char(NAME[k]);
  g_sb2[6] = 0xFF;
  g_sb2[0x0A] = 0x34; g_sb2[0x0B] = 0x12;   /* trainerId lo16 = 0x1234 */
  g_sb2[0x0C] = 0x00; g_sb2[0x0D] = 0x00;   /* trainerId hi16 = 0x0000 */

  int mask = pk_star_ach_set(g_sb1, g_sb2, g, museum_i, true, NULL);
  CHECK(mask == 1, "ACH_MUSEUM on should dirty SB1 only");
  CHECK(pk_star_ach_done(g_sb1, g_sb2, g, museum_i, NULL), "museum star should read done after fill");

  for (int k = 0; k < GC_MUSEUM_COUNT; k++) {
    GcWinner w;
    char msg[64];
    bool ok = gc_museum_get(g_sb1, g, k, &w);
    snprintf(msg, sizeof(msg), "gc_museum_get should succeed for slot %d", k);
    CHECK(ok, msg);
    if (!ok) continue;

    snprintf(msg, sizeof(msg), "slot %d category should read back as %d", k, k);
    CHECK(w.category == (uint8_t)k, msg);

    snprintf(msg, sizeof(msg), "slot %d species should be Pikachu (25)", k);
    CHECK(w.species == 25, msg);

    uint32_t off = gc_museum_offset(g, k);
    CHECK(off != 0, "gc_museum_offset should be nonzero for a supported game");
    uint8_t raw = g_sb1[off + 10];
    uint8_t expect = (uint8_t)(k * 3);   /* 3*category + variant 0 (this fill's choice) */
    snprintf(msg, sizeof(msg), "slot %d raw caption byte should be 3*category (got %d, want %d)",
             k, raw, expect);
    CHECK(raw == expect, msg);
    snprintf(msg, sizeof(msg), "slot %d raw caption byte should stay under 5*3=15 (got %d)", k, raw);
    CHECK(raw < 5 * 3, msg);

    /* #408: check PAINTING_MADE flag is set */
    bool flag = pk_flag_get(g_sb1, g, 0xA0 + k);
    snprintf(msg, sizeof(msg), "#408: slot %d PAINTING_MADE flag should be set after fill", k);
    CHECK(flag, msg);
  }
}

/* ---- BACKLOG #105 review A1 fix: museum_fill's trainer-name bytes must be a
 * verbatim copy, not a decode/re-encode round trip -- gen3_decode_char maps any
 * byte outside the plain-text subset to '?' (0xAC, source/gen3_save.c:52 default
 * case) and gc_encode_char has no case for '?' either (it falls through to its
 * own default, 0x00 = space) -- ONE mapping loses two different byte classes.
 * Proof by construction, no need to hand-revert and re-run: for raw byte 0xB5
 * (MALE_SYMBOL, source/rom_text.c:104) and 0x1D (a ligature), gen3_decode_char
 * has no case for either -- both hit the `default: return '?';` branch. The old
 * museum_fill code then handed that '?' string to gc_museum_set, which encodes
 * ASCII via gc_encode_char -- '?' hits ITS default case too (0x00, a space),
 * not even 0xAC. Either way the round trip is provably lossy: the byte written
 * to the save can never be 0xB5 or 0x1D again once it has passed through
 * gen3_decode_char. This test seeds sb2's OT-name bytes with exactly those two
 * raw bytes (plus 6 more non-terminator bytes -- a full 8-byte name with no
 * 0xFF padding at all) and checks the museum record's trainer-name field is a
 * byte-for-byte copy of sb2's first 8 bytes -- which only gc_museum_set_raw's
 * memcpy (not the old decode/gc_museum_set path) can produce. */
static void test_raw_name_bytes(void) {
  printf("-- raw trainer-name bytes round-trip (Emerald) --\n");
  memset(g_sb1, 0, sizeof(g_sb1));
  memset(g_sb2, 0, sizeof(g_sb2));

  uint8_t raw8[8] = { 0xB5, 0x1D,
                      gen3_encode_char('C'), gen3_encode_char('D'),
                      gen3_encode_char('E'), gen3_encode_char('F'),
                      gen3_encode_char('G'), gen3_encode_char('H') };
  memcpy(g_sb2, raw8, 8);
  g_sb2[0x0A] = 0x78; g_sb2[0x0B] = 0x56;   /* trainerId lo16 */
  g_sb2[0x0C] = 0x34; g_sb2[0x0D] = 0x12;   /* trainerId hi16 */

  /* Sanity-check the proof-by-construction claim above still holds in this
   * build's tables (would only drift if gen3_decode_char/gc_encode_char's
   * charmaps changed underneath this test). */
  CHECK(gen3_decode_char(0xB5) == '?', "0xB5 should be unmapped in gen3_decode_char (proof premise)");
  CHECK(gen3_decode_char(0x1D) == '?', "0x1D should be unmapped in gen3_decode_char (proof premise)");

  int mask = pk_star_ach_set(g_sb1, g_sb2, PK_EMERALD, 2 /* museum star idx */, true, NULL);
  CHECK(mask == 1, "ACH_MUSEUM on should dirty SB1 only");

  for (int k = 0; k < GC_MUSEUM_COUNT; k++) {
    uint32_t off = gc_museum_offset(PK_EMERALD, k);
    char msg[96];
    snprintf(msg, sizeof(msg),
             "slot %d trainer-name bytes should equal sb2's raw 8 bytes exactly", k);
    CHECK(off != 0 && memcmp(g_sb1 + off + 22, raw8, 8) == 0, msg);
  }
}

/* "keep real wins": a slot that already has a species must be left untouched by
 * a fresh ON pass, sentinel byte included -- pk_star_ach_set's own
 * `else if (rd16(w + 8) == 0) museum_fill(...)` guard (gen3_stars.c) is what
 * this pins down. */
static void test_keep_real_wins(void) {
  printf("-- keep real wins (slot 0 untouched) --\n");
  memset(g_sb1, 0, sizeof(g_sb1));
  memset(g_sb2, 0, sizeof(g_sb2));
  static const char* NAME = "TESTER";
  for (int k = 0; NAME[k]; k++) g_sb2[k] = gen3_encode_char(NAME[k]);
  g_sb2[6] = 0xFF;
  g_sb2[0x0A] = 0x34; g_sb2[0x0B] = 0x12;
  g_sb2[0x0C] = 0x00; g_sb2[0x0D] = 0x00;

  uint32_t off0 = gc_museum_offset(PK_EMERALD, 0);
  uint8_t snapshot[GC_RECORD_BYTES];
  g_sb1[off0 + 8] = (uint8_t)999;         /* nonzero species lo byte: slot already won */
  g_sb1[off0 + 9] = (uint8_t)(999 >> 8);
  g_sb1[off0 + 31] = 0xAA;                /* sentinel byte just past the rank field */
  memcpy(snapshot, g_sb1 + off0, GC_RECORD_BYTES);

  int mask = pk_star_ach_set(g_sb1, g_sb2, PK_EMERALD, 2, true, NULL);
  CHECK(mask == 1, "ACH_MUSEUM on should dirty SB1 only");
  CHECK(memcmp(g_sb1 + off0, snapshot, GC_RECORD_BYTES) == 0,
       "slot 0's pre-existing real win must be byte-identical after an ON pass");

  /* #408: check that the pre-existing win's flag is now set (heals lost flag) */
  bool flag = pk_flag_get(g_sb1, PK_EMERALD, 0xA0);
  CHECK(flag, "#408: slot 0's pre-existing real win's PAINTING_MADE flag should be set without rewriting the record");
}

/* OFF must zero all 5 * 0x20 = 0xA0 museum bytes -- gen3_stars.c's
 * `if (!on) memset(w, 0, 0x20);` branch, one slot at a time. */
static void test_off_zeroes(void) {
  printf("-- OFF zeroes all 5 museum slots --\n");
  memset(g_sb1, 0, sizeof(g_sb1));
  memset(g_sb2, 0, sizeof(g_sb2));
  static const char* NAME = "TESTER";
  for (int k = 0; NAME[k]; k++) g_sb2[k] = gen3_encode_char(NAME[k]);
  g_sb2[6] = 0xFF;

  pk_star_ach_set(g_sb1, g_sb2, PK_EMERALD, 2, true, NULL);   /* fill all 5 first */
  int mask = pk_star_ach_set(g_sb1, g_sb2, PK_EMERALD, 2, false, NULL);
  CHECK(mask == 1, "ACH_MUSEUM off should dirty SB1 only");

  uint32_t base = gc_museum_offset(PK_EMERALD, 0);
  uint8_t zeros[GC_MUSEUM_COUNT * GC_RECORD_BYTES];
  memset(zeros, 0, sizeof(zeros));
  CHECK(memcmp(g_sb1 + base, zeros, sizeof(zeros)) == 0,
       "all 5 * 0x20 museum bytes should be zero after OFF");

  /* #408: check all PAINTING_MADE flags are cleared */
  for (int k = 0; k < GC_MUSEUM_COUNT; k++) {
    bool flag = pk_flag_get(g_sb1, PK_EMERALD, 0xA0 + k);
    char msg[64];
    snprintf(msg, sizeof(msg), "#408: slot %d PAINTING_MADE flag should be cleared after OFF", k);
    CHECK(!flag, msg);
  }
}

int main(void) {
  run_game(PK_EMERALD, 2, "Emerald museum_off 0x2F90");
  run_game(PK_RS,       3, "Ruby/Sapphire museum_off 0x2EFC");
  test_raw_name_bytes();
  test_keep_real_wins();
  test_off_zeroes();

  if (fails) {
    printf("%d check(s) FAILED\n", fails);
    return 1;
  }
  printf("all checks passed\n");
  return 0;
}

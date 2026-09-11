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
  }
}

int main(void) {
  run_game(PK_EMERALD, 2, "Emerald museum_off 0x2F90");
  run_game(PK_RS,       3, "Ruby/Sapphire museum_off 0x2EFC");

  if (fails) {
    printf("%d check(s) FAILED\n", fails);
    return 1;
  }
  printf("all checks passed\n");
  return 0;
}

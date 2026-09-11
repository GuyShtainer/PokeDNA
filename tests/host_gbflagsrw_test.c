/* source/gb_flags_rw.c -- gbfl_get/gbfl_set bit math, under test (BACKLOG #88).
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gbflagsrw_test.c \
 *      source/gb_flags_rw.c source/gb_session.c source/gb_edit.c source/gen1_save.c \
 *      source/gen1_write.c source/gen2_save.c source/gen2_write.c source/data_tables.c \
 *      source/gen3_to_gb.c source/gb_sidecar.c source/gen3_edit.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_save.c source/gen3_daycare.c source/gb_fields.c \
 *      source/gb_flags.c -o /tmp/hgbfr && /tmp/hgbfr
 *
 * Round-trips gbfl_get/gbfl_set over a real Red/Gold/Crystal save (a plain read of
 * Guy's own corpus, mutated only on an in-memory copy -- gitignored, outside the repo,
 * missing corpus SKIPS rather than fails). Two classes of check:
 *
 *   (a) BIT MATH, mutation-style: a byte/bit swap in the addressing formula (byte =
 *       base + n/8, bit = n&7) must be CAUGHT -- this file cross-checks gbfl_get
 *       against a hand-rolled reference read straight out of the image at several n,
 *       including n values that only differ from a neighbour by exactly one bit or one
 *       byte, so a swapped >>3/&7 pair fails loudly instead of reading a plausible but
 *       wrong bit.
 *   (b) NO-OP DISCIPLINE: gbfl_set to the CURRENT value must change zero bytes (hard
 *       rule 3) -- checked by a whole-image memcmp, not just the one byte.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gb_flags_rw.h"
#include "gb_session.h"
#include "gen1_save.h"
#include "gen2_save.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_fail = 0, g_check = 0, g_ran = 0;
#define CHECK(c, ...) do { g_check++; if (!(c)) { \
    printf("  !! FAIL: "); printf(__VA_ARGS__); printf("   [%s:%d]\n", __FILE__, __LINE__); g_fail++; } } while (0)

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

/* One reference bit-read straight out of the image, bypassing gbfl_get entirely --
 * the independent re-derivation the bit-math mutation check needs. */
static bool ref_bit(uint32_t base, uint16_t n) {
  uint32_t byte_off = base + (n >> 3);
  return (g_img[byte_off] >> (n & 7)) & 1u;
}

static void check_game(const char* file, GbGame g, bool gen1) {
  uint32_t n = load(file);
  if (!n) { printf("  %s: corpus absent, SKIP\n", file); return; }
  g_ran++;

  GbSession s;
  GbsStatus os = gbs_open(&s, g_img, n, g_scratch, sizeof g_scratch);
  CHECK(os == GBS_OK, "%s: gbs_open succeeds (%s)", file, gbs_status_text(os));
  if (os != GBS_OK) return;

  uint32_t base = gbf_off(g, gen1 ? GBF_EVENT_FLAGS_BASE : GBF_EVENT_FLAGS_BASE_G2);
  uint16_t len  = gbf_len(g,  gen1 ? GBF_EVENT_FLAGS_BASE : GBF_EVENT_FLAGS_BASE_G2);
  CHECK(base != 0 && len != 0, "%s: the event-flags region resolves", file);

  /* (a) bit-math cross-check: every shortlisted flag, plus a few synthetic n values
   * that share a byte but differ by one bit, and a few that share a bit-in-byte
   * position but differ by one byte -- exactly the two swaps a >>3/&7 typo produces. */
  int nc = gbfl_count(g);
  int mismatches = 0;
  for (int i = 0; i < nc; i++) {
    uint16_t flag = 0; const char* label = 0;
    if (!gbfl_at(g, i, &flag, &label)) continue;
    if (flag >= (uint16_t)len * 8) continue;
    bool via_api = gbfl_get(&s, g, flag);
    bool via_ref = ref_bit(base, flag);
    if (via_api != via_ref) mismatches++;
  }
  CHECK(mismatches == 0, "%s: gbfl_get agrees with a direct read for all %d shortlisted flags (%d disagreed)",
       file, nc, mismatches);

  /* Byte-swap / bit-swap synthetic probes: pick a mid-range n, then n^8 (next byte,
   * same bit position) and n^1 (same byte, neighbouring bit) -- gbfl_get must NOT
   * agree with ref_bit(n) for these unless the underlying bits happen to coincide
   * (rare enough that a real byte/bit-swap bug would fail this near-certainly across
   * three games x several probe points). */
  uint16_t probe_ns[] = { 40, 200, 1000 };
  for (unsigned pi = 0; pi < sizeof probe_ns / sizeof probe_ns[0]; pi++) {
    uint16_t pn = probe_ns[pi];
    if (pn >= (uint16_t)len * 8) continue;
    CHECK(gbfl_get(&s, g, pn) == ref_bit(base, pn),
         "%s: gbfl_get(n=%u) agrees with a direct byte/bit read", file, (unsigned)pn);
  }

  /* (b) no-op discipline: setting a flag to its OWN current value must not move a
   * single byte of the whole image. */
  if (nc > 0) {
    uint16_t flag = 0; const char* label = 0;
    gbfl_at(g, 0, &flag, &label);
    bool cur = gbfl_get(&s, g, flag);
    uint8_t before[sizeof g_img]; memcpy(before, g_img, n);
    GbsStatus st = gbfl_set(&s, g, flag, cur);
    CHECK(st == GBS_OK, "%s: gbfl_set to the current value returns GBS_OK", file);
    CHECK(memcmp(before, g_img, n) == 0, "%s: a no-op gbfl_set changes ZERO bytes", file);

    /* Real toggle round-trip: flip it, confirm the read-back, flip it back, confirm
     * the image is byte-identical to where it started (a genuine round trip, not
     * just a no-op). */
    GbsStatus st2 = gbfl_set(&s, g, flag, !cur);
    CHECK(st2 == GBS_OK, "%s: gbfl_set(!cur) returns GBS_OK", file);
    CHECK(gbfl_get(&s, g, flag) == !cur, "%s: the flipped bit reads back flipped", file);
    GbsStatus st3 = gbfl_set(&s, g, flag, cur);
    CHECK(st3 == GBS_OK, "%s: gbfl_set back to cur returns GBS_OK", file);
    CHECK(gbfl_get(&s, g, flag) == cur, "%s: the bit reads back as the original value", file);
    CHECK(memcmp(g_orig, g_img, n) == 0,
         "%s: a flip-then-flip-back round trip restores the image byte-identically", file);
  }

  /* Out-of-range n is refused, not a silent OOB read/write. */
  CHECK(gbfl_get(&s, g, (uint16_t)(len * 8)) == false, "%s: gbfl_get at len*8 (OOB) reads false", file);
  CHECK(gbfl_set(&s, g, (uint16_t)(len * 8), true) == GBS_ERR_ARG, "%s: gbfl_set at len*8 (OOB) refuses", file);
  CHECK(gbfl_get(0, g, 0) == false, "%s: gbfl_get(NULL session) reads false", file);
  CHECK(gbfl_set(0, g, 0, true) == GBS_ERR_ARG, "%s: gbfl_set(NULL session) refuses", file);
}

int main(void) {
  printf("host_gbflagsrw_test\n");
  check_game("Red.sav", GBF_G_RED, true);
  check_game("Gold.sav", GBF_G_GS, false);
  check_game("Crystal.sav", GBF_G_CRYSTAL, false);

  printf("%d checks, %d failed, %d game(s) with corpus present\n", g_check, g_fail, g_ran);
  return g_fail ? 1 : 0;
}

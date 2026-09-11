/* Host test for pdna_yard.h's pure roll core (BACKLOG #114, pure C, no hardware).
 *   cc -std=c11 -I source tests/host_yard_test.c -o /tmp/hy && /tmp/hy
 * pdna_yard_roll_core() is a static inline in the header, so this test needs no
 * companion .c file (unlike pdna_yard.c itself, which pulls in tonc.h and cannot
 * be host-compiled). */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "pdna_yard.h"

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* A hand-rolled mutant of the OLD, pre-#114 body (`% 251` hard-coded regardless
 * of `max_dex`) -- proves the test below actually distinguishes the two, i.e.
 * kills the mutation the brief names ("mutation (`% 251` hard-coded) fails the
 * Gen-1 case"). */
static uint32_t mutant_hardcoded_251(uint32_t seed, uint16_t out_sp[PDNA_YARD_MAXDECO], int* out_n) {
  uint32_t rng = seed;
  rng = rng * 1103515245u + 12345u;
  *out_n = 2 + (int)((rng >> 16) % 4);
  for (int i = 0; i < *out_n; i++) {
    rng = rng * 1103515245u + 12345u;
    out_sp[i] = (uint16_t)(1 + (rng >> 9) % 251);   /* the bug: ignores max_dex */
  }
  return rng | 1u;
}

int main(void) {
  /* 1) A spread of fixed seeds, both caps: every rolled dex must land in
   * [1, max_dex], count in [2, PDNA_YARD_MAXDECO]. */
  const uint32_t seeds[] = { 1u, 0x12345678u, 0xDEADBEEFu, 0x9E3779B9u, 0x00000001u,
                              0xFFFFFFFFu, 0x51EED000u, 7u };
  const uint16_t caps[] = { 151, 251 };
  for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); c++) {
    uint16_t max_dex = caps[c];
    for (size_t s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++) {
      uint16_t sp[PDNA_YARD_MAXDECO];
      int n = -1;
      uint32_t final_rng = pdna_yard_roll_core(seeds[s], max_dex, sp, &n);
      CHECK(n >= 2 && n <= PDNA_YARD_MAXDECO, "cap=%u seed=%#x: n=%d out of [2,%d]",
            max_dex, seeds[s], n, PDNA_YARD_MAXDECO);
      CHECK((final_rng & 1u) == 1u, "cap=%u seed=%#x: final rng not OR'd with 1 (%#x)",
            max_dex, seeds[s], final_rng);
      for (int i = 0; i < n; i++)
        CHECK(sp[i] >= 1 && sp[i] <= max_dex, "cap=%u seed=%#x: sp[%d]=%u out of [1,%u]",
              max_dex, seeds[s], i, sp[i], max_dex);
    }
  }

  /* 2) Mutation-kill: the Gen-1 cap (151) must be able to distinguish the real
   * core from the old hard-coded-251 body -- some seed must roll a species in
   * (151, 251] under the mutant that the real core never can. Search until one
   * is found (guaranteed to exist: species 152..251 is 40% of the 1..251
   * range, so a handful of seeds suffices) -- bounded loop, no infinite scan. */
  int found_violation = 0;
  for (uint32_t seed = 1; seed <= 4096 && !found_violation; seed++) {
    uint16_t sp[PDNA_YARD_MAXDECO];
    int n;
    mutant_hardcoded_251(seed, sp, &n);
    for (int i = 0; i < n; i++)
      if (sp[i] > 151) { found_violation = 1; break; }
  }
  CHECK(found_violation, "could not find a seed where the %% 251-hardcoded mutant "
                          "rolls > 151 within 4096 tries -- the mutation-kill is too weak");

  /* And the REAL core, on the exact same seeds, must never violate the 151 cap --
   * this is the actual regression the brief is pinning. */
  for (uint32_t seed = 1; seed <= 4096; seed++) {
    uint16_t sp[PDNA_YARD_MAXDECO];
    int n;
    pdna_yard_roll_core(seed, 151, sp, &n);
    for (int i = 0; i < n; i++)
      CHECK(sp[i] <= 151, "seed=%u: real core rolled %u > 151 under max_dex=151", seed, sp[i]);
  }

  if (fails) { printf("%d FAILURE(S)\n", fails); return 1; }
  printf("OK\n");
  return 0;
}

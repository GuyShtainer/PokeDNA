/* Mirage Island: the LCG pre-image, the party scan, and the days-owed maths, against Guy's
 * own Emerald/Ruby/Sapphire saves.
 * Build + run (repo root):
 *   cc -I source tests/host_mirage_test.c source/gen3_mirage.c source/gen3_save.c -o /tmp/hm \
 *     && /tmp/hm <emerald.sav> [ruby.sav] [sapphire.sav]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gen3_trainer.h"
#include "gen3_mirage.h"
#include "gen3_save.h"

static int fails;
#define CHECK(c,msg) do{ if(!(c)){ printf("FAIL: %s\n", msg); fails++; } }while(0)

static uint8_t g_buf[G3_SAVE_FILE_SIZE], g_sb1[G3_SAVEBLOCK1_BYTES];

static void one(const char* path, PkGame game, const char* label) {
  FILE* f = fopen(path, "rb"); if (!f) { printf("skip %s\n", path); return; }
  size_t sz = fread(g_buf, 1, sizeof g_buf, f); fclose(f);
  Gen3SaveInfo info;
  if (!gen3_parse(g_buf, (uint32_t)sz, &info) || !gen3_read_saveblock1(g_buf, info.slot, g_sb1)) {
    printf("skip %s (unreadable)\n", path); return; }

  uint16_t hi, lo;
  CHECK(mirage_get(g_sb1, game, &hi, &lo), "mirage_get succeeds on an RSE save");
  uint16_t key[MIRAGE_PARTY_SLOTS]; uint8_t slot[MIRAGE_PARTY_SLOTS];
  int n = mirage_party_keys(g_sb1, key, slot);
  printf("%-9s VAR_MIRAGE_RND H=%04X L=%04X  party=%d  present=%s\n",
         label, hi, lo, n, mirage_present(g_sb1, game) ? "YES" : "no");
  CHECK(n >= 1 && n <= MIRAGE_PARTY_SLOTS, "party has 1..6 usable donors");
  for (int i = 0; i < n; i++) printf("            slot %d key %04X\n", slot[i], key[i]);

  /* THE PROPERTY THAT MATTERS: for every step count, solving and then forward-advancing must
   * land exactly on the target. This is what makes the write correct on a save that owes days. */
  for (int steps = 0; steps <= 80; steps++) {
    for (int i = 0; i < n; i++) {
      uint16_t sh, sl, ah, al;
      mirage_solve(key[i], lo, steps, &sh, &sl);
      mirage_advance(sh, sl, steps, &ah, &al);
      if (ah != key[i]) { printf("FAIL: solve/advance mismatch steps=%d slot=%d\n", steps, i); fails++; }
    }
  }
  printf("            solve->advance round-trips for all 6 donors x 0..80 days\n");

  /* And the write really would make the island appear once the catch-up has run. */
  uint16_t sh, sl;
  mirage_solve(key[0], lo, 5, &sh, &sl);
  uint8_t tmp[G3_SAVEBLOCK1_BYTES]; memcpy(tmp, g_sb1, sizeof tmp);
  CHECK(mirage_set(tmp, game, sh, sl), "mirage_set writes");
  CHECK(!mirage_present(tmp, game) || key[0] == sh, "pre-image is not the target (unless 0 days)");
  uint16_t ah, al; mirage_advance(sh, sl, 5, &ah, &al);
  mirage_set(tmp, game, ah, al);
  CHECK(mirage_present(tmp, game), "after 5 simulated rollovers the island IS present");

  /* days_owed never goes negative even with the clock wound back. */
  CHECK(mirage_days_owed(g_sb1, game, 0) == 0, "a past cur_day owes nothing");
  int owed = mirage_days_owed(g_sb1, game, 99999);
  CHECK(owed > 0, "a far-future cur_day owes something");
  printf("            days owed at day 99999: %d\n", owed);
}

int main(int argc, char** argv) {
  /* The LCG inverse constant is load-bearing; prove it rather than trusting the comment. */
  CHECK((uint32_t)(1103515245u * 4005161829u) == 1u, "LCG_MINV really inverts LCG_M mod 2^32");
  /* No fixed point => three consecutive days is impossible. Spot-check the claim. */
  { int fixed = 0; for (uint32_t t = 0; t < 200000u; t++) if (1103515245u * t + 12345u == t) fixed++;
    CHECK(fixed == 0, "LCG has no fixed point in the sampled range"); }

  if (argc > 1) one(argv[1], PK_EMERALD, "Emerald");
  if (argc > 2) one(argv[2], PK_RS,      "Ruby");
  if (argc > 3) one(argv[3], PK_RS,      "Sapphire");
  printf(fails ? "\n%d FAILURES\n" : "\nOK: host_mirage_test (0 failures)\n", fails);
  return fails ? 1 : 0;
}

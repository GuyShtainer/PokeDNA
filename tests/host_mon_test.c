/* Host (PC) test for the full per-mon decoder. Loads a real .sav fixture,
 * parses it, reads the live party, prints every decoded field, and runs sanity
 * assertions. Build + run:
 *   cc -I source tests/host_mon_test.c source/gen3_save.c source/gen3_mon.c -o /tmp/hm
 *   /tmp/hm tests/fixtures/POKEMON_EMER_BPEE00.sav
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_save.h"
#include "gen3_mon.h"

static const char* NAT[6] = { "HP", "Atk", "Def", "Spe", "SpA", "SpD" };

int main(int argc, char** argv) {
  const char* path = (argc > 1) ? argv[1] : "tests/fixtures/POKEMON_EMER_BPEE00.sav";
  FILE* f = fopen(path, "rb");
  if (!f) { printf("cannot open %s\n", path); return 2; }
  static uint8_t save[G3_SAVE_FILE_SIZE];
  size_t n = fread(save, 1, sizeof(save), f);
  fclose(f);
  printf("== %s (%zu bytes) ==\n", path, n);

  Gen3SaveInfo info;
  if (!gen3_parse(save, (uint32_t)n, &info)) { printf("gen3_parse FAILED\n"); return 1; }
  printf("slot=%d version_guess=%d sb1_ok=%d trainer=\"%s\" TID=%u SID=%u play=%uh%02um\n",
         info.slot, info.version_guess, info.sb1_ok, info.trainer_name,
         info.tid_public, info.tid_secret, info.play_h, info.play_m);

  static uint8_t sb1[G3_SAVEBLOCK1_BYTES];
  if (gen3_read_saveblock1(save, info.slot, sb1) != G3_SAVEBLOCK1_BYTES) {
    printf("SaveBlock1 reassemble FAILED\n");
    return 1;
  }

  bool frlg = false;
  PkMon party[6];
  int n_mon = pk_read_party_auto(sb1, party, &frlg);   /* layout by validity, not the unreliable version_guess */
  printf("party: %d mon (frlg=%d)\n", n_mon, frlg);

  int fails = 0;
  for (int i = 0; i < n_mon; i++) {
    PkMon* p = &party[i];
    printf("\n[%d] %-10s spc=%3u Lv%-3u nat=%2u %s%s%s abilityNum=%u\n",
           i, p->nickname, p->species, p->level, p->nature,
           p->isShiny ? "SHINY " : "", p->isEgg ? "EGG " : "", p->isBadEgg ? "BADEGG " : "",
           p->abilityNum);
    printf("    OT=%-7s otId=%08X item=%u exp=%u friend=%u\n",
           p->otName, p->otId, p->heldItem, p->experience, p->friendship);
    printf("    IV:");
    for (int s = 0; s < 6; s++) printf(" %s=%u", NAT[s], p->ivs[s]);
    printf("\n    EV:");
    for (int s = 0; s < 6; s++) printf(" %s=%u", NAT[s], p->evs[s]);
    printf("  sum=%u\n", p->evSum);
    printf("    ST:");
    for (int s = 0; s < 6; s++) printf(" %s=%u", NAT[s], p->stats[s]);
    printf("\n    moves: %u/%u %u/%u %u/%u %u/%u\n",
           p->moves[0], p->pp[0], p->moves[1], p->pp[1],
           p->moves[2], p->pp[2], p->moves[3], p->pp[3]);
    printf("    met: loc=%u lvl=%u game=%u ball=%u otGender=%u pokerus=%u ribbons=%08X\n",
           p->metLocation, p->metLevel, p->metGame, p->pokeball, p->otGender, p->pokerus, p->ribbons);

    for (int s = 0; s < 6; s++)
      if (p->ivs[s] > 31) { printf("    !! IV %s = %u > 31\n", NAT[s], p->ivs[s]); fails++; }
    if (p->evSum > 510)        { printf("    !! EV sum %u > 510\n", p->evSum); fails++; }
    if (p->nature > 24)        { printf("    !! nature %u > 24\n", p->nature); fails++; }
    if (p->level < 1 || p->level > 100) { printf("    !! level %u out of range\n", p->level); fails++; }
    if (p->species == 0 || p->species > 411) { printf("    !! species %u out of range\n", p->species); fails++; }
    if (!p->isEgg && p->stats[PK_HP] == 0) { printf("    !! HP stat is 0\n"); fails++; }
  }

  printf("\n%s: %d sanity failure(s)\n", fails ? "FAIL" : "OK", fails);
  return fails ? 1 : 0;
}

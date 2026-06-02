/* Host test for PC box reading. Reassembles PC storage, decodes a couple of
 * boxes, and prints species/computed-level for occupied slots.
 *   cc -std=c11 -I source tests/host_box_test.c source/gen3_save.c \
 *      source/gen3_mon.c source/gen3_box.c source/data_tables.c -o /tmp/hb
 *   /tmp/hb tests/fixtures/POKEMON_EMER_BPEE00.sav
 */
#include <stdio.h>
#include <stdint.h>
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "data_tables.h"

static const char* G[3] = { "M", "F", "-" };

int main(int argc, char** argv) {
  const char* path = (argc > 1) ? argv[1] : "tests/fixtures/POKEMON_EMER_BPEE00.sav";
  FILE* f = fopen(path, "rb");
  if (!f) { printf("cannot open %s\n", path); return 2; }
  static uint8_t save[G3_SAVE_FILE_SIZE];
  size_t n = fread(save, 1, sizeof(save), f);
  fclose(f);

  Gen3SaveInfo info;
  if (!gen3_parse(save, (uint32_t)n, &info)) { printf("parse FAILED\n"); return 1; }

  static uint8_t pc[G3_PC_BYTES];
  if (gen3_read_pc_storage(save, info.slot, pc) != G3_PC_BYTES) { printf("PC reassemble FAILED\n"); return 1; }

  printf("== %s : current box = %u ==\n", path, pk_current_box(pc));
  int total = 0, fails = 0;
  for (int b = 0; b < G3_TOTAL_BOXES; b++) {
    PkMon box[30];
    int occ = pk_read_box(pc, b, box);
    if (occ == 0) continue;
    char bn[12]; pk_box_name(pc, b, bn);
    printf("\nBox %d \"%s\" (%d):\n", b + 1, bn, occ);
    for (int s = 0; s < 30; s++) {
      PkMon* p = &box[s];
      if (p->species == 0) continue;
      total++;
      printf("  [%2d] %-10s %-11s Lv%-3u %s  IVsum=%u EVsum=%u\n",
             s, p->nickname, pk_species_name(p->species), p->level, G[p->gender],
             p->ivs[0] + p->ivs[1] + p->ivs[2] + p->ivs[3] + p->ivs[4] + p->ivs[5], p->evSum);
      if (p->species > 411) { printf("    !! species OOR\n"); fails++; }
      if (p->level < 1 || p->level > 100) { printf("    !! level OOR\n"); fails++; }
    }
  }
  printf("\n%s: %d box mons, %d failures\n", fails ? "FAIL" : "OK", total, fails);
  return fails ? 1 : 0;
}

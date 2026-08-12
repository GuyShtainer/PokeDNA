/* Host test for PC box reading. Reassembles PC storage, decodes a couple of
 * boxes, and prints species/computed-level for occupied slots.
 *   cc -std=c11 -I source tests/host_box_test.c source/gen3_save.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_edit.c source/gen3_daycare.c source/data_tables.c -o /tmp/hb
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
  int total = 0, fails = 0, glitch = 0;
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
      /* An out-of-range species is NOT a parse failure. Real saves contain glitch
       * Pokemon — Guy's Emerald has two (slot 7 "DOTS" at Lv100 with EVsum 636, and a
       * blank-named record in slot 19) that he built deliberately as ACE payloads, and
       * his FireRed has one. The parser is SUPPOSED to hand them back as-is; refusing
       * to decode them would break the very saves this tool exists to inspect, and
       * gen3_legality2 already grades them ILLEGAL ("Species id out of range"), which
       * is where that judgement belongs. So count them, report them, and only fail if
       * the save is nothing BUT glitch records — which would mean the decode really is
       * broken rather than the data being odd. */
      if (p->species > 411) { printf("    (glitch record — expected in an ACE save)\n"); glitch++; }
      if (p->level < 1 || p->level > 100) { printf("    !! level OOR\n"); fails++; }
    }
  }
  /* All-glitch means the decode is broken; a handful among real Pokemon is just this
   user's save. Guard the degenerate case so the relaxation cannot hide a regression. */
  if (total && glitch == total) { printf("    !! EVERY record is a glitch — decode is broken\n"); fails++; }
  printf("\n%s: %d box mons, %d failures, %d glitch record(s)\n",
         fails ? "FAIL" : "OK", total, fails, glitch);
  return fails ? 1 : 0;
}

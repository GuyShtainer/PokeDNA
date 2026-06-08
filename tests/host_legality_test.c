/* Host test for the structural legality checker (gen3_legality.c).
 *   cc -std=c11 -I source tests/host_legality_test.c source/gen3_save.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_legality.c source/data_tables.c -o /tmp/hl
 *   /tmp/hl tests/fixtures/POKEMON_EMER_BPEE00.sav
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "gen3_legality.h"
#include "data_tables.h"

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)

static int illegal_count(const PkLegality* L) {
  int c = 0; for (int i = 0; i < L->n; i++) if (L->issue[i].sev) c++; return c;
}

int main(int argc, char** argv) {
  const char* path = (argc > 1) ? argv[1] : "tests/fixtures/POKEMON_EMER_BPEE00.sav";
  FILE* f = fopen(path, "rb");
  if (!f) { printf("cannot open %s\n", path); return 2; }
  static uint8_t save[G3_SAVE_FILE_SIZE];
  size_t n = fread(save, 1, sizeof(save), f);
  fclose(f);
  Gen3SaveInfo info;
  if (!gen3_parse(save, (uint32_t)n, &info)) { printf("parse FAILED\n"); return 1; }

  static uint8_t sb1[G3_SAVEBLOCK1_BYTES], pc[G3_PC_BYTES];
  gen3_read_saveblock1(save, info.slot, sb1);
  gen3_read_pc_storage(save, info.slot, pc);

  PkMon party[6]; bool frlg = false;
  int np = pk_read_party_auto(sb1, party, &frlg);
  printf("== %s ==\n", path);

  /* (1) every real mon with a VALID species (1..411) is structurally legal; the
   *     fixtures also hold a few garbage box slots (species id out of range) that
   *     the checker must correctly flag as illegal. */
  int valid_tested = 0, valid_flagged = 0, garbage_total = 0, garbage_caught = 0;
  #define VISIT(m) do { \
    pk_resolve(&(m)); \
    PkLegality L = pk_check_legality(&(m)); \
    if ((m).species >= 1 && (m).species <= 411) { \
      valid_tested++; \
      if (!L.ok) { valid_flagged++; printf("  VALID flagged %s: %d illegal\n", pk_species_name((m).species), illegal_count(&L)); } \
    } else { garbage_total++; if (!L.ok) garbage_caught++; } \
  } while (0)
  for (int i = 0; i < np; i++) VISIT(party[i]);
  for (int b = 0; b < G3_TOTAL_BOXES; b++) {
    PkMon box[30]; pk_read_box(pc, b, box);
    for (int s = 0; s < 30; s++) if (box[s].species != 0) VISIT(box[s]);
  }
  printf("(1) valid mons: %d tested, %d flagged; garbage slots: %d caught/%d\n",
         valid_tested, valid_flagged, garbage_caught, garbage_total);
  CHECK(valid_flagged == 0, "all valid-species mons are structurally legal");
  CHECK(garbage_caught == garbage_total, "all garbage-species slots are flagged illegal");

  /* (2) a deliberately-corrupted mon is flagged */
  if (np > 0) {
    PkMon m = party[0];
    for (int i = 0; i < 6; i++) m.evs[i] = 255;   /* EV total 1530 > 510 */
    m.moves[0] = 500;                              /* move id out of range */
    m.metLevel = 100; m.level = 5;                 /* met above level */
    PkLegality L = pk_check_legality(&m);
    int ill = illegal_count(&L);
    printf("(2) corrupted mon: ok=%d, %d issues (%d illegal)\n", L.ok, L.n, ill);
    CHECK(!L.ok, "corrupted mon is flagged illegal");
    CHECK(ill >= 3, "catches EV total + bad move + met level");
  }

  printf("\n%s: %d failure(s)\n", g_fail ? "FAIL" : "OK", g_fail);
  return g_fail ? 1 : 0;
}

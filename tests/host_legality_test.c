/* Host test for the structural legality checker (gen3_legality.c).
 *   cc -std=c11 -I source tests/host_legality_test.c source/gen3_save.c source/gen3_mon.c \
 *      source/gen3_box.c source/gen3_legality.c source/learnsets.c source/data_tables.c -o /tmp/hl
 *   /tmp/hl tests/fixtures/POKEMON_EMER_BPEE00.sav
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "gen3_legality.h"
#include "learnsets.h"
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
  int move_warns = 0, origin_warns = 0, loc_warns = 0;
  #define VISIT(m) do { \
    pk_resolve(&(m)); \
    PkLegality L = pk_check_legality(&(m)); \
    if ((m).species >= 1 && (m).species <= 411) { \
      valid_tested++; \
      if (!L.ok) { valid_flagged++; printf("  VALID flagged %s: %d illegal\n", pk_species_name((m).species), illegal_count(&L)); } \
      for (int _i = 0; _i < L.n; _i++) { \
        if (!strncmp(L.issue[_i].text, "Move not learnable", 18)) { \
          move_warns++; printf("  move-warn on %s (legit?)\n", pk_species_name((m).species)); } \
        if (!strncmp(L.issue[_i].text, "Unusual origin", 14)) { \
          origin_warns++; printf("  origin-warn on %s (legit?)\n", pk_species_name((m).species)); } \
        if (!strncmp(L.issue[_i].text, "Invalid met location", 20)) { \
          loc_warns++; printf("  loc-warn on %s (legit?)\n", pk_species_name((m).species)); } \
      } \
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
  printf("(1b) move-source warnings on valid mons: %d (expect 0 — no false positives)\n", move_warns);
  CHECK(move_warns == 0, "no false-positive move-source warnings on legit mons");
  printf("(1d) origin-game warnings on valid mons: %d (expect 0)\n", origin_warns);
  CHECK(origin_warns == 0, "no false-positive origin-game warnings on legit mons");
  printf("(1e) met-location warnings on valid mons: %d (expect 0)\n", loc_warns);
  CHECK(loc_warns == 0, "no false-positive met-location warnings on legit mons");

  /* (1c) regression guard for known code-taught moves that no data file lists and
   *      must be accepted explicitly: Volt Tackle (Light Ball egg move) on the
   *      Pichu/Pikachu/Raichu line, and the FRLG ultimate starter tutor moves. */
  CHECK(pk_can_learn(172, 344) && pk_can_learn(25, 344) && pk_can_learn(26, 344),
        "Pichu/Pikachu/Raichu line can learn Volt Tackle");
  CHECK(pk_can_learn(6, 307) && pk_can_learn(9, 308) && pk_can_learn(3, 338),
        "starter finals can learn Blast Burn/Hydro Cannon/Frenzy Plant");

  /* (2) a deliberately-corrupted mon is flagged */
  CHECK(np > 0, "fixture has at least one party mon to test with");
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

  /* (3) a valid mon taught a move its species line can never learn raises the
   *     move-source warning (and only that — it stays structurally legal). */
  if (np > 0) {
    PkMon m = party[0];
    uint16_t bad = 0;                       /* find a real move it can't learn */
    for (uint16_t mv = 1; mv <= 354; mv++)
      if (!pk_can_learn(m.species, mv)) { bad = mv; break; }
    if (bad) {
      m.moves[0] = bad; m.pp[0] = 1;
      PkLegality L = pk_check_legality(&m);
      int warns = 0;
      for (int i = 0; i < L.n; i++)
        if (!strncmp(L.issue[i].text, "Move not learnable", 18)) warns++;
      printf("(3) %s taught %s: %d move-warning(s)\n",
             pk_species_name(m.species), pk_move_name(bad), warns);
      CHECK(warns >= 1, "unlearnable move raises a move-source warning");
    }
  }

  /* (4) origin/met sanity: a 0 origin game warns; a met level over 100 is illegal. */
  if (np > 0) {
    PkMon m = party[0];
    m.metGame = 0;
    PkLegality L = pk_check_legality(&m);
    int ow = 0; for (int i = 0; i < L.n; i++) if (!strncmp(L.issue[i].text, "Unusual origin", 14)) ow++;
    CHECK(ow >= 1, "origin game 0 raises an origin warning");

    PkMon m2 = party[0];
    m2.metLevel = 120; m2.level = 100;
    PkLegality L2 = pk_check_legality(&m2);
    int ml = 0; for (int i = 0; i < L2.n; i++) if (!strncmp(L2.issue[i].text, "Met level above 100", 19)) ml++;
    printf("(4) origin0 warns=%d, metLevel120 illegal-hit=%d\n", ow, ml);
    CHECK(ml >= 1 && !L2.ok, "met level above 100 is flagged illegal");

    PkMon m3 = party[0];
    m3.metGame = 3; m3.metLocation = 0xE0;     /* dead-zone (0xD6..0xFC) on a non-Colosseum mon */
    PkLegality L3 = pk_check_legality(&m3);
    int lw = 0; for (int i = 0; i < L3.n; i++) if (!strncmp(L3.issue[i].text, "Invalid met location", 20)) lw++;
    m3.metLocation = 0xFF;                       /* fateful special must NOT warn */
    PkLegality L4 = pk_check_legality(&m3);
    int lw2 = 0; for (int i = 0; i < L4.n; i++) if (!strncmp(L4.issue[i].text, "Invalid met location", 20)) lw2++;
    printf("(5) deadzone-loc warns=%d, fateful-loc warns=%d (want 1, 0)\n", lw, lw2);
    CHECK(lw >= 1, "a dead-zone met location warns");
    CHECK(lw2 == 0, "the fateful special met location does not warn");
  }

  printf("\n%s: %d failure(s)\n", g_fail ? "FAIL" : "OK", g_fail);
  return g_fail ? 1 : 0;
}

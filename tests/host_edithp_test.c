/* Host test: BACKLOG #231 -- edits never revive a fainted mon, and current HP is an
 * editable field (party only). Pure gen3_edit.c.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_edithp_test.c \
 *      source/gen3_save.c source/gen3_mon.c source/gen3_box.c source/gen3_edit.c \
 *      source/gen3_daycare.c source/gen3_clip.c source/data_tables.c \
 *      -o /tmp/hedithp && /tmp/hedithp
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "gen3_edit.h"
#include "data_tables.h"

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)

static uint16_t hp_cur(const EditMon* e) { return (uint16_t)(e->raw[0x56] | (e->raw[0x57] << 8)); }
static uint16_t hp_max(const EditMon* e) { return (uint16_t)(e->raw[0x58] | (e->raw[0x59] << 8)); }

/* a party EditMon of `species` at level 50, straight from a box build (fresh -> full HP). */
static void mk(EditMon* e, uint16_t species) {
  uint8_t box[80];
  gen3_build_mon(species, 50, 0x10203040u, 0x33334444u, "GUY", 3, box);
  gen3_edit_load(box, false, e);
  em_set_party_flag(e, true);
}

int main(void) {
  EditMon e;
  printf("host_edithp_test\n");

  mk(&e, 384);
  CHECK(hp_cur(&e) == hp_max(&e) && hp_max(&e) > 1, "A0 fresh box->party mon starts at full HP");

  /* A: fainted stays fainted through every recompute-triggering edit */
  em_set_curhp(&e, 0);
  em_set_iv(&e, PK_HP, 31);            CHECK(hp_cur(&e) == 0, "A1 fainted stays 0 through IV edit");
  em_set_ev(&e, PK_HP, 200);           CHECK(hp_cur(&e) == 0, "A2 fainted stays 0 through EV edit");
  em_set_level(&e, 80);                CHECK(hp_cur(&e) == 0, "A3 fainted stays 0 through level edit");
  em_set_species(&e, 6);               CHECK(hp_cur(&e) == 0, "A4 fainted stays 0 through species edit");
  em_set_pid(&e, 0x55667788u);         CHECK(hp_cur(&e) == 0, "A5 fainted stays 0 through PID edit");
  em_set_ivword(&e, 0x1234567u);       CHECK(hp_cur(&e) == 0, "A6 fainted stays 0 through IV-word edit");
  CHECK(hp_max(&e) > 1, "A7 max HP still recomputed for a fainted mon");

  /* B: healthy mon keeps its HP (not reset to max), clamped when max shrinks */
  mk(&e, 384);
  uint16_t m0 = hp_max(&e);
  em_set_curhp(&e, (uint16_t)(m0 - 10));
  em_set_ev(&e, PK_ATK, 100);          CHECK(hp_cur(&e) == m0 - 10, "B1 damaged HP preserved through unrelated stat edit");
  em_set_level(&e, 5);
  CHECK(hp_max(&e) < m0, "B2 max shrank");
  CHECK(hp_cur(&e) == hp_max(&e), "B3 cur clamped to the smaller max");
  em_set_level(&e, 50);
  CHECK(hp_cur(&e) <= hp_max(&e) && hp_cur(&e) != hp_max(&e) , "B4 growing max does not top HP up");

  /* C: Shedinja (internal 303) */
  mk(&e, SPECIES_SHEDINJA);
  CHECK(hp_max(&e) == 1 && hp_cur(&e) == 1, "C0 fresh Shedinja 1/1");
  em_set_curhp(&e, 0);
  em_set_level(&e, 60);                CHECK(hp_cur(&e) == 0 && hp_max(&e) == 1, "C1 fainted Shedinja stays 0");
  em_set_iv(&e, PK_ATK, 3);            CHECK(hp_cur(&e) == 0, "C2 fainted Shedinja stays 0 (IV)");
  em_set_curhp(&e, 1);
  em_set_level(&e, 61);                CHECK(hp_cur(&e) == 1, "C3 healthy Shedinja stays 1");
  mk(&e, 384); em_set_curhp(&e, 5); em_set_species(&e, SPECIES_SHEDINJA);
  CHECK(hp_max(&e) == 1 && hp_cur(&e) == 1, "C4 healthy mon -> Shedinja pins to 1");
  mk(&e, 384); em_set_curhp(&e, 0); em_set_species(&e, SPECIES_SHEDINJA);
  CHECK(hp_cur(&e) == 0, "C5 fainted mon -> Shedinja stays 0");

  /* D: the field */
  mk(&e, 384);
  uint16_t mx = hp_max(&e);
  em_set_curhp(&e, 7);                 CHECK(hp_cur(&e) == 7 && em_get_curhp(&e) == 7, "D1 writes what it says");
  em_set_curhp(&e, 0);                 CHECK(hp_cur(&e) == 0, "D2 0 allowed (lowering incl. faint)");
  em_set_curhp(&e, 3);                 CHECK(hp_cur(&e) == 3, "D3 raising above 0 revives");
  em_set_curhp(&e, 60000);             CHECK(hp_cur(&e) == mx, "D4 clamps to max HP");
  em_set_curhp(&e, 0);
  em_set_iv(&e, PK_DEF, 9); em_set_ev(&e, PK_SPE, 9); em_set_level(&e, 77); em_set_species(&e, 384);
  CHECK(hp_cur(&e) == 0, "D5 no other field's recompute revives after a 0 write");

  /* E: box record has no current HP */
  uint8_t box[80];
  gen3_build_mon(384, 50, 0x10203040u, 0x33334444u, "GUY", 3, box);
  gen3_edit_load(box, false, &e);
  em_set_curhp(&e, 5);
  CHECK(em_get_curhp(&e) == 0xFFFFu, "E1 box getter reports none");
  uint8_t out[100]; gen3_edit_commit(&e, out);
  CHECK(memcmp(out, box, 80) == 0, "E2 box setter is a no-op (record byte-identical)");

  /* F: commit round-trip carries the HP */
  mk(&e, 384); em_set_curhp(&e, 11);
  gen3_edit_commit(&e, out);
  EditMon e2; gen3_edit_load(out, true, &e2);
  CHECK(hp_cur(&e2) == 11, "F1 cur HP survives commit/load");

  printf("%s (%d failed)\n", g_fail ? "FAIL" : "ok", g_fail);
  return g_fail ? 1 : 0;
}

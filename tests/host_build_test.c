/* Host test for gen3_build_mon (create-a-mon): the built record must decode as a real
 * present mon (species set, not egg/bad-egg), carry the right exp for its level, and
 * pass the lossless edit round-trip (the pre-write safety gate). */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "gen3_edit.h"
#include "gen3_mon.h"
#include "data_tables.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (sp=%u lvl=%u)\n", msg, sp, lvl); fails++; } } while (0)

int main(void) {
  for (uint16_t sp = 1; sp <= 411; sp += 29) {
    for (uint8_t lvl = 1; lvl <= 100; lvl = (uint8_t)(lvl + 33)) {
      uint8_t rec[80];
      gen3_build_mon(sp, lvl, 0xDEADBEEFu, 0x12345678u, "ASH", 3, rec);
      PkMon m;
      bool ok = pk_decode_mon(rec, false, &m);
      CHECK(ok, "decodes as present");
      CHECK(m.species == sp, "species matches");
      CHECK(!m.isEgg && !m.isBadEgg, "not egg/bad-egg");
      CHECK(m.heldItem == 0, "no held item");
      CHECK(m.friendship == 70, "friendship 70");
      CHECK(m.moves[0] == 33, "placeholder move");
      uint8_t gr = pk_species_growth(sp);
      CHECK(pk_level_from_exp(gr, m.experience) == lvl, "level<->exp consistent");
      CHECK(gen3_edit_roundtrip_ok(rec, false), "lossless round-trip");
    }
  }
  printf("host_build_test: %s\n", fails ? "FAILURES" : "ALL PASS");
  return fails ? 1 : 0;
}

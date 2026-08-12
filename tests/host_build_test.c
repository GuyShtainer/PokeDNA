/* Host test for gen3_build_mon (create-a-mon): the built record must decode as a real
 * present mon (species set, not egg/bad-egg), carry the right exp for its level, and
 * pass the lossless edit round-trip (the pre-write safety gate).
 *
 *   cc -std=c11 -O2 -I source tests/host_build_test.c source/gen3_edit.c \
 *      source/gen3_mon.c source/gen3_save.c source/data_tables.c source/learnsets2.c \
 *      source/evolutions.c source/gen3_daycare.c -o /tmp/hbt && /tmp/hbt
 *
 * THIS TEST WAS DEAD AND WRONG, and that pairing is the point. It had no cc line, so the
 * runner SKIPped it — and while it was skipped, gen3_build_mon lost its hard-coded Tackle
 * placeholder (species now get real moves from their own learnset), so its
 * `moves[0] == 33` assertion had quietly become false. Nobody noticed, because a skipped
 * test cannot fail. A test that is not RUN is not coverage, however carefully written; it
 * is a comment that looks like coverage, which is worse than nothing.
 *
 * It is kept rather than deleted because it covers something host_legalbuild_test.c does
 * not: that one builds every species at its OWN floor (gen3_build_level), so it never
 * exercises an arbitrary level. This sweeps levels 1/34/67/100 across the species axis and
 * checks the exp<->level maths and the round-trip hold there too. */
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
      /* The Tackle placeholder is gone: move 1 now comes from the species' own learnset.
       * Assert the PROPERTY (it has a move, and that move is real) rather than a literal
       * id, so this cannot rot into a false claim the next time the source changes. */
      CHECK(m.moves[0] != 0, "move 1 is set");
      CHECK(m.moves[0] < 355, "move 1 is a real move id");
      uint8_t gr = pk_species_growth(sp);
      CHECK(pk_level_from_exp(gr, m.experience) == lvl, "level<->exp consistent");
      CHECK(gen3_edit_roundtrip_ok(rec, false), "lossless round-trip");
    }
  }
  printf("host_build_test: %s\n", fails ? "FAILURES" : "ALL PASS");
  return fails ? 1 : 0;
}

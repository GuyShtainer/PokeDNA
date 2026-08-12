/* Host test: A BUILD WITHOUT THE EVOLUTION TABLE MUST FAIL OPEN, NOT PASS BY SILENCE.
 *
 *   cc -std=c11 -O2 -I source tests/host_evo_nodata_test.c source/gen3_legality_hooks.c \
 *      source/gen3_legality2.c source/learnsets2.c source/encounters.c source/statics.c \
 *      source/gen3_pidiv.c source/gen3_edit.c source/gen3_mon.c source/gen3_save.c \
 *      source/gen3_daycare.c source/data_tables.c -o /tmp/hen && /tmp/hen
 *
 * NOTE WHAT IS *NOT* ON THAT LINE: source/evolutions.c. That is the entire point, and
 * it is why this is a separate file from tests/host_evolutions_test.c — the property
 * "the checker goes quiet AND says so when the table is missing" cannot be observed
 * from a binary that links the table in. Without evolutions.c the weak fallbacks in
 * source/evolutions.h take over, exactly as they would in a clone of this repo that
 * never ran tools/gen_evolutions.py (the generated table is git-ignored, so that clone
 * is the DEFAULT state of a fresh checkout, not an exotic one).
 *
 * The failure this guards against is specific and quiet: if pk_evo_floor's fallback
 * returned 1 ("no evolution constrains this") instead of PK_EVO_NO_DATA, a table-less
 * build would grade every level-5 Charizard LEGAL with a clean banner and no hint that
 * a whole check family never ran. A missing table must produce NO VERDICT — the same
 * tri-state discipline encounters.h and lg2_have_data() already enforce
 * (OVERNIGHT-DECISIONS.md §2).
 *
 * Part (E) covers the same table's OTHER consumer, added when the create flow stopped
 * hard-coding level 5: gen3_build_level() asks pk_evo_floor what level a new Pokemon
 * should start at, so it inherits the identical obligation — with no table it must
 * answer 5 (the old behaviour), never a number derived from the -1 sentinel.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "evolutions.h"
#include "gen3_edit.h"
#include "gen3_mon.h"
#include "gen3_legality2.h"
#include "gen3_legality_hooks.h"
#include "data_tables.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c, msg) do { g_checks++; if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)

#define SP_CHARIZARD 6

int main(void) {
  printf("(A) the table really is absent\n");
  CHECK(!pk_evo_have_data(), "pk_evo_have_data() is false without evolutions.c");
  CHECK(pk_evo_count() == 0, "no links");
  CHECK(pk_evo_row(0) == 0, "no rows");
  CHECK(pk_evo_list(SP_CHARIZARD, 0) == 0, "no forward index");
  CHECK(!pk2_evolution_data_ok(), "the hook's own data gate agrees");

  printf("(B) every answer is NO_DATA, never a permissive default\n");
  /* The dangerous fallback would be 1 — "nothing constrains this species" — because it
   * is indistinguishable from the truthful answer for a base form. */
  CHECK(pk_evo_min_level(SP_CHARIZARD) == PK_EVO_NO_DATA, "min level is NO_DATA, not 1");
  CHECK(pk_evo_floor(SP_CHARIZARD) == PK_EVO_NO_DATA, "checker floor is NO_DATA, not 1");
  CHECK(pk_evo_wild_min(SP_CHARIZARD) == PK_EVO_NO_DATA, "wild min is NO_DATA, not 0");
  CHECK(pk2_evo_floor(SP_CHARIZARD) == PK_EVO_NO_DATA, "the hooks wrapper passes it through");

  printf("(C) the method helpers still answer, because they describe the enum\n");
  /* These two are facts about EVO_* ids, not about data, so a missing table has nothing
   * to be wrong about — and the hook's floor walk in the generated build depends on the
   * same classification, so pinning it here catches a divergence between the two copies. */
  CHECK(pk_evo_method_is_level(PK_EVO_LEVEL), "EVO_LEVEL is level-gated");
  CHECK(pk_evo_method_is_level(PK_EVO_LEVEL_SHEDINJA), "EVO_LEVEL_SHEDINJA is level-gated");
  CHECK(!pk_evo_method_is_level(PK_EVO_FRIENDSHIP), "FRIENDSHIP is not");
  CHECK(!pk_evo_method_is_level(PK_EVO_TRADE), "TRADE is not");
  CHECK(!pk_evo_method_is_level(PK_EVO_ITEM), "ITEM is not");
  CHECK(!pk_evo_method_is_level(PK_EVO_BEAUTY), "BEAUTY is not");
  CHECK(strcmp(pk_evo_method_name(PK_EVO_LEVEL), "LEVEL") == 0, "names still resolve");

  printf("(D) the catalogue reports the family as ABSENT, and shows no clean bill\n");
  uint8_t rec[80];
  gen3_build_mon(SP_CHARIZARD, 5, 0x1234ABCDu, 0x00010002u, "GUY", 3, rec);
  EditMon e;
  gen3_edit_load(rec, false, &e);
  em_set_metloc(&e, 0x11);       /* a real place, so no exemption can be blamed */
  em_set_metlevel(&e, 5);
  gen3_edit_commit(&e, rec);
  PkMon m;
  memset(&m, 0, sizeof m);
  pk_decode_mon(rec, false, &m);

  Pk2Report R;
  pk_check_legality2(&m, &R);
  CHECK((R.hooks_absent & PK2_HOOK_EVO) != 0,
        "PK2_HOOK_EVO is set: the UI can say 'not checked' instead of 'clean'");
  int rows = 0;
  for (int i = 0; i < R.n; i++)
    if (strncmp(R.row[i].text, "Evolves at L", 12) == 0) rows++;
  CHECK(rows == 0, "and the hook invented no verdict it has no data for");
  printf("     L5 %s -> %s, hooks_absent=0x%02X, %d evolution row(s)\n",
         pk_species_name(SP_CHARIZARD), pk2_grade_name(R.grade), R.hooks_absent, rows);

  /* The other three families ARE linked here, so their bits must stay clear — without
   * this, a build error that dropped every hook would make part (D) pass for the wrong
   * reason. */
  CHECK((R.hooks_absent & (PK2_HOOK_MOVES | PK2_HOOK_ENCOUNTER)) == 0,
        "only the evolution family is missing (the others are linked in)");

  printf("(E) the BUILDER fails open too: no table -> the old level 5, never a guess\n");
  /* gen3_build_level() reads the same pk_evo_floor the checker does, so a table-less
   * build must fall back to 5 for EVERY species — including the ones that would be
   * raised in a generated build. The failure this guards against is the mirror of part
   * (B)'s: a builder that treated PK_EVO_NO_DATA (-1) as a number would produce level
   * -1 -> 255 -> a clamp, i.e. a Charizard at some level nobody chose. */
  CHECK(gen3_build_level(SP_CHARIZARD) == 5, "CHARIZARD falls back to L5 (floor is NO_DATA)");
  CHECK(gen3_build_level(1) == 5, "...and so does a base form");
  CHECK(gen3_build_level(0) == 5, "...and species 0");
  CHECK(gen3_build_level(9999) == 5, "...and an out-of-range species");
  int raised = 0;
  for (uint16_t sp = 1; sp <= 411; sp++)
    if (pk_national_no(sp) && gen3_build_level(sp) != 5) raised++;
  CHECK(raised == 0, "NOT ONE species is moved off L5 without the table");

  /* And the record really carries L5 — a box mon stores exp, not a level, so a builder
   * that clamped a bad floor would show up here as the wrong exp. */
  uint8_t r0[80];
  gen3_build_mon(SP_CHARIZARD, 0 /* "you pick" */, 0x1234ABCDu, 0x00010002u, "GUY", 3, r0);
  PkMon m0;
  memset(&m0, 0, sizeof m0);
  pk_decode_mon(r0, false, &m0);
  CHECK(m0.experience == pk_exp_for_level(pk_species_growth(SP_CHARIZARD), 5),
        "a 'you pick' build carries the level-5 exp when there is no table to pick from");

  printf("host_evo_nodata_test: %d checks, %d failures\n", g_checks, g_fail);
  printf("host_evo_nodata_test: %s\n", g_fail ? "FAILURES" : "ALL PASS");
  return g_fail ? 1 : 0;
}

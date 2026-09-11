/* Host test for source/dex_cell_art_rule.{c,h} (BACKLOG #124) -- the pure-C selection
 * rule pdna_gbdex.c's gbdex_cell_art() callback gates on before it ever attempts a GB
 * ROM fetch. Pure C, no GBA/tonc headers, links only this one module.
 *
 *   cc -std=c11 -O2 -Wall -Wextra -I source tests/host_dexcellart_test.c \
 *      source/dex_cell_art_rule.c -o /tmp/hdca && /tmp/hdca
 *
 * Covers the WHOLE 2x2x2 truth table (8 cases): GB wins only when both gb_session AND
 * gb_have are true, regardless of store_ok; otherwise the answer is STORE iff store_ok,
 * else NONE. Also checks the real caller's actual usage shape (store_ok always true,
 * Gen-1/Gen-3 sessions never reach DEX_CELL_ART_GB) and that the function is a pure,
 * repeatable mapping (same inputs -> same answer, called twice).
 */
#include <stdio.h>
#include <string.h>

#include "dex_cell_art_rule.h"

static int checks = 0, fails = 0;
#define CHECK(c, msg) do { checks++; if (!(c)) { printf("  !! FAIL: %s\n", msg); fails++; } } while (0)

static const char* name_of(DexCellArtSource s) {
  switch (s) {
    case DEX_CELL_ART_GB:    return "GB";
    case DEX_CELL_ART_STORE: return "STORE";
    case DEX_CELL_ART_NONE:  return "NONE";
    default:                 return "?";
  }
}

/* (A) The whole 2x2x2 truth table, one line per combination -- a mutation that drops
 * ANY one of the three inputs' effect fails at least one of these 8 checks. */
static void test_truth_table(void) {
  struct { int gb_session, gb_have, store_ok; DexCellArtSource want; } cases[] = {
    /* gb_session gb_have store_ok  -> want */
    { 0, 0, 0,  DEX_CELL_ART_NONE  },
    { 0, 0, 1,  DEX_CELL_ART_STORE },
    { 0, 1, 0,  DEX_CELL_ART_NONE  },   /* gb_have alone (no session) never wins        */
    { 0, 1, 1,  DEX_CELL_ART_STORE },
    { 1, 0, 0,  DEX_CELL_ART_NONE  },   /* gb_session alone (no ROM) never wins         */
    { 1, 0, 1,  DEX_CELL_ART_STORE },
    { 1, 1, 0,  DEX_CELL_ART_GB    },   /* GB wins even when the store could not (would
                                         * not be asked -- the caller never has an
                                         * unservable store in practice, but the rule
                                         * itself must not depend on store_ok to award
                                         * GB) */
    { 1, 1, 1,  DEX_CELL_ART_GB    },
  };
  for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    DexCellArtSource got = dex_cell_art_source((bool)cases[i].gb_session,
                                               (bool)cases[i].gb_have,
                                               (bool)cases[i].store_ok);
    char msg[96];
    snprintf(msg, sizeof msg, "case %u: gb_session=%d gb_have=%d store_ok=%d -> want %s got %s",
             i, cases[i].gb_session, cases[i].gb_have, cases[i].store_ok,
             name_of(cases[i].want), name_of(got));
    CHECK(got == cases[i].want, msg);
  }
  printf("(A) truth table ok\n");
}

/* (B) The real caller's shape: gbdex_cell_art() always passes store_ok=true (pdna_
 * pick.c's dex_cell_grid() always has its own fallback ready) -- so from that
 * caller's actual call site, the ONLY two reachable answers are GB and STORE, never
 * NONE. A Gen-1 or Gen-3 session (gb_session=false, since gbdex_cell_art computes it
 * from `s->gen == GB_GEN2`) must never reach DEX_CELL_ART_GB regardless of gb_have. */
static void test_real_caller_shape(void) {
  CHECK(dex_cell_art_source(true, true, true) == DEX_CELL_ART_GB,
        "GB session + ROM registered -> GB (the only case pdna_gbdex.c installs the override for)");
  CHECK(dex_cell_art_source(true, false, true) == DEX_CELL_ART_STORE,
        "GB session, no ROM registered -> falls back to STORE, never NONE (store_ok always true here)");
  CHECK(dex_cell_art_source(false, true, true) == DEX_CELL_ART_STORE,
        "Gen-1/Gen-3 session (gb_session false by construction) -> STORE even if a Gen-2 ROM "
        "happens to be registered -- gbdex_cell_art's own s->gen check is what makes gb_session "
        "false for Gen 1, never gb_have");
  printf("(B) real-caller shape ok\n");
}

/* (C) Pure function: same inputs, same answer, called twice, no state carried between
 * calls (a stray file-static in a future edit would still pass this by luck, but a
 * genuinely impure implementation that e.g. alternated answers would not). */
static void test_pure_repeatable(void) {
  DexCellArtSource a1 = dex_cell_art_source(true, true, true);
  DexCellArtSource a2 = dex_cell_art_source(true, true, true);
  CHECK(a1 == a2, "same inputs called twice give the same answer");
  DexCellArtSource b1 = dex_cell_art_source(false, false, false);
  DexCellArtSource a3 = dex_cell_art_source(true, true, true);
  CHECK(a3 == a1, "an unrelated call in between does not change a repeat's answer");
  (void)b1;
  printf("(C) pure/repeatable ok\n");
}

int main(void) {
  test_truth_table();
  test_real_caller_shape();
  test_pure_repeatable();

  printf("\n%d checks, %d FAILED\n", checks, fails);
  return fails ? 1 : 0;
}

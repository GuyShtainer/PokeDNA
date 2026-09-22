/* Host test for source/dex_cell_art_rule.{c,h} (BACKLOG #124, extended #196) -- the
 * pure-C selection rule pdna_gbdex.c's gbdex_cell_art() callback gates on before it
 * ever attempts a GB ROM fetch. Pure C, no GBA/tonc headers, links only this one
 * module.
 *
 *   cc -std=c11 -O2 -Wall -Wextra -I source tests/host_dexcellart_test.c \
 *      source/dex_cell_art_rule.c -o /tmp/hdca && /tmp/hdca
 *
 * BACKLOG #196: `gb_session` (bool) became `session_gen` (0 / GB_GEN1=1 / GB_GEN2=2)
 * so Gen 1 gets its own GB rung alongside Gen 2's. Covers the whole 3x2x2 truth
 * table (session_gen in {0,1,2} x gb_have x store_ok, 12 cases): GB wins only when
 * session_gen is 1 or 2 AND gb_have is true, regardless of store_ok; otherwise the
 * answer is STORE iff store_ok, else NONE. Also checks the real caller's actual
 * usage shape (store_ok always true) and that the function is a pure, repeatable
 * mapping (same inputs -> same answer, called twice).
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

/* (A) The whole 3x2x2 truth table, one line per combination -- a mutation that drops
 * the gen check (e.g. treats ANY nonzero session_gen the same as "is 1 or 2", which
 * it already is here, OR drops the check entirely and always tries GB) or the have
 * check fails at least one of these. session_gen values other than 0/1/2 are also
 * covered (case 12: a garbage value must behave exactly like "no session"). */
static void test_truth_table(void) {
  struct { int session_gen, gb_have, store_ok; DexCellArtSource want; } cases[] = {
    /* session_gen gb_have store_ok  -> want */
    { 0, 0, 0,  DEX_CELL_ART_NONE  },
    { 0, 0, 1,  DEX_CELL_ART_STORE },
    { 0, 1, 0,  DEX_CELL_ART_NONE  },   /* gb_have alone (no session) never wins        */
    { 0, 1, 1,  DEX_CELL_ART_STORE },
    { 1, 0, 0,  DEX_CELL_ART_NONE  },   /* GB_GEN1 session, no ROM -> never wins        */
    { 1, 0, 1,  DEX_CELL_ART_STORE },
    { 1, 1, 0,  DEX_CELL_ART_GB    },   /* GB_GEN1 session + have -> GB, even when the
                                         * store could not (would not be asked in
                                         * practice -- the rule itself must not
                                         * depend on store_ok to award GB) */
    { 1, 1, 1,  DEX_CELL_ART_GB    },
    { 2, 0, 0,  DEX_CELL_ART_NONE  },   /* GB_GEN2 session, no ROM -> never wins        */
    { 2, 0, 1,  DEX_CELL_ART_STORE },
    { 2, 1, 0,  DEX_CELL_ART_GB    },
    { 2, 1, 1,  DEX_CELL_ART_GB    },
  };
  for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    DexCellArtSource got = dex_cell_art_source(cases[i].session_gen,
                                               (bool)cases[i].gb_have,
                                               (bool)cases[i].store_ok);
    char msg[112];
    snprintf(msg, sizeof msg, "case %u: session_gen=%d gb_have=%d store_ok=%d -> want %s got %s",
             i, cases[i].session_gen, cases[i].gb_have, cases[i].store_ok,
             name_of(cases[i].want), name_of(got));
    CHECK(got == cases[i].want, msg);
  }
  /* case 12: a garbage session_gen (neither 0, 1 nor 2) must behave like "no
   * session" -- the caller only ever passes 0/1/2, but the rule is defensive. */
  CHECK(dex_cell_art_source(3, true, true) == DEX_CELL_ART_STORE,
        "case 12: session_gen=3 (garbage) + gb_have -> STORE, never GB");
  CHECK(dex_cell_art_source(-1, true, true) == DEX_CELL_ART_STORE,
        "case 13: session_gen=-1 (garbage) + gb_have -> STORE, never GB");
  printf("(A) truth table ok\n");
}

/* (B) The real caller's shape: gbdex_cell_art() always passes store_ok=true (pdna_
 * pick.c's dex_cell_grid() always has its own fallback ready) -- so from that
 * caller's actual call site, the ONLY two reachable answers are GB and STORE, never
 * NONE. A Gen-3 session (session_gen=0) must never reach DEX_CELL_ART_GB regardless
 * of gb_have -- and neither generation's session may be served by the OTHER
 * generation's `gb_have` (that is the caller's own job to keep straight -- this test
 * pins that a Gen-1 session with gb_have=true still resolves to GB, i.e. the rule
 * itself does not distinguish WHICH generation gb_have describes, so the caller
 * computing the WRONG per-generation have() would slip through this rule
 * undetected -- see tests/host_gborigin*_test.c / pdna_gbdex.c's own comment for
 * where THAT guarantee actually lives: in gbdex_serves_dex() always asking
 * have(session's own gen), never have(the other one) or have(either)). */
static void test_real_caller_shape(void) {
  CHECK(dex_cell_art_source(1, true, true) == DEX_CELL_ART_GB,
        "GB_GEN1 session + ROM registered -> GB");
  CHECK(dex_cell_art_source(2, true, true) == DEX_CELL_ART_GB,
        "GB_GEN2 session + ROM registered -> GB");
  CHECK(dex_cell_art_source(1, false, true) == DEX_CELL_ART_STORE,
        "GB_GEN1 session, no ROM registered -> falls back to STORE, never NONE");
  CHECK(dex_cell_art_source(0, true, true) == DEX_CELL_ART_STORE,
        "Gen-3 session (session_gen=0 by construction) -> STORE even if some GB ROM "
        "happens to be registered -- gbdex_cell_art's own s->gen check is what makes "
        "session_gen 0 for a non-GB visit, never gb_have");
  printf("(B) real-caller shape ok\n");
}

/* (D) Review A5 cross-review finding: the PAGE-level rule (dex_cell_art_serves_page,
 * what dex_declare_page()'s icon-store-plan skip and the bob-animation loop gate on)
 * pinned side by side with the per-cell rule above, over the same {session_gen,
 * gb_have} space -- must always agree with dex_cell_art_source()'s own GB/not-GB
 * split when store_ok is held true (the real caller's shape), and in particular:
 * a Gen-1 session (session_gen=1) whose OWN generation has no ROM but SOME OTHER
 * generation's `gb_have` was (wrongly) passed in -- the exact class of bug this rule
 * exists to prevent a repeat of -- must still answer honestly off whatever gb_have
 * it was actually given (the rule cannot see the caller's mistake; the caller-side
 * discipline is pinned elsewhere, see (B) above). */
static void test_page_rule(void) {
  struct { int session_gen, gb_have; bool want_serves; } cases[] = {
    /* session_gen gb_have -> want_serves_page */
    { 0, 0, false },
    { 0, 1, false },   /* a Gen-3 session, some GB ROM registered anyway            */
    { 1, 0, false },
    { 1, 1, true  },
    { 2, 0, false },
    { 2, 1, true  },
  };
  for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    bool got = dex_cell_art_serves_page(cases[i].session_gen, (bool)cases[i].gb_have);
    char msg[96];
    snprintf(msg, sizeof msg, "page case %u: session_gen=%d gb_have=%d -> want serves_page=%d got %d",
             i, cases[i].session_gen, cases[i].gb_have, cases[i].want_serves, got);
    CHECK(got == cases[i].want_serves, msg);

    /* Consistency with the per-cell rule (store_ok held true, the real caller's own
     * shape): serves_page must be true iff dex_cell_art_source() picks GB. */
    DexCellArtSource cell = dex_cell_art_source(cases[i].session_gen, (bool)cases[i].gb_have, true);
    char msg2[128];
    snprintf(msg2, sizeof msg2,
             "page case %u: serves_page=%d must agree with per-cell source=%s (store_ok=true)",
             i, got, name_of(cell));
    CHECK((cell == DEX_CELL_ART_GB) == got, msg2);
  }
  /* The named bug case, spelled out once more, unmissably (BACKLOG #124 review A5): */
  CHECK(dex_cell_art_serves_page(0, true) == false,
        "Gen-3/no-session + a GB ROM registered anyway -> serves_page FALSE");
  CHECK(dex_cell_art_serves_page(1, false) == false,
        "GB_GEN1 session + its OWN generation not registered -> serves_page FALSE "
        "(BACKLOG #196: the Gen-1 twin of review A5's bug case)");
  printf("(D) page rule ok\n");
}

/* (C) Pure function: same inputs, same answer, called twice, no state carried between
 * calls (a stray file-static in a future edit would still pass this by luck, but a
 * genuinely impure implementation that e.g. alternated answers would not). */
static void test_pure_repeatable(void) {
  DexCellArtSource a1 = dex_cell_art_source(2, true, true);
  DexCellArtSource a2 = dex_cell_art_source(2, true, true);
  CHECK(a1 == a2, "same inputs called twice give the same answer");
  DexCellArtSource b1 = dex_cell_art_source(0, false, false);
  DexCellArtSource a3 = dex_cell_art_source(2, true, true);
  CHECK(a3 == a1, "an unrelated call in between does not change a repeat's answer");
  (void)b1;
  printf("(C) pure/repeatable ok\n");
}

int main(void) {
  test_truth_table();
  test_real_caller_shape();
  test_page_rule();
  test_pure_repeatable();

  printf("\n%d checks, %d FAILED\n", checks, fails);
  return fails ? 1 : 0;
}

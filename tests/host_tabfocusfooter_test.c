/* Host test for source/tab_focus_footer.c — BACKLOG #173 F2's pure decision for
 * the box grid's tab-focus footer line, factored out of pdna_box.c's draw_footer()
 * (the `s_tab_focus >= 0` branch) so the round-1 defect (tested the DISPLAYED
 * screen's is_bank instead of the carried item's origin) has a host-side pin.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_tabfocusfooter_test.c \
 *      source/tab_focus_footer.c -o /tmp/htff && /tmp/htff
 *
 * pdna_box.c's draw_footer() calls this exact function -- there is no second
 * copy of the decision to drift out of sync with what ships; only the session
 * state (s_tab_focus, s_holding, pdna_box_carry_is_gb()) stays in pdna_box.c,
 * which is GBA UI and not host-compilable. */
#include <stdio.h>
#include <string.h>

#include "tab_focus_footer.h"
#include "pdna_layout.h"

static int fails = 0, checks = 0;

static void expect_str(const char* what, const char* got, const char* want) {
  checks++;
  if (strcmp(got, want) != 0) {
    fails++;
    printf("  FAIL %-62s got=\"%s\" want=\"%s\"\n", what, got, want);
  } else {
    printf("  ok   %-62s \"%s\"\n", what, got);
  }
}

int main(void) {
  /* Not holding anything: always the plain pick footer, regardless of origin. */
  expect_str("not holding, carry_is_gb=false",
             tab_focus_footer(false, false), "L/R tab  A pick  DN");
  expect_str("not holding, carry_is_gb=true (moot -- nothing held)",
             tab_focus_footer(false, true), "L/R tab  A pick  DN");

  /* Holding a GB-origin carry: the carry-aware footer, whatever screen is displayed
   * (the round-1 bug: the old condition looked at the DISPLAYED screen's is_bank,
   * not this carry_is_gb origin flag -- that distinction is exactly what this test
   * pins). */
  expect_str("holding, carry_is_gb=true",
             tab_focus_footer(true, true), PDNA_TAB_FOCUS_CARRY_FOOTER);

  /* Holding a non-GB (PC/Bank) origin carry: the plain pick footer. */
  expect_str("holding, carry_is_gb=false",
             tab_focus_footer(true, false), "L/R tab  A pick  DN");

  printf("\n%d/%d checks passed\n", checks - fails, checks);
  return fails ? 1 : 0;
}

/* Host test for source/flags_fold.c — BACKLOG #2a's pure row-visibility math for
 * the Gen-3 data editor's collapsible FLAGS list (section titles fold/unfold on A,
 * every session starts fully collapsed, cursor movement skips hidden rows).
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_flagsfold_test.c \
 *      source/flags_fold.c -o /tmp/hff && /tmp/hff
 *
 * pdna_main.c's nf_cache/nf_hdr_ord/nf_visible/nf_step call these exact
 * functions (ff_build_ord/ff_hdr_ord/ff_row_visible/ff_step) -- there is no
 * second copy of this logic to drift out of sync with what ships; only the
 * session state (which table is cached, the fold bitmask) stays in pdna_main.c,
 * which is GBA UI and not host-compilable. */
#include <stdio.h>
#include <string.h>

#include "flags_fold.h"

static int fails = 0, checks = 0;

static void expect_int(const char* what, int got, int want) {
  checks++;
  if (got != want) { fails++; printf("  FAIL %-62s got=%d want=%d\n", what, got, want); }
  else               printf("  ok   %-62s %d\n", what, got);
}

static void expect_true(const char* what, int cond) {
  checks++;
  if (!cond) { fails++; printf("  FAIL %-62s\n", what); }
  else         printf("  ok   %-62s\n", what);
}

/* A small synthetic table shaped like the real per-game ones: a header, some
 * rows, another header, some rows, ... 3 groups, 9 rows + 3 headers = 12 total
 * (indices 0..11), plus the trailing raw-browser row at index 12 (nc). */
static const NamedFlag kTable[] = {
  { NAMED_FLAG_HEADER, "Badges" },       /* 0  group 0 */
  { 0x100, "Stone Badge" },              /* 1 */
  { 0x101, "Knuckle Badge" },            /* 2 */
  { NAMED_FLAG_HEADER, "System" },       /* 3  group 1 */
  { 0x200, "Pokedex obtained" },         /* 4 */
  { 0x201, "National Dex" },             /* 5 */
  { 0x202, "Game cleared (HoF)" },       /* 6 */
  { NAMED_FLAG_HEADER, "Elite Four (reset @HoF)" },  /* 7  group 2 */
  { 0x202, "League beaten (HoF)" },      /* 8  (same flag as row 6, different label -- fine, ff_* never dedupes by num) */
  { 0x300, "E4 Sidney" },                /* 9 */
  { 0x301, "E4 Phoebe" },                /* 10 */
  { 0x302, "E4 Glacia" },                /* 11 */
};
#define NC ((int)(sizeof(kTable) / sizeof(kTable[0])))   /* 12 */
#define TOTAL (NC + 1)                                    /* + trailing raw-browser row */

static void test_build_ord(void) {
  printf("== ff_build_ord: header ordinals over 3 groups ==\n");
  uint8_t ord[NC];
  ff_build_ord(kTable, NC, ord, NC);
  static const int want[NC] = { 0,0,0, 1,1,1,1, 2,2,2,2,2 };
  int all_ok = 1;
  for (int i = 0; i < NC; i++) if (ord[i] != want[i]) all_ok = 0;
  expect_true("every row's ordinal matches its owning header (0,0,0,1,1,1,1,2,2,2,2,2)", all_ok);

  /* ord_cap smaller than nc: only writes up to ord_cap, never past the array
   * the caller actually handed over (the overrun this guards against would be
   * a real GBA memory-safety bug, not just a wrong answer). */
  uint8_t small[4]; memset(small, 0xAA, sizeof small);
  ff_build_ord(kTable, NC, small, 3);
  expect_true("ord_cap < nc: writes stop exactly at ord_cap",
              small[0] == 0 && small[1] == 0 && small[2] == 0 && small[3] == 0xAA);

  /* Defensive no-ops: must not crash, must not touch the buffer. */
  uint8_t untouched[4] = { 9, 9, 9, 9 };
  ff_build_ord(NULL, NC, untouched, 4);
  ff_build_ord(kTable, NC, NULL, 4);
  ff_build_ord(kTable, NC, untouched, 0);
  expect_true("NULL nf / NULL ord / ord_cap<=0 are no-ops (buffer untouched)",
              untouched[0] == 9 && untouched[1] == 9 && untouched[2] == 9 && untouched[3] == 9);
}

static void test_row_visible_all_expanded(void) {
  printf("\n== ff_row_visible: folded==0 (every group expanded) ==\n");
  uint8_t ord[NC]; ff_build_ord(kTable, NC, ord, NC);
  int all_visible = 1;
  for (int r = 0; r < TOTAL; r++)
    if (!ff_row_visible(kTable, NC, ord, NC, 0u, r)) all_visible = 0;
  expect_true("folded==0: every row, including the trailing raw-browser one, is visible", all_visible);
}

static void test_row_visible_all_collapsed(void) {
  printf("\n== ff_row_visible: folded==~0 (every session's OWN starting state) ==\n");
  uint8_t ord[NC]; ff_build_ord(kTable, NC, ord, NC);
  uint32_t folded = 0xFFFFFFFFu;   /* pdna_main.c's own s_flags_folded initializer */

  /* Headers are ALWAYS visible even fully collapsed -- otherwise there would
   * be no row left to press A on to unfold them again. */
  expect_true("header row 0 (\"Badges\") visible while collapsed", ff_row_visible(kTable, NC, ord, NC, folded, 0));
  expect_true("header row 3 (\"System\") visible while collapsed", ff_row_visible(kTable, NC, ord, NC, folded, 3));
  expect_true("header row 7 (\"Elite Four...\") visible while collapsed", ff_row_visible(kTable, NC, ord, NC, folded, 7));
  /* The trailing raw-flag-browser row is not inside any group. */
  expect_true("trailing raw-browser row (index nc) visible while collapsed", ff_row_visible(kTable, NC, ord, NC, folded, NC));

  /* Every NAMED row is hidden. */
  int any_named_visible = 0;
  static const int named_rows[] = { 1, 2, 4, 5, 6, 8, 9, 10, 11 };
  for (size_t i = 0; i < sizeof(named_rows) / sizeof(named_rows[0]); i++)
    if (ff_row_visible(kTable, NC, ord, NC, folded, named_rows[i])) any_named_visible = 1;
  expect_true("every named flag row is hidden when its group is collapsed", !any_named_visible);
}

static void test_row_visible_one_group_open(void) {
  printf("\n== ff_row_visible: only group 1 (\"System\") open ==\n");
  uint8_t ord[NC]; ff_build_ord(kTable, NC, ord, NC);
  uint32_t folded = 0xFFFFFFFFu & ~(1u << 1);   /* clear bit 1: group 1 open, 0 and 2 stay folded */

  expect_true("group 0 row (Stone Badge) still hidden", !ff_row_visible(kTable, NC, ord, NC, folded, 1));
  expect_true("group 1 row (Pokedex obtained) now visible", ff_row_visible(kTable, NC, ord, NC, folded, 4));
  expect_true("group 1 row (Game cleared (HoF)) now visible", ff_row_visible(kTable, NC, ord, NC, folded, 6));
  expect_true("group 2 row (E4 Sidney) still hidden", !ff_row_visible(kTable, NC, ord, NC, folded, 9));
  expect_true("every header stays visible regardless of which groups are open",
              ff_row_visible(kTable, NC, ord, NC, folded, 0) &&
              ff_row_visible(kTable, NC, ord, NC, folded, 3) &&
              ff_row_visible(kTable, NC, ord, NC, folded, 7));
}

static void test_step_skips_hidden(void) {
  printf("\n== ff_step: cursor movement skips folded rows entirely (all collapsed) ==\n");
  uint8_t ord[NC]; ff_build_ord(kTable, NC, ord, NC);
  uint32_t folded = 0xFFFFFFFFu;

  /* From row 0 ("Badges"), stepping DOWN must land on row 3 ("System") next --
   * rows 1 and 2 (Stone/Knuckle Badge) are hidden and must be skipped
   * entirely, not merely un-selectable. */
  int r = ff_step(kTable, NC, ord, NC, folded, TOTAL, 0, +1);
  expect_int("down from \"Badges\" (0) skips 1,2 and lands on \"System\" (3)", r, 3);

  r = ff_step(kTable, NC, ord, NC, folded, TOTAL, r, +1);
  expect_int("down again lands on \"Elite Four...\" (7), skipping 4,5,6", r, 7);

  r = ff_step(kTable, NC, ord, NC, folded, TOTAL, r, +1);
  expect_int("down again lands on the trailing raw-browser row (nc==12)", r, NC);

  r = ff_step(kTable, NC, ord, NC, folded, TOTAL, r, +1);
  expect_int("down from the last row is a bottom stop (unchanged)", r, NC);

  /* And back up, symmetrically. */
  r = ff_step(kTable, NC, ord, NC, folded, TOTAL, NC, -1);
  expect_int("up from the trailing row lands on \"Elite Four...\" (7)", r, 7);
  r = ff_step(kTable, NC, ord, NC, folded, TOTAL, r, -1);
  expect_int("up again lands on \"System\" (3)", r, 3);
  r = ff_step(kTable, NC, ord, NC, folded, TOTAL, r, -1);
  expect_int("up again lands on \"Badges\" (0)", r, 0);
  r = ff_step(kTable, NC, ord, NC, folded, TOTAL, r, -1);
  expect_int("up from the first row is a top stop (unchanged)", r, 0);
}

static void test_step_one_group_open(void) {
  printf("\n== ff_step: only \"System\" open -- steps through its 3 named rows too ==\n");
  uint8_t ord[NC]; ff_build_ord(kTable, NC, ord, NC);
  uint32_t folded = 0xFFFFFFFFu & ~(1u << 1);

  int r = 3;   /* start on "System" */
  r = ff_step(kTable, NC, ord, NC, folded, TOTAL, r, +1);
  expect_int("down from \"System\" (3) lands on \"Pokedex obtained\" (4)", r, 4);
  r = ff_step(kTable, NC, ord, NC, folded, TOTAL, r, +1);
  expect_int("down again lands on \"National Dex\" (5)", r, 5);
  r = ff_step(kTable, NC, ord, NC, folded, TOTAL, r, +1);
  expect_int("down again lands on \"Game cleared (HoF)\" (6)", r, 6);
  r = ff_step(kTable, NC, ord, NC, folded, TOTAL, r, +1);
  expect_int("down again skips into the still-folded next group's header (7)", r, 7);
}

static void test_dir_zero_and_defensive(void) {
  printf("\n== ff_step/ff_hdr_ord/ff_row_visible: defensive edges ==\n");
  uint8_t ord[NC]; ff_build_ord(kTable, NC, ord, NC);
  expect_int("dir==0 returns r unchanged", ff_step(kTable, NC, ord, NC, 0u, TOTAL, 5, 0), 5);
  expect_int("ff_hdr_ord out of range reads as group 0", ff_hdr_ord(ord, NC, -1), 0);
  expect_int("ff_hdr_ord out of range (>= ord_cap) reads as group 0", ff_hdr_ord(ord, NC, NC + 100), 0);
  expect_true("ff_row_visible with r == nc (trailing row) is visible even with NULL ord",
              ff_row_visible(kTable, NC, NULL, 0, 0xFFFFFFFFu, NC));
}

/* Exercise a table shaped like the real generated ones (BACKLOG #17's SIZE RULE
 * comment: Emerald sits at ~530 rows once "Fly destinations" landed) to prove
 * the clamp at FF_MAX_GROUPS groups degrades safely rather than corrupting
 * anything -- the 33rd header's rows fold under group 31 instead of a wraparound
 * or an out-of-bounds shift, matching pdna_main.c's own documented tradeoff. */
static void test_many_groups_clamped(void) {
  printf("\n== ff_build_ord: > FF_MAX_GROUPS headers clamp, they do not wrap or corrupt ==\n");
  static NamedFlag big[40 * 3];
  int n = 0;
  for (int g = 0; g < 40; g++) {
    big[n].num = NAMED_FLAG_HEADER; big[n].name = "H"; n++;
    big[n].num = (uint16_t)(0x10 + g); big[n].name = "row-a"; n++;
    big[n].num = (uint16_t)(0x50 + g); big[n].name = "row-b"; n++;
  }
  uint8_t ord[40 * 3];
  ff_build_ord(big, n, ord, n);
  expect_int("group 31 (the clamp ceiling) owns everything from header 31 onward",
             ord[31 * 3], FF_MAX_GROUPS - 1);
  expect_int("the LAST header's rows also clamp to group 31, not 39 or wrapped to 7",
             ord[n - 1], FF_MAX_GROUPS - 1);
  /* Folding bit 31 must hide every row from header 31 onward, including the
   * very last row of the very last (header 39) group -- the "far ceiling"
   * pdna_main.c's own comment promises, not a silent corruption. */
  uint32_t fold_bit31 = 1u << (FF_MAX_GROUPS - 1);
  expect_true("folding group 31 hides the last row of header 39's group too",
              !ff_row_visible(big, n, ord, n, fold_bit31, n - 1));
  expect_true("folding group 31 leaves header 0's row visible (unrelated group)",
              ff_row_visible(big, n, ord, n, fold_bit31, 1));
}

int main(void) {
  test_build_ord();
  test_row_visible_all_expanded();
  test_row_visible_all_collapsed();
  test_row_visible_one_group_open();
  test_step_skips_hidden();
  test_step_one_group_open();
  test_dir_zero_and_defensive();
  test_many_groups_clamped();

  printf("\n%d/%d checks passed\n", checks - fails, checks);
  return fails ? 1 : 0;
}

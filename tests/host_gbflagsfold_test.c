/* flags_fold.h (BACKLOG #2a, host-tested standalone by tests/host_flagsfold_test.c)
 * reused over the GB FLAGS-tab table (BACKLOG #88) -- under test here.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gbflagsfold_test.c \
 *      source/flags_fold.c source/gb_fields.c source/gb_flags.c -o /tmp/hgbff && /tmp/hgbff
 *
 * GbFlagRow (gb_flags.h: {uint16_t index; const char* label; uint8_t kind;}) is NOT
 * layout-identical to NamedFlag (gen3_flags.h: {uint16_t num; const char* name;}) --
 * it carries one more field (kind) -- so pdna_gbflags.c does not reinterpret-cast
 * between them (that would rely on common-initial-sequence aliasing across unrelated
 * struct types, undefined outside a union). Instead it copies gbfl_row_at()'s rows
 * into a small NamedFlag-shaped array once per screen entry; this file proves that
 * conversion preserves flags_fold.h's own header/fold/step contract exactly the way
 * tests/host_flagsfold_test.c already proved it over Gen 3's synthetic fixture --
 * same GBFL_HEADER/NAMED_FLAG_HEADER sentinel VALUE (0xFFFF), so a row copied
 * faithfully folds/unfolds/steps identically regardless of which table it came from.
 */
#include <stdio.h>
#include <string.h>

#include "flags_fold.h"
#include "gb_flags.h"

static int fails = 0, checks = 0;

static void expect_true(const char* what, int cond) {
  checks++;
  if (!cond) { fails++; printf("  FAIL %-70s\n", what); }
  else         printf("  ok   %-70s\n", what);
}

#define ROW_CAP 64

/* The exact conversion pdna_gbflags.c performs: GbFlagRow -> NamedFlag, one field
 * assignment each, no cast. GBFL_HEADER and NAMED_FLAG_HEADER are asserted equal
 * below (both 0xFFFF) so a header row's sentinel survives the copy unchanged. */
static int build_named(GbGame g, NamedFlag* out, int cap) {
  int n = gbfl_row_count(g);
  if (n > cap) n = cap;
  int k = 0;
  for (int i = 0; i < n; i++) {
    GbFlagRow row;
    if (!gbfl_row_at(g, i, &row)) break;
    out[k].num = row.index;
    out[k].name = row.label;
    k++;
  }
  return k;
}

static void check_game(const char* name, GbGame g) {
  printf("== %s ==\n", name);
  static NamedFlag nf[ROW_CAP];
  int nc_rows_incl_headers = build_named(g, nf, ROW_CAP);
  expect_true("gbfl_row_count is non-empty", nc_rows_incl_headers > 0);
  if (nc_rows_incl_headers <= 0) return;

  /* The trailing raw-browser row this screen appends is EXTRA past the table --
   * flags_fold.h's own `nc` parameter is the table size, `total = nc + 1`. */
  int nc = nc_rows_incl_headers;
  int total = nc + 1;

  uint8_t ord[ROW_CAP];
  ff_build_ord(nf, nc, ord, ROW_CAP);

  /* Every header row is its OWN group's ordinal; group ordinals are non-decreasing
   * and start at 0 (ff_build_ord's own contract, verified over the synthetic Gen-3
   * fixture in host_flagsfold_test.c -- same invariant, a REAL table this time). */
  int last_ord = -1, headers = 0, monotone = 1;
  for (int r = 0; r < nc; r++) {
    if (nf[r].num == NAMED_FLAG_HEADER) {
      headers++;
      int this_ord = ff_hdr_ord(ord, ROW_CAP, r);
      if (this_ord != last_ord + 1) monotone = 0;
      last_ord = this_ord;
    }
  }
  expect_true("every table starts with at least one header", headers > 0);
  expect_true("header ordinals are 0,1,2,... in table order (monotone)", monotone);

  /* folded==0: every row (including the trailing raw-browser one) is visible. */
  int all_visible = 1;
  for (int r = 0; r < total; r++)
    if (!ff_row_visible(nf, nc, ord, ROW_CAP, 0u, r)) all_visible = 0;
  expect_true("folded==0: every row is visible", all_visible);

  /* folded==~0 (every session's own starting state, pdna_main.c's s_flags_folded
   * initializer): only header rows + the trailing raw-browser row survive. */
  uint32_t folded_all = 0xFFFFFFFFu;
  int collapsed_ok = 1;
  for (int r = 0; r < nc; r++) {
    bool vis = ff_row_visible(nf, nc, ord, ROW_CAP, folded_all, r);
    bool should = (nf[r].num == NAMED_FLAG_HEADER);
    if (vis != should) collapsed_ok = 0;
  }
  expect_true("folded==~0: only header rows are visible (every flag hidden)", collapsed_ok);
  expect_true("folded==~0: the trailing raw-browser row is still visible",
              ff_row_visible(nf, nc, ord, ROW_CAP, folded_all, nc));

  /* ff_step over the fully-collapsed table must land ONLY on header rows / the
   * trailing row -- stepping through the whole table and back must return to start. */
  int r = 0, steps = 0, bad_land = 0;
  int first = r;
  do {
    int nr = ff_step(nf, nc, ord, ROW_CAP, folded_all, total, r, +1);
    if (nr == r) break;   /* stop: no further visible row */
    if (!(nf[nr].num == NAMED_FLAG_HEADER) && nr != nc) bad_land = 1;
    r = nr;
    steps++;
  } while (r != first && steps < total + 2);
  expect_true("ff_step (folded) never lands on a hidden flag row", !bad_land);
  expect_true("ff_step (folded) terminates within the table size", steps <= total);

  /* Unfold group 0 only: its member rows become visible, every other group's rows
   * (besides their own header) stay hidden. */
  uint32_t folded_g0_open = folded_all & ~1u;
  int g0_ok = 1;
  for (int rr = 0; rr < nc; rr++) {
    bool vis = ff_row_visible(nf, nc, ord, ROW_CAP, folded_g0_open, rr);
    bool is_header = (nf[rr].num == NAMED_FLAG_HEADER);
    bool in_g0 = (ff_hdr_ord(ord, ROW_CAP, rr) == 0);
    bool should = is_header || in_g0;
    if (vis != should) g0_ok = 0;
  }
  expect_true("unfolding group 0 reveals exactly its own member rows", g0_ok);
}

int main(void) {
  expect_true("GBFL_HEADER == NAMED_FLAG_HEADER (same sentinel value, 0xFFFF)",
              GBFL_HEADER == NAMED_FLAG_HEADER);

  check_game("Red",     GBF_G_RED);
  check_game("Yellow",  GBF_G_YELLOW);
  check_game("Gold/Silver", GBF_G_GS);
  check_game("Crystal", GBF_G_CRYSTAL);

  printf("\n%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}

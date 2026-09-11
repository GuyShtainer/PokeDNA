/* source/pdna_gbflags.c's nf_draw_row() row formatting -- under test (BACKLOG #88 D3
 * review). Independently re-derives the EXACT siprintf format strings nf_draw_row()
 * uses (member row: "%-16s %-3s %s" name/ON-off/kind-suffix; header row: "%c %s"
 * fold-glyph/title) over every generated row (source/gb_flags.c's gbfl_row_count/
 * gbfl_row_at, one table per game) and asserts every formatted row -- BEFORE any
 * ui_truncate() clamp -- already fits the 29-column budget nf_draw_row()'s own
 * ui_truncate(..., 29) calls clamp to, so truncation is dead code on every row this
 * generator ever emits (not just today's four games' current tables). Also proves
 * `char row[64]` (D1's fix for the smaller `char row[40]` that a 45-char formatted
 * row overflowed) comfortably holds the longest row this test finds.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gbflagsrow_test.c \
 *      source/gb_fields.c source/gb_flags.c -o /tmp/hgbfrow && /tmp/hgbfrow
 *
 * Missing generated tables (a clone that never ran tools/gen_gbfields.py) SKIP rather
 * than fail -- gbfl_row_count() reports 0 rows for every game on the weak fallback,
 * so the loop below simply has nothing to check.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "gb_flags.h"

static int g_fail = 0, g_check = 0;
#define CHECK(c, ...) do { g_check++; if (!(c)) { \
    printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

#define ROW_BUDGET 29
#define ROW_BUF 64   /* mirrors nf_draw_row()'s `char row[64]` (D1) */

static const char* kind_suffix(uint8_t kind) {
  switch (kind) {
    case GBFL_KIND_BAG_GRANT: return "(bag)";
    case GBFL_KIND_READONLY:  return "(sty)";
    case GBFL_KIND_WARN:      return "(!)";
    default: return "";
  }
}

static const char* GAME_NAME[GBF_G_COUNT] = { "RED", "YELLOW", "GS", "CRYSTAL" };

int main(void) {
  int total_rows = 0;
  size_t longest = 0;
  const char* longest_row_desc = "(none)";

  for (int g = 0; g < GBF_G_COUNT; g++) {
    int n = gbfl_row_count((GbGame)g);
    if (n == 0) {
      printf("  SKIP %-10s no generated rows (tools/gen_gbfields.py not run)\n", GAME_NAME[g]);
      continue;
    }
    for (int i = 0; i < n; i++) {
      GbFlagRow row;
      bool ok = gbfl_row_at((GbGame)g, i, &row);
      CHECK(ok, "%s row %d: gbfl_row_at() reports out-of-range for a row inside [0, count)", GAME_NAME[g], i);
      if (!ok) continue;
      total_rows++;

      char buf[ROW_BUF];
      if (row.index == GBFL_HEADER) {
        /* Both fold states -- the glyph itself never changes the length. */
        int n1 = snprintf(buf, sizeof buf, "%c %s", '+', row.label);
        CHECK(n1 >= 0 && (size_t)n1 < sizeof buf, "%s header %d %s: formatted row does not fit char row[64]", GAME_NAME[g], i, row.label);
        CHECK((size_t)n1 <= ROW_BUDGET, "%s header %d '%s': %d chars, over the %d-column budget", GAME_NAME[g], i, row.label, n1, ROW_BUDGET);
        if ((size_t)n1 > longest) { longest = (size_t)n1; longest_row_desc = row.label; }
      } else {
        for (int state = 0; state < 2; state++) {
          const char* onoff = state ? "ON" : "off";
          int n1 = snprintf(buf, sizeof buf, "%-16s %-3s %s", row.label, onoff, kind_suffix(row.kind));
          CHECK(n1 >= 0 && (size_t)n1 < sizeof buf, "%s row %d %s (%s): formatted row does not fit char row[64]", GAME_NAME[g], i, row.label, onoff);
          CHECK((size_t)n1 <= ROW_BUDGET, "%s row %d '%s' (%s): %d chars, over the %d-column budget", GAME_NAME[g], i, row.label, onoff, n1, ROW_BUDGET);
          if ((size_t)n1 > longest) { longest = (size_t)n1; longest_row_desc = row.label; }
        }
      }
    }
  }

  if (total_rows > 0) {
    printf("  ok   %d row(s) across %d game(s), longest formatted row %zu chars ('%s'), "
           "row[64] margin %zu bytes\n", total_rows, GBF_G_COUNT, longest, longest_row_desc,
           ROW_BUF - 1 - longest);
    CHECK(longest < ROW_BUF, "longest formatted row (%zu) must fit char row[%d]", longest, ROW_BUF);
    CHECK(longest <= ROW_BUDGET, "longest formatted row (%zu) must be within the %d-column budget", longest, ROW_BUDGET);
  } else {
    printf("SKIP host_gbflagsrow_test: no game reports generated rows\n");
  }

  printf("host_gbflagsrow_test: %d checks, %d failed\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}

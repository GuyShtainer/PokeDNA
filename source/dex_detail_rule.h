#ifndef DEX_DETAIL_RULE_H
#define DEX_DETAIL_RULE_H
/*
 * dex_detail_rule -- BACKLOG #203's pure logic for the Pokedex DETAIL view: which entry
 * L/R lands on, the status word, and the "No. NNN" line. Header-only static inline, no
 * tonc/GBA headers, so tests/host_dexdetail_test.c compiles the exact code the screen
 * runs (source/pdna_pick.c's dex_detail()).
 *
 * The list the detail walks is the grid's own g_list (INTERNAL species ids, in the
 * current filtered/sorted order); `n` is its length and indices are positions in it,
 * never species numbers. Numbering for display goes through pk_national_no() at the
 * caller -- this file never sees a species id.
 */
#include <stdint.h>

/* One L/R step in a list of `n` entries: dir < 0 previous, dir > 0 next, wrapping at
 * both ends (the grid's own UP/DOWN wrap the same way). An empty list, or an index that
 * is already out of range, answers 0 -- never a value outside 0..n-1 for n > 0. */
static inline int dex_detail_step(int idx, int n, int dir) {
  if (n <= 0) return 0;
  if (idx < 0 || idx >= n) return 0;
  if (dir < 0) return (idx == 0) ? n - 1 : idx - 1;
  if (dir > 0) return (idx == n - 1) ? 0 : idx + 1;
  return idx;
}

/* State 0/1/2 (unseen/seen/caught) -> the word the status line prints. Anything else
 * reads as unseen. */
static inline const char* dex_detail_status_word(int st) {
  return st == 2 ? "CAUGHT" : st == 1 ? "SEEN" : "--";
}

/* The A-press cycle: unseen -> seen -> caught -> unseen. */
static inline int dex_detail_cycle(int st) {
  return (st >= 0 && st < 2) ? st + 1 : 0;
}

#endif /* DEX_DETAIL_RULE_H */

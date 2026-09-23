/* Host test: BACKLOG #249 cases B/C/D/E -- source/item_map_g1g2.c's item_g2_to_g1()
 * (Gen-2 -> Gen-1 item id, by NAME EQUALITY, never a hand-copied table) and
 * source/gen3_to_gb.c's g3gb_item_ladder() (the pure BAG/PC/STAYS decision).
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_itemladder_test.c \
 *      source/item_map_g1g2.c source/item_map_g2g3.c source/gb_item_names.c \
 *      source/gb_bag.c source/gb_fields.c source/gb_session.c source/gen1_save.c \
 *      source/gen1_write.c source/gen2_save.c source/gen2_write.c source/gb_edit.c \
 *      source/gen3_to_gb.c source/gb_sidecar.c source/bank_cell.c source/gen3_save.c \
 *      source/gen3_mon.c source/gen3_box.c source/gen3_edit.c source/gen3_daycare.c \
 *      source/data_tables.c source/evolutions.c source/gb_moves_legal.c \
 *      -o /tmp/hitemladder && /tmp/hitemladder
 *
 * No corpus needed -- every case here is a pure decision over the two shipped item
 * name tables and small integer pocket counts, no .sav file involved. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "item_map_g1g2.h"
#include "gb_item_names.h"
#include "gb_bag.h"
#include "gen3_to_gb.h"
#include "gb_edit.h"   /* GB_GEN1/GB_GEN2 */

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { \
    g_check++; \
    if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
  } while (0)

/* ---- item_g2_to_g1: known real pairs, an ambiguous real pair, and a full
 * independent re-derivation over the whole id space -------------------------- */

static void test_known_pairs(void) {
  printf("-- 1. item_g2_to_g1: known real pairs --\n");
  /* RARE CANDY: Gen-2 0x20 <-> Gen-1 0x28, unique both ways (gb_item_names.c). */
  CHECK(item_g2_to_g1(0x20) == 0x28, "RARE CANDY: g2 0x20 -> g1 0x28 (got 0x%02X)",
        item_g2_to_g1(0x20));
  /* MOON STONE: Gen-2 0x08 <-> Gen-1 0x0A. */
  CHECK(item_g2_to_g1(0x08) == 0x0A, "MOON STONE: g2 0x08 -> g1 0x0A (got 0x%02X)",
        item_g2_to_g1(0x08));
  /* POTION: Gen-2 0x12 <-> Gen-1 0x14. */
  CHECK(item_g2_to_g1(0x12) == 0x14, "POTION: g2 0x12 -> g1 0x14 (got 0x%02X)",
        item_g2_to_g1(0x12));
  /* id 0 always maps to 0. */
  CHECK(item_g2_to_g1(0) == 0, "id 0 -> 0");
  /* PP UP: Gen-2 0x3E spells "PP UP" -- but Gen 1's OWN table has TWO entries
   * spelled "PP UP" (0x32 and 0x4F, a real quirk of gb_item_names.c's own data,
   * not a test fixture) -- the forward match is ambiguous, so this refuses rather
   * than guessing which of the two Gen-1 ids is "the" PP Up. */
  CHECK(item_g2_to_g1(0x3E) == 0, "PP UP: ambiguous forward match (two Gen-1 ids) -> 0 (got 0x%02X)",
        item_g2_to_g1(0x3E));
}

static bool name_eq(const char* a, const char* b) {
  return a && b && a[0] && b[0] && strcmp(a, b) == 0;
}

/* Independent re-derivation: for every Gen-2 id, redo the SAME name-equality search
 * with a separately-written double loop (not calling item_g2_to_g1's own internals)
 * and assert the two agree -- this is what "the table can never drift from the
 * names" means when there is no checked-in table at all, only two live functions
 * that both have to agree with the name tables right now. */
static void test_full_reexpansion(void) {
  printf("-- 2. item_g2_to_g1: independent re-derivation over the whole id space --\n");
  int agree = 0, disagree = 0;
  for (int g2 = 0; g2 <= 255; g2++) {
    const char* g2name = gb2_item_name((uint8_t)g2);
    uint8_t expect = 0;
    if (g2name && g2name[0]) {
      int g1_match = 0, g1_count = 0;
      for (int g1 = 1; g1 <= 255; g1++) {
        if (gbb_is_g1_key_item((uint8_t)g1)) continue;
        const char* g1name = gb1_item_name((uint8_t)g1);
        if (name_eq(g1name, g2name)) { g1_match = g1; g1_count++; }
      }
      if (g1_count == 1) {
        int g2_count = 0;
        for (int g2b = 1; g2b <= 255; g2b++)
          if (name_eq(gb2_item_name((uint8_t)g2b), g2name)) g2_count++;
        if (g2_count == 1) expect = (uint8_t)g1_match;
      }
    }
    uint8_t got = item_g2_to_g1((uint8_t)g2);
    if (got == expect) agree++; else { disagree++;
      printf("  !! FAIL: g2 id %d ('%s'): item_g2_to_g1=0x%02X, independent re-derivation=0x%02X\n",
             g2, g2name ? g2name : "(null)", got, expect);
    }
  }
  g_check++;
  if (disagree != 0) g_fail++;
  printf("  %d ids agree, %d disagree\n", agree, disagree);
}

static void test_key_items_never_returned(void) {
  printf("-- 3. item_g2_to_g1: never returns a Gen-1 key item --\n");
  int checked = 0;
  for (int g2 = 1; g2 <= 0xBE; g2++) {
    uint8_t g1 = item_g2_to_g1((uint8_t)g2);
    if (g1 == 0) continue;
    checked++;
    CHECK(!gbb_is_g1_key_item(g1), "g2 id %d maps to g1 id %u, which IS a key item", g2, g1);
  }
  printf("  %d non-zero mappings checked\n", checked);
}

/* ---- g3gb_item_ladder: the pure B/C/D/E decision --------------------------- */

static void test_ladder(void) {
  printf("-- 4. g3gb_item_ladder: cases B/C/D/E as a pure decision --\n");
  uint8_t g1item = 0xFF;

  /* No item at all -> NONE, regardless of destination or bag state. */
  CHECK(g3gb_item_ladder(GB_GEN1, 0, 0, 20, 0, 50, &g1item) == G3GB_ITEM_NONE, "no item -> NONE");
  CHECK(g1item == 0, "no item -> g1_item_out cleared to 0 (got %u)", g1item);

  /* dst_gen != GB_GEN1 always answers STAYS for a real item (Gen-2's own A/E ladder
   * is decided inside gen3_to_gb_fixed, not here). */
  g1item = 0xFF;
  CHECK(g3gb_item_ladder(GB_GEN2, 1, 0, 20, 0, 50, &g1item) == G3GB_ITEM_STAYS,
        "dst_gen GEN2 -> STAYS (not this function's job)");
  CHECK(g1item == 0, "GEN2 path -> g1_item_out cleared");

  /* case E: item id 5 has no Gen-2 counterpart at all (kG3ToG2[5]==0). */
  g1item = 0xFF;
  CHECK(g3gb_item_ladder(GB_GEN1, 5, 0, 20, 0, 50, &g1item) == G3GB_ITEM_STAYS,
        "g3 item 5 (no g2 counterpart) -> STAYS (case E)");
  CHECK(g1item == 0, "case E -> g1_item_out cleared");

  /* case E via ambiguity: g3 item 1 maps to g2 id 1 (MASTER BALL, per
   * item_map_g2g3.c's generated table); MASTER BALL is unambiguous both ways, so
   * pick a g3 item whose g2 target IS ambiguous instead. Gen-3 item ids that map to
   * g2 0x3E (PP UP, proven ambiguous above) would also refuse -- but no Gen-3 id maps
   * to 0x3E in the shipped table (checked via kG3ToG2 not containing 0x3E for any
   * entry that matters here), so this case is instead proven directly on
   * item_g2_to_g1(0x3E)==0 in test_known_pairs -- g3gb_item_ladder's own ambiguity
   * handling is exactly "call item_g2_to_g1 and trust its 0", already covered. */

  /* case B: g3 item 1 -> g2 1 -> g1 1 (MASTER BALL, unique both ways -- verified by
   * test_known_pairs's sibling assertions on RARE CANDY/MOON STONE/POTION; MASTER
   * BALL is the same shape). Items pocket has room. */
  g1item = 0xFF;
  uint8_t g1_master = item_g2_to_g1(1);
  CHECK(g1_master != 0, "sanity: MASTER BALL (g2 id 1) has a Gen-1 counterpart (got 0x%02X)", g1_master);
  if (g1_master != 0) {
    CHECK(g3gb_item_ladder(GB_GEN1, 1, 3, 20, 0, 50, &g1item) == G3GB_ITEM_BAG,
          "items_count 3 < cap 20 -> BAG (case B)");
    CHECK(g1item == g1_master, "case B -> g1_item_out == %u (got %u)", g1_master, g1item);

    /* case C: Items pocket full, Item PC has room. */
    g1item = 0xFF;
    CHECK(g3gb_item_ladder(GB_GEN1, 1, 20, 20, 10, 50, &g1item) == G3GB_ITEM_PC,
          "items_count 20 == cap, pc 10 < cap 50 -> PC (case C)");
    CHECK(g1item == g1_master, "case C -> g1_item_out still set (got %u)", g1item);

    /* case D: both full -> falls through to STAYS (E), but g1_item_out is still the
     * would-be id (the caller decides what, if anything, to say about it). */
    g1item = 0xFF;
    CHECK(g3gb_item_ladder(GB_GEN1, 1, 20, 20, 50, 50, &g1item) == G3GB_ITEM_STAYS,
          "both pockets full -> STAYS (case D falls to E)");
    CHECK(g1item == g1_master, "case D -> g1_item_out still reports the id that would have travelled");
  }
}

int main(void) {
  test_known_pairs();
  test_full_reexpansion();
  test_key_items_never_returned();
  test_ladder();

  /* F6 (xfer-items fix pass review): mutation M5 ("drop the reverse g2_count
   * ambiguity check in item_g2_to_g1") used to be a printed claim here, not a test --
   * no REAL Gen-1/Gen-2 name pair triggers reverse ambiguity today, so a printf could
   * not prove anything without a SYNTHETIC name table, and a printf is not evidence
   * regardless. It is now a real, running test: tests/host_itemladder_m5_test.py
   * builds a scratch-aliased copy of gb_item_names.c and both a real and a mutant
   * item_map_g1g2.c against it, and asserts the real build refuses (0) while the
   * mutant wrongly accepts (1) -- wired into tests/run_host_tests.py's PY_TESTS. */

  printf(g_fail ? "FAIL: %d check(s), %d failure(s)\n" : "PASS: %d check(s), %d failure(s)\n",
         g_check, g_fail);
  return g_fail ? 1 : 0;
}

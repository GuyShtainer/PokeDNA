/* U5 fix pass (BACKLOG #67, review-opus ac9ffc0) -- pins the pure-logic half of
 * source/pdna_gbpack.c's Gen-2 Pack painter on the host.
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_gbpack_test.c \
 *      source/gb_bag.c source/gb_fields.c source/gb_session.c source/gb_edit.c \
 *      source/gen1_save.c source/gen1_write.c source/gen2_save.c source/gen2_write.c \
 *      source/data_tables.c source/gen3_to_gb.c source/gb_sidecar.c source/gen3_edit.c \
 *      source/gen3_mon.c source/gen3_box.c source/gen3_save.c source/gen3_daycare.c \
 *      -o /tmp/hgbpack && /tmp/hgbpack
 *
 * WHY A COPY, NOT A LINK. pdna_gbpack.c is NOT one of this codebase's pure-C cores --
 * it is a tonc/ui/snd/pdna_app GB-screen SHELL SCREEN (`#include <tonc.h>`, `ui.h`,
 * `snd.h`, `pdna_app.h`, `pdna_trainer.h`, `pdna_gen12.h` -- the last of which alone
 * pulls pdna_box.h -> pdna_app.h -> sprite_era.h/gen3_trainer.h, a transitive web with
 * no host-buildable leaf). tools/gb_oracle/README.md's own convention for testing a
 * screen like this is to extract the target static functions into a `body.inc` behind
 * minimal GbScreen/gbscr_cell()/gbscr_text() stand-ins rather than link the shipped
 * file. This test follows that shape but keeps the extract INLINE (no separate
 * generated file to go stale unnoticed): the five functions below (g2_tm_rebuild,
 * g2pack_row_total, g2pack_is_hm, g2pack_row_label, g2pack_paint_list,
 * gbpack_clamp_scroll) plus the constants/enum they need are a VERBATIM COPY of
 * source/pdna_gbpack.c as committed by this same slice (commit
 * "test(gen2-pack): the painter is pinned on the host; the swap harness runs on
 * Gold") -- kept in sync BY HAND, the same posture host_gbeditor_test.c/
 * host_gbreal_test.c/host_daycare_slot_test.c/host_dexicons_test.c already use
 * elsewhere in this suite for logic embedded in a tonc-dependent file. A silent drift
 * between this copy and the shipped file is a real gap this test cannot catch by
 * itself -- but pdna_gbpack.c's own line numbers are cited below so a future diff is
 * a straight `diff` command, not a re-read of the whole 500+-line file.
 *
 * `siprintf` (tonc's own printf-family fill-in) is `snprintf` on the host, matching
 * every other GB-screen-adjacent host test's own convention.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "gb_bag.h"
#include "pdna_gbscreen.h"   /* GbScreen, GbScrSrc, gbscr_cell()/gbscr_text() decls -- pure */

#define siprintf snprintf

/* ============================================================================
 * VERBATIM COPY of source/pdna_gbpack.c (this slice's own commit) -- see the
 * file header above for why this is a copy, not a link.
 * ============================================================================ */

/* pdna_gbpack.c:147-151 */
#define ROWS_VISIBLE 5
#define CURSOR_COL   7
#define NAME_COL     8
#define QTY_COL      17
#define TMNUM_COL    5

/* pdna_gbpack.c:153-154 */
static int name_row(int slot) { return 2 + 2 * slot; }
static int qty_row(int slot)  { return name_row(slot) + 1; }

/* pdna_gbpack.c:166-178 (D1 fix) */
enum { G2I_UL = 0, G2I_H = 1, G2I_UR = 2, G2I_V = 3, G2I_DL = 4, G2I_DR = 5 };

/* pdna_gbpack.c:67-102 (D4 fix) */
static uint8_t g2_tm_owned[GBB_TMHM_COUNT];
static int     g2_tm_n;

static void g2_tm_rebuild(const GbBag* bag) {
  g2_tm_n = 0;
  for (int i = 0; i < GBB_TMHM_COUNT; i++) {
    uint8_t c = 0;
    gbb_tmhm_get(bag, i, &c);
    if (c > 0) g2_tm_owned[g2_tm_n++] = (uint8_t)i;
  }
}

static int g2pack_row_total(const GbBag* bag, GbBagPocket pocket) {
  if (pocket == GBB_POCKET_TMHM) { g2_tm_rebuild(bag); return g2_tm_n + 1; }
  return bag->pockets[pocket].count + 1;
}

static bool g2pack_is_hm(int tmhm_index) { return tmhm_index >= 50; }

static void g2pack_row_label(const GbBag* bag, char* buf, int bufsz, GbBagPocket pocket, int idx) {
  (void)bufsz;
  if (pocket == GBB_POCKET_TMHM) {
    int real = (idx >= 0 && idx < g2_tm_n) ? g2_tm_owned[idx] : 0;
    int num = g2pack_is_hm(real) ? real - 50 + 1 : real + 1;
    siprintf(buf, bufsz, "%s%02u", g2pack_is_hm(real) ? "HM" : "TM", (unsigned)num);
  } else {
    siprintf(buf, bufsz, "ITEM-%u", (unsigned)bag->pockets[pocket].entries[idx].id);
  }
}

/* pdna_gbpack.c:246-247 */
static bool g2_pack_swap_active;
static int  g2_pack_swap_src;

/* pdna_gbpack.c:319-388 (D4/D5/D8 fix + the swap-mark glyph fix) */
static void g2pack_paint_list(GbScreen* gs, const GbBag* bag, GbBagPocket pocket,
                              int top, int sel) {
  int total = g2pack_row_total(bag, pocket);
  bool tmhm = (pocket == GBB_POCKET_TMHM);
  bool key  = (pocket == GBB_POCKET_KEY);
  char buf[16];
  for (int slot = 0; slot < ROWS_VISIBLE; slot++) {
    int idx = top + slot;
    int ny = name_row(slot), qy = qty_row(slot);
    bool has = idx < total;
    bool is_cancel = has && idx == total - 1;
    bool is_sel = has && idx == sel;
    bool is_swap_src = g2_pack_swap_active && has && !is_cancel &&
                       idx == g2_pack_swap_src && !is_sel;

    GbScrSrc mark_src = is_sel ? GBSCR_SRC_FONT : (is_swap_src ? GBSCR_SRC_FONT : GBSCR_SRC_BLANK);
    uint8_t  mark_tile = is_sel ? 0xED : (is_swap_src ? 0xEC : 0);
    gbscr_cell(gs, CURSOR_COL, ny, mark_src, mark_tile);

    int real = (tmhm && has && !is_cancel) ? g2_tm_owned[idx] : 0;

    if (tmhm && has && !is_cancel) {
      int num = g2pack_is_hm(real) ? real - 50 + 1 : real + 1;
      siprintf(buf, sizeof buf, "%02u", (unsigned)num);
      gbscr_text(gs, TMNUM_COL, ny, buf);
    } else {
      gbscr_cell(gs, TMNUM_COL, ny, GBSCR_SRC_BLANK, 0);
      gbscr_cell(gs, TMNUM_COL + 1, ny, GBSCR_SRC_BLANK, 0);
    }

    if (is_cancel) {
      siprintf(buf, sizeof buf, "CANCEL");
    } else if (has) {
      g2pack_row_label(bag, buf, sizeof buf, pocket, idx);
    } else {
      buf[0] = 0;
    }
    gbscr_text(gs, NAME_COL, ny, buf);
    for (int cx = NAME_COL + (int)strlen(buf); cx < QTY_COL; cx++)
      gbscr_cell(gs, cx, ny, GBSCR_SRC_BLANK, 0);

    bool has_qty = has && !is_cancel && !key && !(tmhm && g2pack_is_hm(real));
    if (has_qty) {
      gbscr_text(gs, QTY_COL, qy, "\xC3\x97");
      unsigned q = 0;
      if (tmhm) { uint8_t c = 0; gbb_tmhm_get(bag, real, &c); q = c; }
      else      q = bag->pockets[pocket].entries[idx].qty;
      if (q > 99u) siprintf(buf, sizeof buf, "**");
      else         siprintf(buf, sizeof buf, "%2u", q);
      gbscr_text(gs, QTY_COL + 1, qy, buf);
    } else {
      for (int cx = QTY_COL; cx <= QTY_COL + 2; cx++)
        gbscr_cell(gs, cx, qy, GBSCR_SRC_BLANK, 0);
    }
  }
}

/* pdna_gbpack.c:398-405 (D3 fix) */
static void gbpack_clamp_scroll(int total, int* sel, int* top) {
  if (total <= 0) { *sel = 0; *top = 0; return; }
  if (*sel >= total) *sel = total - 1;
  if (*sel < 0) *sel = 0;
  if (*sel < *top) *top = *sel;
  if (*sel >= *top + ROWS_VISIBLE) *top = *sel - (ROWS_VISIBLE - 1);
  if (*top < 0) *top = 0;
}

/* ============================================================================
 * Minimal GbScreen/gbscr_cell()/gbscr_text() RECORDING stand-ins (gb_oracle/
 * README.md's own harness shape) -- a 20x18 grid of (src, tile) pairs, plus a
 * raw-string overlay per gbscr_text() call so a test can just strcmp() a row.
 * `GbScreen` itself is the REAL struct (pdna_gbscreen.h, pure C, unused fields
 * left zeroed) -- only the two paint primitives are faked.
 * ============================================================================ */
#define GRID_W 20
#define GRID_H 18
static uint8_t g_src[GRID_H][GRID_W];
static uint8_t g_tile[GRID_H][GRID_W];
static char    g_text[GRID_H][GRID_W + 1];   /* raw gbscr_text() strings, NUL-padded */

static void grid_reset(void) {
  memset(g_src, 0, sizeof g_src);
  memset(g_tile, 0, sizeof g_tile);
  memset(g_text, 0, sizeof g_text);
}

void gbscr_cell(GbScreen* gs, int x, int y, GbScrSrc src, uint8_t tile) {
  (void)gs;
  if (x < 0 || x >= GRID_W || y < 0 || y >= GRID_H) return;
  g_src[y][x] = (uint8_t)src;
  g_tile[y][x] = tile;
  g_text[y][x] = 0;   /* a raw cell write clears any text that landed on this column */
}

void gbscr_text(GbScreen* gs, int x, int y, const char* s) {
  (void)gs;
  if (y < 0 || y >= GRID_H) return;
  for (int i = 0; s[i] && x + i < GRID_W; i++) {
    if (x + i < 0) continue;
    g_src[y][x + i] = GBSCR_SRC_FONT;
    g_text[y][x + i] = s[i];
  }
}

/* ============================================================================
 * tests
 * ============================================================================ */
static int g_fail = 0;
#define CHECK(cond, msg) do { \
  if (!(cond)) { printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); g_fail++; } \
} while (0)

/* D1: the description-box frame's tile indices are Gen 2's own frames-block
 * ids (0..5, frame 0), not Gen 1's borrowed textbox indices (25-31). */
static void test_d1_tile_indices(void) {
  printf("test_d1_tile_indices\n");
  CHECK(G2I_UL == 0, "G2I_UL");
  CHECK(G2I_H  == 1, "G2I_H");
  CHECK(G2I_UR == 2, "G2I_UR");
  CHECK(G2I_V  == 3, "G2I_V");
  CHECK(G2I_DL == 4, "G2I_DL");
  CHECK(G2I_DR == 5, "G2I_DR");
}

/* D3: gold_down4 ground truth -- the cursor walks all ROWS_VISIBLE (5) rows
 * before `top` moves, unlike Gen 1's own bag (pinned at slot ROWS_VISIBLE-2). */
static void test_d3_clamp_table(void) {
  printf("test_d3_clamp_table\n");
  const int total = 26;
  int sel = 0, top = 0;
  /* press 0 = the initial state, before any DOWN */
  int want_top[27], want_sel[27];
  /* Hand-derived once, cross-checked against the shipped DOWN handler's own
   * shape (pdna_gbpack.c:520-524): `if (sel < total-1) sel++;
   * gbpack_clamp_scroll(total, &sel, &top);` */
  for (int i = 0; i <= 26; i++) {
    if (i > 0) { if (sel < total - 1) sel++; gbpack_clamp_scroll(total, &sel, &top); }
    want_top[i] = top; want_sel[i] = sel;
  }
  /* Spot-check the exact values this D3 fix is FOR, independently reasoned:
   *  press 0: fresh open,            top=0  sel=0
   *  press 4: cursor on row 5 of 5,  top=0  sel=4   (NOT scrolled -- the bug)
   *  press 5: the 6th DOWN scrolls,  top=1  sel=5
   *  press 8: still scrolling 1:1,   top=4  sel=8
   *  press 25: sel pinned at total-1=25, top=21
   *  press 26: unchanged (sel already at the cap), top=21 sel=25 */
  CHECK(want_top[0] == 0 && want_sel[0] == 0, "press 0");
  CHECK(want_top[4] == 0 && want_sel[4] == 4, "press 4 (D3's own ground truth)");
  CHECK(want_top[5] == 1 && want_sel[5] == 5, "press 5");
  CHECK(want_top[8] == 4 && want_sel[8] == 8, "press 8");
  CHECK(want_top[25] == 21 && want_sel[25] == 25, "press 25");
  CHECK(want_top[26] == 21 && want_sel[26] == 25, "press 26 (pinned, no further move)");
}

/* D4: row_total() for the TM/HM pocket counts OWNED entries only. */
static void test_d4_owned_only_row_total(void) {
  printf("test_d4_owned_only_row_total\n");
  GbBag bag; memset(&bag, 0, sizeof bag);
  /* own TM01, TM03, HM01 (index 50) -- 3 owned out of 57 */
  bag.tmhm_counts[0] = 1;
  bag.tmhm_counts[2] = 5;
  bag.tmhm_counts[50] = 1;
  int total = g2pack_row_total(&bag, GBB_POCKET_TMHM);
  CHECK(total == 3 + 1, "3 owned + CANCEL");
  CHECK(g2_tm_n == 3, "g2_tm_n rebuilt to 3");
  CHECK(g2_tm_owned[0] == 0 && g2_tm_owned[1] == 2 && g2_tm_owned[2] == 50,
        "owned list is TM01, TM03, HM01 in ROM-index order");

  /* zeroing every owned index removes every row but CANCEL */
  memset(&bag, 0, sizeof bag);
  total = g2pack_row_total(&bag, GBB_POCKET_TMHM);
  CHECK(total == 1, "no owned TM/HM -> CANCEL alone");
}

/* D5/D8/description-column has_qty: paint the TM/HM pocket (one TM row, one
 * HM row) and the KEY pocket, and CANCEL, then inspect the qty column. */
static void test_has_qty_and_qty_paint(void) {
  printf("test_has_qty_and_qty_paint\n");
  GbBag bag; memset(&bag, 0, sizeof bag);
  bag.tmhm_counts[0] = 7;     /* TM01, qty column expected */
  bag.tmhm_counts[50] = 3;    /* HM01, D5: no qty column   */
  GbScreen gs; memset(&gs, 0, sizeof gs);

  grid_reset();
  g2pack_paint_list(&gs, &bag, GBB_POCKET_TMHM, 0, 0);
  int total = g2pack_row_total(&bag, GBB_POCKET_TMHM);
  CHECK(total == 3, "TM01 + HM01 + CANCEL");
  /* row 0 = TM01 (qty 7): qty_row(0)=3 -- '×' at QTY_COL, "7" (right-padded to
   * 2 via "%2u") at +1, both on the QTY row, one below the NAME row. */
  CHECK(g_text[3][QTY_COL] == (char)0xC3, "TM row shows the multiply-sign lead byte");
  /* "%2u" of 7 is " 7" (right-aligned, width 2) -- the digit lands at +2, +1 is
   * the blank tens column, matching a real single-digit count's own layout. */
  CHECK(g_text[3][QTY_COL + 2] == '7', "TM row qty digit");
  /* row 1 (name_row(1)=4, qty_row(1)=5) = HM01: D5, no qty column at all */
  CHECK(g_src[5][QTY_COL] == GBSCR_SRC_BLANK, "HM row: qty column is blank (D5)");
  CHECK(g_text[5][QTY_COL] == 0, "HM row: no text landed in the qty column");
  /* row 2 (name_row(2)=6, qty_row(2)=7) = CANCEL: no qty column either */
  CHECK(g_src[7][QTY_COL] == GBSCR_SRC_BLANK, "CANCEL row: qty column is blank");

  /* KEY pocket: no qty column on any real row, matching Gen 2's own contract */
  memset(&bag, 0, sizeof bag);
  bag.pockets[GBB_POCKET_KEY].count = 1;
  bag.pockets[GBB_POCKET_KEY].entries[0].id = 5;
  bag.pockets[GBB_POCKET_KEY].entries[0].qty = 1;   /* ignored for KEY, gb_bag.h's own contract */
  grid_reset();
  g2pack_paint_list(&gs, &bag, GBB_POCKET_KEY, 0, 0);
  CHECK(g_src[3][QTY_COL] == GBSCR_SRC_BLANK, "KEY row: qty column is blank");

  /* D8: a stored qty > 99 (a foreign/corrupt save, not reachable through this
   * core's own gbb_set_qty) renders as "**", not a wrong truncated number. */
  memset(&bag, 0, sizeof bag);
  bag.pockets[GBB_POCKET_ITEMS].count = 1;
  bag.pockets[GBB_POCKET_ITEMS].entries[0].id = 5;
  bag.pockets[GBB_POCKET_ITEMS].entries[0].qty = 150;
  grid_reset();
  g2pack_paint_list(&gs, &bag, GBB_POCKET_ITEMS, 0, 0);
  CHECK(g_text[3][QTY_COL + 1] == '*' && g_text[3][QTY_COL + 2] == '*',
        "D8: an out-of-range stored qty renders as ** not a truncated digit pair");
}

int main(void) {
  test_d1_tile_indices();
  test_d3_clamp_table();
  test_d4_owned_only_row_total();
  test_has_qty_and_qty_paint();
  if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
  printf("all host_gbpack_test checks passed\n");
  return 0;
}

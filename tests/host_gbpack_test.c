/* U5 fix pass (BACKLOG #67, review-opus ac9ffc0) + N5 re-verify (BACKLOG #111) --
 * pins the pure-logic half of source/pdna_gbpack.c's Gen-2 Pack painter on the
 * host.
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_gbpack_test.c \
 *      source/gb_bag.c source/gb_fields.c source/gb_session.c source/gb_edit.c \
 *      source/gen1_save.c source/gen1_write.c source/gen2_save.c source/gen2_write.c \
 *      source/data_tables.c source/gen3_to_gb.c source/gb_sidecar.c source/gen3_edit.c \
 *      source/gen3_mon.c source/gen3_box.c source/gen3_save.c source/gen3_daycare.c \
 *      source/gb_item_names.c \
 *      -o /tmp/hgbpack && /tmp/hgbpack
 *
 * WHY AN #include OF THE SHIPPED FILE, NOT A LINK TO IT. pdna_gbpack.c is NOT one
 * of this codebase's pure-C cores -- it is a tonc/ui/snd/pdna_app GB-screen SHELL
 * SCREEN (`#include <tonc.h>`, `ui.h`, `snd.h`, `pdna_app.h`, `pdna_trainer.h`,
 * `pdna_gen12.h` -- the last of which alone pulls pdna_box.h -> pdna_app.h ->
 * sprite_era.h/gen3_trainer.h, a transitive web with no host-buildable leaf).
 * tools/gb_oracle/README.md's own convention for testing a screen like this is to
 * extract the target static functions into a `body.inc` behind minimal
 * GbScreen/gbscr_cell()/gbscr_text() stand-ins rather than link the shipped file.
 *
 * N5 (BACKLOG #111): this test USED TO keep its own hand-typed copy of the
 * painter, "kept in sync BY HAND" -- and it had already drifted (g2pack_row_label's
 * HM branch was missing the "H"-prefix real cartridges print, and the D8
 * out-of-range-qty fallback printed "**" here vs the shipped file's own "??" --
 * '*' is not in the GB charmap). source/pdna_gbpack_body.inc is now the ONE
 * copy of every pure painter function (g2_tm_rebuild..gbpack_clamp_scroll, the
 * desc-box/pic-column/list painters), #include-d verbatim by BOTH this test and
 * the shipped screen -- so a future edit to the painter cannot drift here again,
 * it can only fail to compile or fail a CHECK.
 *
 * `siprintf` is newlib's own 2-arg (no size) sprintf-family function on the real
 * target (arm-none-eabi's siprintf == "integer sprintf", same call shape as
 * sprintf) -- pdna_gbpack_body.inc's own calls use that 2-arg shape, so the host
 * side maps it to plain `sprintf`, not `snprintf`, to keep the shared body
 * unmodified in both environments (every buffer the body writes into is a local
 * array sized generously above the format's real output, matching the target's
 * own unbounded-siprintf posture).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "gb_bag.h"
#include "pdna_gbscreen.h"   /* GbScreen, GbScrSrc, gbscr_cell()/gbscr_text() decls -- pure */

#define siprintf sprintf

/* ============================================================================
 * Minimal GbScreen/gbscr_cell()/gbscr_text() RECORDING stand-ins (gb_oracle/
 * README.md's own harness shape) -- a 20x18 grid of (src, tile) pairs, plus a
 * raw-string overlay per gbscr_text() call so a test can just strcmp() a row.
 * `GbScreen` itself is the REAL struct (pdna_gbscreen.h, pure C, unused fields
 * left zeroed) -- only the two paint primitives are faked. Declared BEFORE the
 * shared body (which calls them) rather than after, matching the body's own
 * calling order.
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
 * THE SHARED BODY (source/pdna_gbpack_body.inc) -- see this file's own header
 * for why an #include, not a copy. Comes after the gbscr_cell()/gbscr_text()
 * stand-ins above since the body's g2pack_desc_box/g2pack_pic_column/
 * g2pack_paint_list call them (C needs the callee declared, not necessarily
 * defined, but the stand-ins are defined here for one-TU simplicity).
 * ============================================================================ */
#include "pdna_gbpack_body.inc"

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
   * core's own gbb_set_qty) renders as "??" -- '*' is not in the GB charmap
   * (encodes to a blank), '?' is 0xE6 (U5 re-verify N2; this is also the exact
   * drift N5's own commit message caught: the old hand-copy still said "**"). */
  memset(&bag, 0, sizeof bag);
  bag.pockets[GBB_POCKET_ITEMS].count = 1;
  bag.pockets[GBB_POCKET_ITEMS].entries[0].id = 5;
  bag.pockets[GBB_POCKET_ITEMS].entries[0].qty = 150;
  grid_reset();
  g2pack_paint_list(&gs, &bag, GBB_POCKET_ITEMS, 0, 0);
  CHECK(g_text[3][QTY_COL + 1] == '?' && g_text[3][QTY_COL + 2] == '?',
        "D8: an out-of-range stored qty renders as ?? not a truncated digit pair");
}

/* N1 (real-cartridge re-verify, carried into N5's own coverage ask): an HM row
 * prints a literal 'H' + left-aligned digit ("H3"), a TM row its two-digit
 * number ("02") -- tmhm.asm's own asymmetry, easy to lose in a hand-copy (this
 * is exactly the branch the pre-N5 test copy was missing). */
static void test_tmhm_hm_h_prefix(void) {
  printf("test_tmhm_hm_h_prefix\n");
  GbBag bag; memset(&bag, 0, sizeof bag);
  bag.tmhm_counts[1] = 1;    /* TM02 (index 1) */
  bag.tmhm_counts[52] = 1;   /* HM03 (index 52 = 50 + 3 - 1) */
  GbScreen gs; memset(&gs, 0, sizeof gs);
  grid_reset();
  g2pack_paint_list(&gs, &bag, GBB_POCKET_TMHM, 0, 0);
  /* row 0 (TM02): TMNUM_COL prints "02" */
  CHECK(g_text[name_row(0)][TMNUM_COL] == '0' && g_text[name_row(0)][TMNUM_COL + 1] == '2',
        "TM row prints its two-digit number");
  /* row 1 (HM03): TMNUM_COL prints "H3" */
  CHECK(g_text[name_row(1)][TMNUM_COL] == 'H' && g_text[name_row(1)][TMNUM_COL + 1] == '3',
        "HM row prints the H-prefix + left-aligned digit (N1)");
}

/* N3 (BACKLOG #111): g2pack_desc_box must offset EVERY frame tile id by
 * 6*frame (the frames block is 9 frames x 6 tiles, G2I_UL..G2I_DR = 0..5 within
 * one frame) -- pin frame 0 (unchanged corners) and frame 3 (the brief's own
 * worked example: G2I_UL + 18 at cell (0,12)) by reading gbscr_cell's raw tile
 * id back out of the recording grid. This is the host-side half of N3's own
 * pixel proof (the brief's fallback when the oracle cannot drive the Pack
 * screen directly, which docs/briefs/U5-gen2-pack-brief.md's own oracle
 * plumbing does not yet do for a description-box-only repaint) -- an actual
 * on-hardware/mGBA VRAM comparison of a frame-3 OPTIONS save was NOT captured
 * this slice; that gap is reported, not silently closed. */
static void test_n3_desc_box_frame(void) {
  printf("test_n3_desc_box_frame\n");
  GbScreen gs; memset(&gs, 0, sizeof gs);

  grid_reset();
  g2pack_desc_box(&gs, 0);
  CHECK(g_tile[12][0] == G2I_UL, "frame 0: top-left corner is G2I_UL unshifted");
  CHECK(g_tile[12][19] == G2I_UR, "frame 0: top-right corner is G2I_UR unshifted");
  CHECK(g_tile[17][0] == G2I_DL, "frame 0: bottom-left corner is G2I_DL unshifted");
  CHECK(g_tile[17][19] == G2I_DR, "frame 0: bottom-right corner is G2I_DR unshifted");
  CHECK(g_tile[12][10] == G2I_H, "frame 0: top edge is G2I_H unshifted");
  CHECK(g_tile[14][0] == G2I_V, "frame 0: left edge is G2I_V unshifted");

  grid_reset();
  g2pack_desc_box(&gs, 3);
  CHECK(g_tile[12][0] == G2I_UL + 6 * 3, "frame 3: top-left corner is G2I_UL + 18 (the brief's own worked example)");
  CHECK(g_tile[12][19] == G2I_UR + 6 * 3, "frame 3: top-right corner is G2I_UR + 18");
  CHECK(g_tile[17][0] == G2I_DL + 6 * 3, "frame 3: bottom-left corner is G2I_DL + 18");
  CHECK(g_tile[17][19] == G2I_DR + 6 * 3, "frame 3: bottom-right corner is G2I_DR + 18");
  CHECK(g_tile[12][10] == G2I_H + 6 * 3, "frame 3: top edge is G2I_H + 18");
  CHECK(g_tile[14][0] == G2I_V + 6 * 3, "frame 3: left edge is G2I_V + 18");
  CHECK(g_src[12][0] == GBSCR_SRC_TEXTBOX, "the box still reads the frames block, not some other source");
}

/* g2pack_desc_label: the PC-store disambiguation line (D6) -- present only in
 * the PC store, cleared to blank the moment the screen is NOT in the PC store. */
static void test_desc_label(void) {
  printf("test_desc_label\n");
  GbScreen gs; memset(&gs, 0, sizeof gs);

  grid_reset();
  g2pack_desc_label(&gs, true);
  CHECK(g_text[14][1] == 'P' && g_text[14][2] == 'C', "in_pc=true prints the PC ITEM STORE label");

  grid_reset();
  g2pack_desc_label(&gs, false);
  CHECK(g_text[14][1] == 0, "in_pc=false clears row 14 (no stale label)");
  CHECK(g_src[14][1] == GBSCR_SRC_BLANK, "in_pc=false: row 14 is GBSCR_SRC_BLANK");
}

/* N5 coverage ask: g2pack_pic_column -- the header strip (row 0, 20 PACKMENU
 * tiles 0x28..0x3B), the picture block (rows 3-5, PACK_M vs PACK_F by gender,
 * rom_idx*15 + r*5 + c per D-Kris), and the per-pocket nameplate label ids
 * (kLabelIds, row 8). */
static void test_n5_pic_column(void) {
  printf("test_n5_pic_column\n");
  GbScreen gs; memset(&gs, 0, sizeof gs);

  grid_reset();
  g2pack_pic_column(&gs, 0 /* ITEMS */, false, false);
  CHECK(g_src[0][0] == GBSCR_SRC_PACKMENU && g_tile[0][0] == 0x28, "header strip col 0 = 0x28");
  CHECK(g_tile[0][19] == 0x28 + 19, "header strip col 19 = 0x28+19 (0x3B)");
  int rom_idx_items = kPackRomIdx[0];   /* cyc=0 (ITEMS) -> ROM order index 1 */
  CHECK(g_src[3][0] == GBSCR_SRC_PACK_M, "male picture source when female=false");
  CHECK(g_tile[3][0] == (uint8_t)(rom_idx_items * 15 + 0), "picture tile (0,0) = rom_idx*15");
  CHECK(g_tile[5][4] == (uint8_t)(rom_idx_items * 15 + 2 * 5 + 4), "picture tile (4,2) = rom_idx*15+14");
  const uint8_t* lbl_items = kLabelIds[0];
  CHECK(g_tile[8][0] == lbl_items[0] && g_tile[8][4] == lbl_items[4], "ITEMS nameplate label row");

  /* female=true switches the picture source (D-Kris) without touching the
   * header strip or the label row. */
  grid_reset();
  g2pack_pic_column(&gs, 0, false, true);
  CHECK(g_src[3][0] == GBSCR_SRC_PACK_F, "female picture source (D-Kris fix)");
  CHECK(g_tile[8][0] == lbl_items[0], "label row unaffected by gender");

  /* in_pc reuses cyc 0's (ITEMS') art regardless of the real cyc value (D6). */
  grid_reset();
  g2pack_pic_column(&gs, 2 /* KEY */, true, false);
  int rom_idx_pc = kPackRomIdx[0];
  CHECK(g_tile[3][0] == (uint8_t)(rom_idx_pc * 15 + 0), "PC store reuses ITEMS' picture, not KEY's (D6)");
  CHECK(g_tile[8][0] == lbl_items[0], "PC store reuses ITEMS' nameplate label, not KEY's (D6)");
}

int main(void) {
  test_d1_tile_indices();
  test_d3_clamp_table();
  test_d4_owned_only_row_total();
  test_has_qty_and_qty_paint();
  test_tmhm_hm_h_prefix();
  test_n3_desc_box_frame();
  test_desc_label();
  test_n5_pic_column();
  if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
  printf("all host_gbpack_test checks passed\n");
  return 0;
}

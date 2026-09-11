/* Host test for source/g2card_cells.c -- the Gen-2 trainer card's pure
 * static graphical cell table (BACKLOG #96 D11). Asserts every cell the
 * table can emit resolves inside its located block's byte length (the same
 * bound gbscr_flush()'s VRAM blit relies on to never read past the cached
 * tail buffer), and pins the three facts the brief names as having survived
 * mutation before: the pic repeat cell (18,9) is display index 4;
 * GBSCR_SRC_STATUSWORD is located at gu->leaders - 96; G2L_BADGES_WORD is 80.
 *
 *   cc -std=c11 -Wall -Wextra -I source -DPDNA_GBSCREEN_HOST_TEST \
 *      tests/host_gbcard_cells_test.c source/g2card_cells.c \
 *      source/pdna_gbscreen.c source/gb_edit.c source/gen1_save.c \
 *      source/gen2_save.c -o /tmp/hgbcardcells && /tmp/hgbcardcells
 *
 * -DPDNA_GBSCREEN_HOST_TEST compiles pdna_gbscreen.c's tonc/FatFs I/O half
 * out (same posture as host_gbscreen_test.c) -- gbscr_block_off()/
 * gbscr_block_bytes() live outside that guard, so they still link.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "g2card_cells.h"
#include "pdna_gbscreen.h"
#include "rom_gbui.h"

/* gb_edit.c (linked in for gen1_save.c/gen2_save.c's own transitive needs via
 * this test's object list) pulls in data_tables.h for stat/exp/PP lookups
 * this test never exercises -- link-only stubs, same technique
 * host_gbscreen_test.c already uses (see its own comment) since
 * data_tables.c is GENERATED and not vendored into every checkout. */
void pk_base_stats(uint16_t internal, uint8_t out[6]) { (void)internal; for (int i = 0; i < 6; i++) out[i] = 0; }
uint8_t pk_species_gender_ratio(uint16_t internal) { (void)internal; return 0; }
uint8_t pk_species_growth(uint16_t internal) { (void)internal; return 0; }
uint8_t pk_move_pp(uint16_t move_id) { (void)move_id; return 0; }
uint32_t pk_exp_for_level(uint8_t growth_rate, uint8_t level) { (void)growth_rate; (void)level; return 0; }
uint8_t pk_level_from_exp(uint8_t growth_rate, uint32_t exp) { (void)growth_rate; (void)exp; return 1; }

static int fails = 0, checks = 0;

static void expect_true(const char* what, bool got) {
  checks++;
  if (!got) { fails++; printf("FAIL %s\n", what); }
}
static void expect_int(const char* what, long got, long want) {
  checks++;
  if (got != want) { fails++; printf("FAIL %s: got %ld want %ld\n", what, got, want); }
}

/* A fake but internally-consistent RomGbUi -- the exact byte lengths matter
 * (gbscr_block_bytes() is a fixed formula per src/gen, not read from this
 * struct), but the OFFSETS just need to be distinguishable and plausible
 * (leaders - 96 for STATUSWORD, non-overlapping blocks) so an out-of-bounds
 * cell shows up as landing outside [off, off+bytes) rather than accidentally
 * inside a neighbour's block. */
static RomGbUi fake_gu(void) {
  RomGbUi gu; memset(&gu, 0, sizeof gu);
  gu.font       = 0x10000;
  gu.cardgfx    = 0x20000;              /* 6 tiles, 96 B   */
  gu.leaders    = 0x21000;              /* 86 tiles, 1376 B; STATUSWORD = leaders-96 */
  gu.cardpic_m  = 0x30000;              /* 35 tiles, 560 B */
  gu.cardpic_f  = 0x30230;              /* Kris = Chris + 0x230, D9's own invariant */
  gu.badges     = 0x40000;              /* Gen 2: 44 tiles, 704 B */
  gu.ok = 1;
  return gu;
}

/* Bytes per tile for the given src/gen -- matches gbscr_block_bytes()'s own
 * per-case tile-count*stride derivation (pdna_gbscreen.c:175): every block
 * the Gen-2 card table can emit is 2bpp (16 B/tile) EXCEPT FONT, which is
 * the shared 1bpp (8 B/tile) font both generations use (the (r) hint arrow
 * cell). Gen 1's TEXTBOX is also 2bpp; Gen 2's (via `frames`) is 1bpp, but
 * this table never emits a TEXTBOX cell, so that case is not reachable here. */
static uint32_t tile_stride(GbScrSrc src) {
  return (src == GBSCR_SRC_FONT) ? 8u : 16u;
}

/* Every cell in `cells` must resolve inside its located block. Returns the
 * number that failed (0 == all in bounds). FONT is special-cased: the
 * shell's own tile lookup (pdna_gbscreen.c's gbscr_tile_pixels ->
 * rom_gbui_glyph(), rom_gbui.c:1132) does not index gu->font directly by the
 * cell's raw `index` byte -- it requires index >= 0x80 (the GB charmap's own
 * "this is a glyph, not a control byte" bit) and subtracts 0x80 before
 * multiplying by the 8 B stride, so the bounds formula here mirrors that
 * exact subtraction rather than the located-block formula the brief's other
 * five sources (CARDGFX/CARDPIC_M/CARDPIC_F/STATUSWORD/LEADERS/BADGES) use
 * directly. */
static int check_cells(const char* what, const RomGbUi* gu, uint8_t gen,
                        const G2CardCell* cells, int n) {
  int bad = 0;
  for (int i = 0; i < n; i++) {
    uint32_t off    = gbscr_block_off(gu, gen, cells[i].src);
    uint32_t bytes  = gbscr_block_bytes(gen, cells[i].src);
    uint32_t stride = tile_stride(cells[i].src);
    uint32_t tile_idx = cells[i].index;
    bool ok;
    if (cells[i].src == GBSCR_SRC_FONT) {
      ok = (tile_idx >= 0x80u) && bytes != 0;
      if (ok) { tile_idx -= 0x80u; ok = (tile_idx * stride + stride) <= bytes; }
    } else {
      uint32_t cell_off = off + tile_idx * stride;
      ok = (bytes != 0) && (cell_off + stride <= off + bytes);
    }
    checks++;
    if (!ok) {
      bad++; fails++;
      printf("FAIL %s[%d]: (%u,%u) src=%d index=%u off=0x%x bytes=%u out of bounds\n",
             what, i, cells[i].x, cells[i].y, (int)cells[i].src, cells[i].index, off, bytes);
    }
  }
  return bad;
}

int main(void) {
  RomGbUi gu = fake_gu();

  /* -- bounds: every cell g2card_cells.c can emit, on both genders/pages -- */
  G2CardCell upper_m[G2CARD_UPPER_CELLS], upper_f[G2CARD_UPPER_CELLS];
  int nu_m = g2card_build_upper_cells(false, upper_m);
  int nu_f = g2card_build_upper_cells(true, upper_f);
  expect_int("upper cell count (male)", nu_m, G2CARD_UPPER_CELLS);
  expect_int("upper cell count (female)", nu_f, G2CARD_UPPER_CELLS);
  check_cells("upper(male)", &gu, 2, upper_m, nu_m);
  check_cells("upper(female)", &gu, 2, upper_f, nu_f);

  G2CardCell p1[G2CARD_PAGE1_CELLS];
  int np1 = g2card_build_page1_cells(p1);
  expect_int("page1 cell count", np1, G2CARD_PAGE1_CELLS);
  check_cells("page1", &gu, 2, p1, np1);

  bool all_owned[8]; for (int i = 0; i < 8; i++) all_owned[i] = true;
  G2CardCell p2[G2CARD_PAGE2_CELLS_MAX];
  int np2 = g2card_build_page2_cells(all_owned, p2);
  expect_int("page2 cell count (all owned)", np2, 85 + 4 * 8);
  check_cells("page2(all owned)", &gu, 2, p2, np2);

  G2CardCell p2n[G2CARD_PAGE2_CELLS_MAX];
  int np2n = g2card_build_page2_cells(NULL, p2n);
  expect_int("page2 cell count (none owned)", np2n, 85);
  check_cells("page2(none owned)", &gu, 2, p2n, np2n);

  bool one_owned[8] = {0}; one_owned[3] = true;
  G2CardCell p2o[G2CARD_PAGE2_CELLS_MAX];
  int np2o = g2card_build_page2_cells(one_owned, p2o);
  expect_int("page2 cell count (one owned)", np2o, 85 + 4);
  check_cells("page2(one owned)", &gu, 2, p2o, np2o);

  /* -- the three pinned facts -- */

  /* fact 1: the pic repeat cell (18,9) is display index 4. */
  {
    bool found = false;
    for (int i = 0; i < nu_m; i++)
      if (upper_m[i].x == 18 && upper_m[i].y == 9) {
        found = true;
        expect_int("repeat cell (18,9) src", upper_m[i].src, GBSCR_SRC_CARDPIC_M);
        expect_int("repeat cell (18,9) index", upper_m[i].index, 4);
      }
    expect_true("repeat cell (18,9) present", found);
  }

  /* fact 2: STATUSWORD == leaders - 96. */
  expect_int("STATUSWORD = leaders - 96",
             (long)gbscr_block_off(&gu, 2, GBSCR_SRC_STATUSWORD),
             (long)(gu.leaders - 96u));

  /* fact 3: G2L_BADGES_WORD == 80. */
  expect_int("G2L_BADGES_WORD", G2L_BADGES_WORD, 80);
  {
    bool found = false;
    for (int i = 0; i < np2; i++)
      if (p2[i].src == GBSCR_SRC_LEADERS && p2[i].index == G2L_BADGES_WORD) found = true;
    expect_true("a page2 cell uses index G2L_BADGES_WORD (80)", found);
  }

  printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
  return fails ? 1 : 0;
}

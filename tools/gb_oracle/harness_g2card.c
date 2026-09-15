/* gb_oracle demo harness -- the Gen-2 trainer card's STATIC "upper" cell set
 * (source/g2card_cells.c's g2card_build_upper_cells(), BACKLOG #96 D11: the
 * CARDGFX "ID"/"No" glyphs, the 5x7 player-pic grid, the divider fill/cap
 * and the right-corner cell -- 51 cells, always shown, never save-dependent)
 * resolved to REAL ROM FILE BYTE OFFSETS via rom_gbui_open() (locate-by-
 * shape in the given .gbc) + gbscr_block_off() (source/pdna_gbscreen.c's own
 * block-offset table) -- the exact two functions the shipped painter and
 * tests/host_gbcard_cells_test.c already use. female=false/has_corner=false
 * is correct and save-independent for a Gold ROM specifically (the
 * protagonist is always the boy sprite; Gold has no separate CARDCORNER
 * block, BACKLOG #125) -- no save file is parsed, no Python model of the
 * cell geometry exists anywhere, this prints the real compiled function's
 * own output.
 *
 * Prints one "x y rom_offset" line per cell (decimal). Built at runtime by
 * celldiff.py --demo (see PDNA_GBOR_DEMO in this directory); not a
 * standalone Makefile target.
 *
 *   cc -std=c11 -Wall -Wextra -I <pokedna>/source -DPDNA_GBSCREEN_HOST_TEST \
 *      harness_g2card.c <pokedna>/source/g2card_cells.c \
 *      <pokedna>/source/pdna_gbscreen.c <pokedna>/source/rom_gbui.c \
 *      <pokedna>/source/gb_sprite_codec.c <pokedna>/source/gb_edit.c \
 *      <pokedna>/source/gen1_save.c <pokedna>/source/gen2_save.c \
 *      -o /tmp/g2card_dump && /tmp/g2card_dump path/to/Gold.gbc
 *
 * gb_edit.c is linked in only for pdna_gbscreen.c's gb_char_encode() (used
 * by gbscr_text()'s EXIT-box glyphs elsewhere in the shared shell, pulled in
 * transitively); gen1_save.c/gen2_save.c are gb_edit.c's own link-time needs.
 * gb_edit.c also references data_tables.h's stat/exp/PP lookups this harness
 * never exercises -- link-only stubs below, same technique
 * tests/host_gbcard_cells_test.c already uses (data_tables.c is GENERATED
 * and not vendored into every checkout).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "g2card_cells.h"
#include "pdna_gbscreen.h"
#include "rom_gbui.h"
#include "pdna_origin_art.h"   /* PDNA_GEN1/PDNA_GEN2 */

void pk_base_stats(uint16_t internal, uint8_t out[6]) { (void)internal; for (int i = 0; i < 6; i++) out[i] = 0; }
uint8_t pk_species_gender_ratio(uint16_t internal) { (void)internal; return 0; }
uint8_t pk_species_growth(uint16_t internal) { (void)internal; return 0; }
uint8_t pk_move_pp(uint16_t move_id) { (void)move_id; return 0; }
uint32_t pk_exp_for_level(uint8_t growth_rate, uint8_t level) { (void)growth_rate; (void)level; return 0; }
uint8_t pk_level_from_exp(uint8_t growth_rate, uint32_t exp) { (void)growth_rate; (void)exp; return 1; }

typedef struct { const uint8_t* buf; uint32_t size; } G2RomCtx;

static bool read_fn(void* ctx, uint32_t off, void* out, uint32_t n) {
  G2RomCtx* c = (G2RomCtx*)ctx;
  if ((uint64_t)off + n > c->size) return false;
  memcpy(out, c->buf + off, n);
  return true;
}

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: g2card_dump <rom.gbc>\n"); return 2; }
  FILE* f = fopen(argv[1], "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* buf = malloc((size_t)sz);
  if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "short read\n"); return 2; }
  fclose(f);

  G2RomCtx ctx = { buf, (uint32_t)sz };
  static uint8_t scratch[ROM_GBUI_SCRATCH_MIN * 4];
  RomGbUi gu; memset(&gu, 0, sizeof gu);
  int ok = rom_gbui_open(&gu, read_fn, &ctx, (uint32_t)sz, scratch, sizeof scratch);
  if (!ok || !gu.ok) { fprintf(stderr, "rom_gbui_open failed (ok=%d gu.ok=%d)\n", ok, gu.ok); return 3; }
  if (gu.gen != PDNA_GEN2) { fprintf(stderr, "not a Gen-2 ROM (gu.gen=%d)\n", gu.gen); return 3; }

  G2CardCell cells[G2CARD_UPPER_CELLS];
  int n = g2card_build_upper_cells(false, false, cells);
  for (int i = 0; i < n; i++) {
    uint32_t base = gbscr_block_off(&gu, PDNA_GEN2, cells[i].src);
    uint32_t off = base + (uint32_t)cells[i].index * 16u;
    printf("%d %d %u\n", cells[i].x, cells[i].y, off);
  }
  free(buf);
  return 0;
}

/* gb_oracle demo harness -- the Gen-1 Item bag's STATIC BORDER cells (the
 * frame g1bag_border() draws: the outer box, the nested EXIT box -- not the
 * dynamic item list g1bag_paint_list() draws, which needs a live GbBag) --
 * resolved to REAL ROM FILE BYTE OFFSETS the same way harness_g2card.c does
 * for the trainer card.
 *
 * "g1bag_body.inc" (built next to this file by celldiff.py --demo, NOT
 * checked in) is a VERBATIM `sed` extraction of source/pdna_gbbag.c's own
 * enum G1I_, the BOX_/EXIT_ geometry defines, ROWS_VISIBLE, name_row(),
 * qty_row() and g1bag_border() itself -- so this harness runs the actual
 * shipped function, not a hand-retyped Python (or C) model of its geometry;
 * it can never silently drift from what ships (README.md's own "Building a
 * harness" design, generalized here into a permanent, regenerate-every-run
 * extraction instead of a one-off per review). pdna_gbbag.c itself is NOT
 * compiled or linked -- it #includes <tonc.h>/ui.h/snd.h/the FatFs stack, none
 * of which exist on a host build; only the ~80-line pure geometry slice this
 * harness needs is extracted.
 *
 * Prints one "x y rom_offset" line per border cell (decimal), keyed to
 * whichever (x,y) g1bag_border() itself wrote GBSCR_SRC_TEXTBOX into.
 *
 *   cc -std=c11 -Wall -Wextra -I <pokedna>/source -I. -DPDNA_GBSCREEN_HOST_TEST \
 *      harness_g1bag.c <pokedna>/source/pdna_gbscreen.c \
 *      <pokedna>/source/rom_gbui.c <pokedna>/source/gb_sprite_codec.c \
 *      <pokedna>/source/gb_edit.c <pokedna>/source/gen1_save.c \
 *      <pokedna>/source/gen2_save.c -o /tmp/g1bag_dump && \
 *      /tmp/g1bag_dump path/to/Red.gb
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "pdna_gbscreen.h"
#include "rom_gbui.h"
#include "pdna_origin_art.h"   /* PDNA_GEN1 */

void pk_base_stats(uint16_t internal, uint8_t out[6]) { (void)internal; for (int i = 0; i < 6; i++) out[i] = 0; }
uint8_t pk_species_gender_ratio(uint16_t internal) { (void)internal; return 0; }
uint8_t pk_species_growth(uint16_t internal) { (void)internal; return 0; }
uint8_t pk_move_pp(uint16_t move_id) { (void)move_id; return 0; }
uint32_t pk_exp_for_level(uint8_t growth_rate, uint8_t level) { (void)growth_rate; (void)level; return 0; }
uint8_t pk_level_from_exp(uint8_t growth_rate, uint32_t exp) { (void)growth_rate; (void)exp; return 1; }

#include "g1bag_body.inc"   /* g1bag_border() -- the real shipped function, sed-extracted */

typedef struct { const uint8_t* buf; uint32_t size; } G1RomCtx;

static bool read_fn(void* ctx, uint32_t off, void* out, uint32_t n) {
  G1RomCtx* c = (G1RomCtx*)ctx;
  if ((uint64_t)off + n > c->size) return false;
  memcpy(out, c->buf + off, n);
  return true;
}

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: g1bag_dump <rom.gb>\n"); return 2; }
  FILE* f = fopen(argv[1], "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t* buf = malloc((size_t)sz);
  if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "short read\n"); return 2; }
  fclose(f);

  G1RomCtx ctx = { buf, (uint32_t)sz };
  static uint8_t scratch[ROM_GBUI_SCRATCH_MIN * 4];
  RomGbUi gu; memset(&gu, 0, sizeof gu);
  int ok = rom_gbui_open(&gu, read_fn, &ctx, (uint32_t)sz, scratch, sizeof scratch);
  if (!ok || !gu.ok) { fprintf(stderr, "rom_gbui_open failed (ok=%d gu.ok=%d)\n", ok, gu.ok); return 3; }
  if (gu.gen != PDNA_GEN1) { fprintf(stderr, "not a Gen-1 ROM (gu.gen=%d)\n", gu.gen); return 3; }

  GbScreen gs; memset(&gs, 0, sizeof gs);
  gs.ok = true;
  gs.gen = PDNA_GEN1;
  g1bag_border(&gs);

  for (int y = 0; y < GBSCR_ROWS; y++) {
    for (int x = 0; x < GBSCR_COLS; x++) {
      int i = y * GBSCR_COLS + x;
      if (gs.src[i] != GBSCR_SRC_TEXTBOX) continue;   /* only cells g1bag_border() touched */
      uint32_t base = gbscr_block_off(&gu, PDNA_GEN1, GBSCR_SRC_TEXTBOX);
      uint32_t off = base + (uint32_t)gs.map[i] * 16u;
      printf("%d %d %u\n", x, y, off);
    }
  }
  free(buf);
  return 0;
}

/* Throwaway D9 verification harness (BACKLOG #96) -- NOT part of the repo's
 * test suite (no cc-line header, run_host_tests.py's glob won't touch it).
 * Opens a real Crystal.gbc, locates cardpic_m via rom_gbui_open(), and dumps
 * the RGB15 pixels rom_gbui_tile() resolves for DISPLAY index 4 (the (18,9)
 * repeat cell / (18,1) grid cell g2card_cells.c paints) -- the exact call
 * gbscr_tile_pixels()'s GBSCR_SRC_CARDPIC_M case makes in production. Prints
 * one hex line so a Python script can diff it against oracle.py's captured
 * real-VRAM tile bitmap.
 *
 *   cc -std=c11 -I source tests/d9_cardpic_probe.c source/rom_gbui.c \
 *      source/gb_sprite_codec.c -o /tmp/d9probe && /tmp/d9probe <Crystal.gbc>
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rom_gbui.h"

static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FILE* f = (FILE*)ctx;
  if (fseek(f, (long)off, SEEK_SET) != 0) return false;
  if (fread(dst, 1, len, f) != len) return false;
  return true;
}

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <rom.gbc>\n", argv[0]); return 2; }
  FILE* f = fopen(argv[1], "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
  fseek(f, 0, SEEK_END);
  uint32_t size = (uint32_t)ftell(f);

  static uint8_t scratch[ROM_GBUI_SCRATCH_MIN];
  RomGbUi gu; memset(&gu, 0, sizeof gu);
  int ok = rom_gbui_open(&gu, file_read, f, size, scratch, sizeof scratch);
  if (!ok || !gu.ok) { fprintf(stderr, "rom_gbui_open failed\n"); return 1; }

  printf("gen=%d cardpic_m=0x%x cardpic_f=0x%x cardgfx=0x%x colmajor=%d anchor_cardpic=0x%x\n",
         gu.gen, gu.cardpic_m, gu.cardpic_f, gu.cardgfx, gu.cardpic_colmajor,
         gu.anchor[ROM_GBUI_ANCH_CARDPIC]);

  {
    uint32_t anchor = gu.anchor[ROM_GBUI_ANCH_CARDPIC];
    uint8_t w[33];
    file_read(f, anchor, w, 33);
    printf("anchor bytes:");
    for (int i = 0; i < 33; i++) printf(" %02x", w[i]);
    printf("\n");
  }

  uint16_t px[64];
  int r = rom_gbui_tile(&gu, gu.cardpic_m, 4, 2, 5, 7, gu.cardpic_colmajor, px);
  if (!r) { fprintf(stderr, "rom_gbui_tile failed\n"); return 1; }

  printf("PIXELS4:");
  for (int i = 0; i < 64; i++) printf(" %04x", px[i]);
  printf("\n");

  /* Also the raw storage tile (display 4 -> storage 28 = 0x1C per D9's own
   * math: col*grid_h+row = 4*7+0 = 28) via the no-reorder call, to confirm
   * the reorder is actually doing something (should match the display call
   * exactly, since rom_gbui_tile's colmajor path IS this same lookup). */
  uint16_t px_raw[64];
  int r2 = rom_gbui_tile(&gu, gu.cardpic_m, 28, 2, 0, 0, 0, px_raw);
  if (r2) {
    int same = memcmp(px, px_raw, sizeof px) == 0;
    printf("STORAGE28_MATCHES_DISPLAY4: %d\n", same);
  }

  fclose(f);
  return 0;
}

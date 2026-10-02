/* BACKLOG #103 -- rom_wallpaper's tile expander: the word-wide path against the ORIGINAL
 * per-pixel algorithm (kept below verbatim as the reference).
 *
 * Build + run (from the repo root):
 *   cc -std=c11 -I source tests/host_wpexpand_test.c source/rom_wallpaper.c \
 *      source/map_render.c source/rom_map.c -o /tmp/hwpx && /tmp/hwpx
 *
 * No ROM needed: seeded random tile sheets, palettes, flip flags and backdrop shapes.
 * Aligned source+destination exercise the 32-bit path; a source/destination shifted by one
 * byte (or two) exercises the byte-wise fallback. Every case must be byte-identical to the
 * reference, through BOTH public entry points (expand_tile = skip0 off, expand_cell = the base
 * pass plus the index-0-transparent overlay pass).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "rom_wallpaper.h"

static int fails = 0, checks = 0;
#define CHK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* ---- the reference: the pre-#103 expander, verbatim --------------------------------- */
static void ref_px(const uint8_t* src, int hflip, int vflip, const uint16_t pal[16],
                   int skip0, uint16_t out[64]) {
  for (int y = 0; y < 8; y++) {
    int sy = vflip ? (7 - y) : y;
    for (int xb = 0; xb < 4; xb++) {
      uint8_t b = src[sy * 4 + xb];
      uint8_t lo = (uint8_t)(b & 0x0Fu), hi = (uint8_t)((b >> 4) & 0x0Fu);
      int x0 = xb * 2, x1 = xb * 2 + 1;
      if (hflip) { x0 = 7 - x0; x1 = 7 - x1; }
      if (!(skip0 && lo == 0)) out[y * 8 + x0] = pal[lo];
      if (!(skip0 && hi == 0)) out[y * 8 + x1] = pal[hi];
    }
  }
}
static int ref_cell(const uint8_t* tiles, uint32_t tiles_bytes, uint16_t e, int tx, int ty,
                    const RomWpBase* bs, const uint16_t pal[ROM_WP_PAL_BANKS][16], uint16_t out[64]) {
  uint16_t tid = (uint16_t)(e & 0x3FFu);
  if ((uint32_t)tid * 32u + 32u > tiles_bytes) return 0;
  const uint16_t* bg = pal[ROM_WP_EM_BANKS - 1];
  int rb = (e >> 12) & 0xF;                                  /* #311: R/S reads banks 1:1, clamped */
  const uint16_t* fg = bs->rs ? pal[rb >= ROM_WP_PAL_BANKS ? ROM_WP_PAL_BANKS - 1 : rb]
                              : pal[rom_wallpaper_pal_bank(rb)];
  uint16_t tone = bg[1];
  uint16_t fill = tone;
  if (!bs->cols && tone == 0) fill = fg[0];
  if (bs->rs) fill = 0x1041u;   /* review-zr D2: UI_BG, not the transparency key pal[0][0] */
  for (int i = 0; i < 64; i++) out[i] = fill;
  if (bs->cols && bs->rows) {
    int pi = (ty % bs->rows) * bs->cols + (tx % bs->cols);
    if (pi < bs->used) {
      uint32_t boff = ((uint32_t)bs->first + (uint32_t)pi) * 32u;
      if (boff + 32u <= tiles_bytes) {
        uint16_t bp[16];
        memcpy(bp, bg, sizeof bp);
        bp[0] = tone;
        ref_px(tiles + boff, 0, 0, bp, 0, out);
      }
    }
  }
  ref_px(tiles + (uint32_t)tid * 32u, (e >> 10) & 1, (e >> 11) & 1, fg, 1, out);
  return 1;
}

static uint32_t seed = 0x5EED1234u;
static uint32_t rnd(void) { seed = seed * 1664525u + 1013904223u; return seed >> 8; }

int main(void) {
  enum { NT = 40 };
  static _Alignas(4) uint8_t sheet[NT * 32 + 4];
  static _Alignas(4) uint16_t outbuf[64 + 2], refbuf[64];
  int bad_tile = 0, bad_cell = 0, ncases = 0;
  for (int iter = 0; iter < 3000; iter++) {
    for (int i = 0; i < NT * 32; i++) {
      uint32_t r = rnd();
      sheet[i] = (uint8_t)r;
      if ((iter & 3) == 0 && (r & 0x300u) == 0) sheet[i] = 0;          /* transparent runs */
      if ((iter & 3) == 1 && (r & 0x300u) == 0) sheet[i] = 0x11;       /* no-zero-nibble rows */
    }
    uint16_t pal[ROM_WP_PAL_BANKS][16];
    for (int b = 0; b < ROM_WP_PAL_BANKS; b++)
      for (int c = 0; c < 16; c++) pal[b][c] = (uint16_t)rnd();
    if (iter % 7 == 0) pal[ROM_WP_EM_BANKS - 1][1] = 0;              /* tone 0: the fg[0] fill */
    RomWpBase bs = { (uint16_t)(rnd() % 8u), (uint8_t)(rnd() % 4u), (uint8_t)(rnd() % 4u), (uint8_t)(rnd() % 12u), 0 };
    if (iter % 5 == 0) bs.cols = 0;
    if (iter % 3 == 0) { bs.rs = 1; bs.cols = 0; }                  /* #311: R/S flat backdrop */
    uint16_t e = (uint16_t)rnd();
    e = (uint16_t)((e & 0xFC00u) | (rnd() % NT));                      /* in-range tid, any flags/bank */
    int tx = (int)(rnd() % 20u), ty = (int)(rnd() % 18u);
    int shift = iter % 3;     /* 0: aligned/aligned, 1: unaligned dst, 2: unaligned src */

    /* expand_cell */
    memset(outbuf, 0xA5, sizeof outbuf); memset(refbuf, 0xA5, sizeof refbuf);
    uint16_t* dst = (uint16_t*)((uint8_t*)outbuf + (shift == 1 ? 2 : 0));
    const uint8_t* src = sheet + (shift == 2 ? 1 : 0);
    /* shift the whole sheet by one byte for the unaligned-src case: copy so contents match */
    static uint8_t shifted[NT * 32 + 8];
    if (shift == 2) { memcpy(shifted + 1, sheet, NT * 32); src = shifted + 1; }
    else src = sheet;
    int r1 = rom_wallpaper_expand_cell(src, NT * 32, e, tx, ty, &bs, pal, dst);
    int r2 = ref_cell(sheet, NT * 32, e, tx, ty, &bs, pal, refbuf);
    ncases++;
    if (r1 != r2 || (r1 && memcmp(dst, refbuf, sizeof refbuf) != 0)) bad_cell++;

    /* expand_tile (skip0 off) */
    uint16_t tid = (uint16_t)(rnd() % NT);
    int hf = (int)(rnd() & 1u), vf = (int)(rnd() & 1u), pb = (int)(rnd() % ROM_WP_PAL_BANKS);
    memset(outbuf, 0x5A, sizeof outbuf); memset(refbuf, 0x5A, sizeof refbuf);
    int t1 = rom_wallpaper_expand_tile(src, NT * 32, tid, hf, vf, pal[pb], dst);
    ref_px(sheet + tid * 32u, hf, vf, pal[pb], 0, refbuf);
    if (!t1 || memcmp(dst, refbuf, sizeof refbuf) != 0) bad_tile++;
  }
  CHK(bad_cell == 0, "expand_cell differs from the reference in %d of %d cases", bad_cell, ncases);
  CHK(bad_tile == 0, "expand_tile differs from the reference in %d of %d cases", bad_tile, ncases);
  printf("%d checks, %d fail%s\n", checks, fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}

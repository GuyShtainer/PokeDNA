/* source/icon4.c -- the 32x32 4bpp -> RGB15 icon expander.
 *
 *   cc -std=c11 -I source tests/host_icon4_test.c source/icon4.c -o /tmp/hi4 && /tmp/hi4
 */
#include <stdio.h>
#include <string.h>

#include "icon4.h"

static int fails = 0, checks = 0;
#define CHK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
  uint16_t pal[16];
  for (int i = 0; i < 16; i++) pal[i] = (uint16_t)(i * 2 + 1); /* distinct, nonzero-ish */

  /* refuse a short buffer / NULL args, and must not touch buf on refusal */
  uint8_t small[100];
  memset(small, 0xAA, sizeof small);
  CHK(!icon4_to_rgb15(small, sizeof small, pal), "short buffer accepted");
  CHK(small[0] == 0xAA, "short-buffer refusal still wrote to buf");
  CHK(!icon4_to_rgb15(0, ART_ICON_RGB15_BYTES, pal), "NULL buf accepted");
  uint8_t buf[ART_ICON_RGB15_BYTES];
  CHK(!icon4_to_rgb15(buf, ART_ICON_RGB15_BYTES, 0), "NULL pal accepted");

  /* An all-zero frame (index 0 everywhere) must expand to all-transparent (0). */
  memset(buf, 0, ART_ICON_RGB15_BYTES);
  CHK(icon4_to_rgb15(buf, ART_ICON_RGB15_BYTES, pal), "all-zero frame refused");
  uint16_t* out = (uint16_t*)buf;
  bool all_transparent = true;
  for (int i = 0; i < 1024; i++) if (out[i] != 0) all_transparent = false;
  CHK(all_transparent, "an all-index-0 frame produced a non-transparent pixel");

  /* A single known nonzero nibble at a known tile/byte must land at the RIGHT pixel.
   * Tile 5 (row 1, col 1 of the 4x4 grid: tx=4,ty=8), byte offset 2 within the tile
   * (row 0, the 3rd byte of the row -> pixel columns 4,5 of that row), low nibble = 7.
   * Expected linear position: row 8 (ty+0), col tx*2 + (byte_in_row*2) = 8+4=12 -> the
   * LOW nibble is the even pixel (col 12), value pal[7]|0x8000. */
  memset(buf, 0, ART_ICON_RGB15_BYTES);
  buf[5 * 32 + 2] = 0x07; /* low nibble 7, high nibble 0 */
  CHK(icon4_to_rgb15(buf, ART_ICON_RGB15_BYTES, pal), "single-pixel frame refused");
  out = (uint16_t*)buf;
  int row = 8, col = 12; /* pixel (col,row) in the 32x32 image, 0-indexed */
  uint16_t px = out[row * 32 + col];
  CHK(px == (uint16_t)(0x8000u | pal[7]), "single pixel landed at the wrong tile/offset "
      "(got %04x at [%d][%d], want %04x)", px, row, col, (uint16_t)(0x8000u | pal[7]));
  /* every other pixel in that row/col neighbourhood (and everywhere else) is 0 */
  int nonzero = 0;
  for (int i = 0; i < 1024; i++) if (out[i] != 0) nonzero++;
  CHK(nonzero == 1, "expected exactly 1 nonzero pixel, got %d", nonzero);

  /* A fully-populated frame (every nibble = its own tile*2+halfbyte index mod 15 + 1,
   * never 0) must produce zero transparent pixels and every value traceable to pal[]. */
  memset(buf, 0, ART_ICON_RGB15_BYTES);
  for (int i = 0; i < 512; i++) buf[i] = (uint8_t)(((i % 15) + 1) | (((i + 7) % 15 + 1) << 4));
  CHK(icon4_to_rgb15(buf, ART_ICON_RGB15_BYTES, pal), "dense frame refused");
  out = (uint16_t*)buf;
  int opaque = 0;
  for (int i = 0; i < 1024; i++) {
    if (out[i] == 0) continue;
    opaque++;
    CHK((out[i] & 0x8000u) != 0, "opaque pixel missing the 0x8000 bit");
    uint16_t rgb = out[i] & 0x7FFFu;
    bool found = false;
    for (int p = 1; p < 16; p++) if (pal[p] == rgb) found = true;
    CHK(found, "opaque pixel colour %04x is not any pal[1..15] entry", rgb);
  }
  CHK(opaque == 1024, "dense frame (no zero nibbles) produced %d opaque of 1024", opaque);

  printf("%d checks, %d fail%s\n", checks, fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}

/* icon4.c — see icon4.h. Pure C, mirrors rom_sprite.c's rom_sprite_to_rgb15 at 32x32
 * (16 tiles, a 4x4 tile grid) instead of 64x64 (64 tiles, 8x8). */
#include "icon4.h"

bool icon4_to_rgb15(uint8_t* buf, uint32_t cap, const uint16_t pal[16]) {
  if (!buf || !pal || cap < ART_ICON_RGB15_BYTES) return false;

  /* BACKLOG #103 (speed): the opaque value of every index once, instead of two compares and
   * two masks per byte. Index 0 stays transparent (0). 32 B on the stack. */
  uint16_t px[16];
  px[0] = 0u;
  for (uint32_t i = 1; i < 16; i++) px[i] = (uint16_t)(0x8000u | (pal[i] & 0x7FFFu));

  /* 1. de-tile the raw 512 B frame into the buffer's tail 512 bytes. A tile row is
   * 4 bytes = 8 pixels; 16 tiles arranged 4x4 for a 32x32 icon. */
  uint8_t* lin = buf + (ART_ICON_RGB15_BYTES - 512u); /* buf + 1536 */
  /* Word-wide when the buffer is 4-byte aligned (mon_decomp is aligned(4); every tile row,
   * every linear row start and every tile column offset is then a multiple of 4), byte-wise
   * otherwise (a host test's stack array). Same bytes either way. */
  const bool wide = (((uintptr_t)buf & 3u) == 0u);
  for (uint32_t t = 0; t < 16; t++) {
    uint32_t tx = (t & 3u) * 4u;  /* byte column of the tile (4 B = 8 px) */
    uint32_t ty = (t >> 2) * 8u;  /* pixel row of the tile                */
    const uint8_t* src = buf + t * 32u;
    for (uint32_t row = 0; row < 8; row++) {
      uint8_t* d = lin + (ty + row) * 16u + tx; /* linear row is 16 B (32 px / 2) */
      if (wide) {
        *(uint32_t*)(void*)d = *(const uint32_t*)(const void*)src;
      } else {
        d[0] = src[0]; d[1] = src[1]; d[2] = src[2]; d[3] = src[3];
      }
      src += 4;
    }
  }

  /* 2. expand forward, one source word (8 pixels) at a time: write 16 bytes at 16*j, read the
   * linear word at 1536+4*j. Safe to write over buf[0..2047] while still reading lin: the word's
   * write ends at 16*j+16 <= 1536+4*j+4 for every j < 128 (12*j <= 1524), i.e. it never reaches
   * a source byte the loop has not consumed yet, and the word is fully read before it is
   * written. (The byte-wise form of this proof is in the header: 3i < 1536 <=> i < 512.) */
  if (wide) {
    const uint32_t* in = (const uint32_t*)(const void*)lin;
    uint32_t* out = (uint32_t*)(void*)buf;
    for (uint32_t j = 0; j < 128u; j++) {
      uint32_t w = in[j];                         /* read BEFORE any write can reach this word */
      out[0] = (uint32_t)px[w & 15u]         | ((uint32_t)px[(w >> 4) & 15u]  << 16);
      out[1] = (uint32_t)px[(w >> 8) & 15u]  | ((uint32_t)px[(w >> 12) & 15u] << 16);
      out[2] = (uint32_t)px[(w >> 16) & 15u] | ((uint32_t)px[(w >> 20) & 15u] << 16);
      out[3] = (uint32_t)px[(w >> 24) & 15u] | ((uint32_t)px[(w >> 28) & 15u] << 16);
      out += 4;
    }
    return true;
  }
  uint16_t* out = (uint16_t*)(void*)buf;
  for (uint32_t i = 0; i < 512u; i++) {
    uint8_t v = lin[i]; /* read BEFORE any write can reach this byte */
    out[i * 2u] = px[v & 0x0Fu];
    out[i * 2u + 1u] = px[v >> 4];
  }
  return true;
}

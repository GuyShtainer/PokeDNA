/* icon4.c — see icon4.h. Pure C, mirrors rom_sprite.c's rom_sprite_to_rgb15 at 32x32
 * (16 tiles, a 4x4 tile grid) instead of 64x64 (64 tiles, 8x8). */
#include "icon4.h"

bool icon4_to_rgb15(uint8_t* buf, uint32_t cap, const uint16_t pal[16]) {
  if (!buf || !pal || cap < ART_ICON_RGB15_BYTES) return false;

  /* 1. de-tile the raw 512 B frame into the buffer's tail 512 bytes. A tile row is
   * 4 bytes = 8 pixels; 16 tiles arranged 4x4 for a 32x32 icon. */
  uint8_t* lin = buf + (ART_ICON_RGB15_BYTES - 512u); /* buf + 1536 */
  for (uint32_t t = 0; t < 16; t++) {
    uint32_t tx = (t & 3u) * 4u;  /* byte column of the tile (4 B = 8 px) */
    uint32_t ty = (t >> 2) * 8u;  /* pixel row of the tile                */
    const uint8_t* src = buf + t * 32u;
    for (uint32_t row = 0; row < 8; row++) {
      uint8_t* d = lin + (ty + row) * 16u + tx; /* linear row is 16 B (32 px / 2) */
      d[0] = src[0]; d[1] = src[1]; d[2] = src[2]; d[3] = src[3];
      src += 4;
    }
  }

  /* 2. expand forward; index 0 -> transparent, 1..15 -> 0x8000 | pal[i]. Safe to
   * write over buf[0..2047] while still reading lin (see the header's proof). */
  uint16_t* out = (uint16_t*)buf;
  for (uint32_t i = 0; i < 512u; i++) {
    uint8_t v = lin[i]; /* read BEFORE any write can reach this byte */
    uint8_t lo = (uint8_t)(v & 0x0Fu), hi = (uint8_t)(v >> 4);
    out[i * 2u] = lo ? (uint16_t)(0x8000u | (pal[lo] & 0x7FFFu)) : 0u;
    out[i * 2u + 1u] = hi ? (uint16_t)(0x8000u | (pal[hi] & 0x7FFFu)) : 0u;
  }
  return true;
}

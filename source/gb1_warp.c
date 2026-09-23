/* Gen-1 in-map teleport -- see gb1_warp.h for why this has no continueGameWarp-style
 * indirection layer and why its scope stops at the current map. Pure C, no tonc, no
 * GBA headers -- tests/host_gb1warp_test.c runs this exact code on the PC. */
#include "gb1_warp.h"

int gb1warp_check(const Gb1MapBounds* b, const Gb1Warp* w) {
  if (!b || !w) return GB1W_BAD_MAP;
  if (!b->known || b->map_id != w->map_id) return GB1W_BAD_MAP;
  if (b->width == 0 || b->height == 0) return GB1W_BAD_MAP;
  if (w->bx < 0 || w->by < 0) return GB1W_OUT_OF_BOUNDS;
  if ((uint16_t)w->bx >= b->width || (uint16_t)w->by >= b->height) return GB1W_OUT_OF_BOUNDS;
  return GB1W_OK;
}

uint8_t gb1warp_coord(int16_t block) {
  if (block < 0) block = 0;
  if (block > 127) block = 127;   /* *2 stays <= 254 -- never wraps, never hits 0xFF */
  return (uint8_t)(block * 2);
}

#define GB1W_OVERWORLD_MAP  0xC6E8u
#define GB1W_OVERWORLD_END  0xCBFCu   /* one past the game's own wOverworldMap buffer */

bool gb1warp_viewptr(uint16_t width, int16_t bx, int16_t by, uint16_t* out) {
  if (!out) return false;
  if (width == 0 || width > 128) return false;   /* rgm1_header() itself never returns
                                                    * a wider map (m1 review D2) -- an
                                                    * independent re-check anyway,
                                                    * never trusting the caller alone */
  if (bx < 0 || by < 0 || bx > 127 || by > 127) return false;

  /* viewptr = wOverworldMap + 7 + W + (W+6)*by + bx -- see this function's own
   * doc comment (gb1_warp.h) for the citation. All arithmetic in uint32_t: the
   * widest possible term is (128+6)*127 + 128 + 7 + 127, comfortably inside a
   * uint32_t, so this never wraps before the bounds check below sees it. */
  uint32_t off = 7u + (uint32_t)width + (uint32_t)(width + 6u) * (uint32_t)by + (uint32_t)bx;
  uint32_t vp = GB1W_OVERWORLD_MAP + off;
  if (vp < GB1W_OVERWORLD_MAP || vp >= GB1W_OVERWORLD_END) return false;   /* fail closed:
                                                                             * outside the
                                                                             * game's own
                                                                             * buffer */
  *out = (uint16_t)vp;
  return true;
}

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

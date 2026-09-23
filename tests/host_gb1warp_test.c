/* Host (PC) test for gb1_warp -- the Gen-1 in-map teleport writer's pure-C core.
 * See source/gb1_warp.h for why Gen 1 has no continueGameWarp-style indirection and
 * why this stays confined to the CURRENT map.
 *
 * Build + run (from the repo root):
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gb1warp_test.c source/gb1_warp.c -o /tmp/hgb1w && /tmp/hgb1w
 */
#include <stdio.h>
#include <stdint.h>
#include "gb1_warp.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } } while (0)

int main(void) {
  /* ---- 1. a normal in-bounds destination checks OK ---- */
  {
    Gb1MapBounds b = { 0x29, 10, 8, true };   /* map 0x29, 10x8 blocks */
    Gb1Warp w = { 0x29, 3, 5 };
    CHECK(gb1warp_check(&b, &w) == GB1W_OK, "in-bounds destination is GB1W_OK");
  }

  /* ---- 2. the map-id mismatch refuses -- this is NOT advisory (mirrors
   *      g3warp_check's own G3W_BAD_MAP posture: a wrong pair must never write) ---- */
  {
    Gb1MapBounds b = { 0x29, 10, 8, true };
    Gb1Warp w = { 0x2A, 3, 5 };   /* different map id than the bounds were located for */
    CHECK(gb1warp_check(&b, &w) == GB1W_BAD_MAP, "map_id mismatch is GB1W_BAD_MAP");
  }

  /* ---- 3. unknown bounds (never located) refuses ---- */
  {
    Gb1MapBounds b = { 0x29, 10, 8, false };
    Gb1Warp w = { 0x29, 3, 5 };
    CHECK(gb1warp_check(&b, &w) == GB1W_BAD_MAP, "unknown bounds is GB1W_BAD_MAP");
  }

  /* ---- 4. zero width/height (a locator that somehow produced a degenerate map)
   *      refuses rather than accept a vacuous 0..0 range ---- */
  {
    Gb1MapBounds b = { 0x29, 0, 8, true };
    Gb1Warp w = { 0x29, 0, 0 };
    CHECK(gb1warp_check(&b, &w) == GB1W_BAD_MAP, "zero width is GB1W_BAD_MAP even at (0,0)");
  }

  /* ---- 5. out-of-bounds x/y (>= width/height, and negative) both refuse, but are
   *      distinguished from GB1W_BAD_MAP -- this IS the class the UI can offer to
   *      just clamp/re-pick, unlike a bad map id which must never be retried blind ---- */
  {
    Gb1MapBounds b = { 0x29, 10, 8, true };
    Gb1Warp w1 = { 0x29, 10, 0 };    /* == width: one past the last valid column */
    Gb1Warp w2 = { 0x29, 0, 8 };     /* == height: one past the last valid row */
    Gb1Warp w3 = { 0x29, -1, 0 };
    Gb1Warp w4 = { 0x29, 0, -1 };
    CHECK(gb1warp_check(&b, &w1) == GB1W_OUT_OF_BOUNDS, "x == width refused (one past last column)");
    CHECK(gb1warp_check(&b, &w2) == GB1W_OUT_OF_BOUNDS, "y == height refused (one past last row)");
    CHECK(gb1warp_check(&b, &w3) == GB1W_OUT_OF_BOUNDS, "negative x refused");
    CHECK(gb1warp_check(&b, &w4) == GB1W_OUT_OF_BOUNDS, "negative y refused");
    /* the last in-bounds corner, one less than each cap, must be OK -- an off-by-one
     * in either direction on this check would show up here */
    Gb1Warp w5 = { 0x29, 9, 7 };
    CHECK(gb1warp_check(&b, &w5) == GB1W_OK, "the map's own last valid corner is GB1W_OK");
  }

  /* ---- 6. gb1warp_coord is the exact inverse of gbmap_block_of's `coord >> 1`
   *      (rom_gbmap.h) across the whole block range this module can be asked for,
   *      and always lands on an EVEN byte (bit 0 clear, matching wXBlockCoord/
   *      wYBlockCoord's own "coord & 1" derivation landing on 0) ---- */
  {
    for (int block = 0; block <= 127; block++) {
      uint8_t coord = gb1warp_coord((int16_t)block);
      CHECK((coord & 1) == 0, "gb1warp_coord() is always even");
      CHECK((coord >> 1) == block, "gb1warp_coord() inverts gbmap_block_of()'s >>1");
    }
  }

  /* ---- 7. gb1warp_coord clamps rather than wraps for an implausible block index
   *      (belt: gb1warp_check already refuses this upstream, but the conversion
   *      itself must never silently wrap past a byte either) ---- */
  {
    CHECK(gb1warp_coord(-5) == 0, "a negative block clamps to 0, not a huge unsigned wrap");
    CHECK(gb1warp_coord(200) == 254, "an oversized block clamps to 127*2=254, never 0xFF/wraps");
  }

  if (g_fail) { printf("host_gb1warp_test: %d FAILED\n", g_fail); return 1; }
  printf("host_gb1warp_test: all OK\n");
  return 0;
}

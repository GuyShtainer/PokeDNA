#ifndef GB1_WARP_H
#define GB1_WARP_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Gen-1 (Red/Blue/Yellow) in-map teleport -- BACKLOG #91 M3 (Gen 1 half).
 * docs/GB-MAP-DESIGN.md §7, as corrected by its "Grilling 2026-09-10" section's
 * finding A4.
 *
 * WHY THIS IS NOT gen3_warp.h'S SHAPE, EVEN THOUGH IT MIRRORS ITS INTENT:
 * Gen 3 writes `continueGameWarp` -- a SEPARATE pending-warp field the game's own
 * boot code consumes and uses to rewrite `pos`/`location` itself, because patching
 * `pos` directly would leave weather/mapLayoutId/music/mapView inconsistent and let
 * LoadSavedMapView() blit past a fixed-size buffer with no bounds check.
 *
 * Gen 1/2 has NO such indirection layer. `wCurMap`/`wXCoord`/`wYCoord` (or the Gen-2
 * equivalent) ARE the resume state -- the SAME bytes LoadMainData copies straight back
 * from SRAM into WRAM on Continue, with no separate "pending warp" concept to write
 * instead. The Grilling section's A4 finding traced the actual boot consumer
 * (Continue -> EnterMap -> LoadMapData -> LoadCurrentMapView, pokered home/
 * overworld.asm) and found it performs NO cross-field bounds validation of its own --
 * so, exactly as for Gen 3, the safety of this write comes ENTIRELY from THIS
 * module's own pre-write check, not from anything the game does on load.
 *
 * SCOPE: this module only validates/converts a destination WITHIN the map the
 * player is already on (the caller already has that map's own width/height from
 * rgm1_header(), the same locate-by-shape table Gen-1 MAP M1 already reads). A
 * cross-map teleport (BACKLOG #91's own phased plan, "M3 -- gated on M2's bounds
 * table") needs the all-maps browser (M2, unbuilt for Gen 1) to enumerate every
 * map's own bounds AND the risky-map research §7/§10 both flag as still open --
 * doing that here would be exactly the class of locator that trusts a shape it
 * has not verified. `map_id` is carried on both sides purely as a belt-and-braces
 * identity check (the caller must pass the SAME map for both bounds and warp).
 *
 * Pure C: no tonc, no FatFs, no GBA headers -- tests/host_gb1warp_test.c runs this
 * exact code on the PC.
 */

typedef struct {
  uint8_t  map_id;
  uint16_t width, height;   /* in BLOCKS -- rgm1_header()'s own out->width/height */
  bool     known;
} Gb1MapBounds;

typedef struct {
  uint8_t map_id;
  int16_t bx, by;            /* target BLOCK coordinate within that map          */
} Gb1Warp;

enum {
  GB1W_OK = 0,
  GB1W_BAD_MAP,           /* map_id mismatch, or bounds not known -- NEVER write  */
  GB1W_OUT_OF_BOUNDS,     /* bx/by outside the map's own width/height             */
};

/* Validate a destination against the CURRENT map's own bounds. Mirrors
 * g3warp_check()'s posture: an unvalidated bounds struct must never be trusted,
 * and a (map_id) mismatch between `b` and `w` is refused the same as an unknown
 * one -- the caller must always pass the SAME map's own just-located header. */
int gb1warp_check(const Gb1MapBounds* b, const Gb1Warp* w);

/* Block index -> the byte VALUE stored at wXCoord/wYCoord. One coordinate unit is
 * HALF a block (rom_gbmap.h's own gbmap_block_of(): `coord >> 1`); this is that
 * function's exact inverse (`block << 1`), so it always lands on the block's own
 * NW corner with bit 0 clear -- which is also the input tilesets.asm's own
 * `wYCoord/wXCoord & 1` derivation of wXBlockCoord/wYBlockCoord needs to land on
 * 0, the value this module's caller writes there directly rather than relying on
 * the game to re-derive it (belt: the derivation only runs on a dungeon-tileset
 * transition per that routine's own guard, not unconditionally on every load).
 * Clamped to [0,127] block first so the doubled result never reaches 0xFF (which
 * would collide with no real sentinel here, but staying inside a byte matters). */
uint8_t gb1warp_coord(int16_t block);

#endif /* GB1_WARP_H */

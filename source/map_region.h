#ifndef MAP_REGION_H
#define MAP_REGION_H

#include <stdint.h>
#include <stdbool.h>
#include "rom_map.h"
#include "map_render.h"
#include "map_gfx.h"
#include "region_map.h"

/*
 * Zoom levels 3 and 4: the game's OWN region map, read live from the user's ROM.
 *
 * The two levels are ONE data set. On RSE the region map is a single 8bpp affine
 * background, and the game's "zoomed" mode is nothing but BG2PA/PD = 0x80 (2x
 * magnification) plus a scroll, instead of 0x100. So the tiles and tilemap are uploaded
 * once and switching between the levels is four register writes.
 *
 * ---- VRAM: this DELIBERATELY reuses the tile view's zoom blocks -------------
 *   chars   14,912 B  ->  CBB2 (MGFX_VRAM_MIP_CHAR), which is 16,384 B
 *   tilemap  4,096 B  ->  the affine tilemap slot (MGFX_VRAM_AFF_MAP), exactly 4,096 B
 * There is nowhere else for them: BG VRAM is fully allocated (see map_gfx.h). The cost is
 * that entering the region view DESTROYS the Z1/Z2 mip dictionary and affine tilemap, so
 * mr_region_exit() invalidates them and the caller must rebuild on the way back down.
 *
 * ---- PALETTE ---------------------------------------------------------------
 * The region map wants all 256 BG palette entries, but the HUD's text lives in bank 15.
 * Entries 0xF0/0xF1 are therefore restored to backdrop/white after the upload so the HUD
 * stays legible; the region map loses those two indices, which is invisible in practice.
 * Coming back to the tile view calls mgfx_repaint_palettes().
 *
 * ---- DECOMPRESSION ---------------------------------------------------------
 * The ROM is a FILE on the SD card, not memory-mapped, so the compressed stream is read
 * into the arena first and then expanded with the BIOS SWI 0x12 (LZ77UnCompVram), which
 * writes VRAM 16 bits at a time. mr_lz77 must NEVER target VRAM: it writes bytes, and an
 * 8-bit store into VRAM lands in both halves of the halfword.
 */

/* Upload the region map for `rom` into VRAM. Does SD reads and BIOS decompression, so call
 * it with the screen blanked or showing a static frame, inside the same rmbl_pause window
 * as any other ROM read. `arena`/`arena_len` are borrowed for staging and are free again on
 * return. Returns false if this ROM's region map could not be located or validated — the
 * caller must then stay on the tile view rather than showing garbage. */
bool mr_region_enter(RgnMap* rg, const RomCtx* rom, uint8_t* arena, uint32_t arena_len);

/* Show it. `level` is 3 (zoomed, 2x) or 4 (whole region). `mapsec` is the player's current
 * region-map section, used to centre the zoomed view and to place the marker. */
void mr_region_show(const RgnMap* rg, int level, uint8_t mapsec);

/* Leave the region view: invalidate the mip dictionary and affine tilemap this clobbered,
 * and repaint the map's own BG palettes. Does NOT touch the tilesets in CBB0/CBB1, so the
 * tile view comes back with no SD access. */
void mr_region_exit(MapGfx* g, const MapRender* mr);

/* ---- the cursor -------------------------------------------------------------
 * Modelled on the game's own (pret/pokeemerald src/region_map.c):
 *   FULL view   - a 16x16 sprite that BLINKS between two frames every 20 frames and
 *                 GLIDES 2 px per frame over 4 frames to cross one 8 px cell.
 *   ZOOMED view - the cursor stays put on screen and the BACKGROUND scrolls instead,
 *                 1 px per frame for 8 frames per cell; the sprite is 32x32 there
 *                 because everything is at 2x.
 * map_oam is shut down while the region view is up, so this owns OBJ entirely and may
 * freely use the high sprite tiles — moam_init() regenerates its own effect tiles when
 * the tile view comes back. */
/* True once a FRLG-style TEXT region map is loaded: that family has exactly ONE view scale,
 * so the caller must not offer a zoomed level. */
bool mr_region_no_zoom(void);

/* Region maps with more than one VIEW (FRLG: Kanto + three Sevii groups). The retail game
 * switches these with a "SWITCH MAP" button; PokeDNA puts it on L, which is a dead key at
 * the region zoom level. Views share a tileset, so a switch re-uploads only the tilemap. */
int         mr_region_view_count(const RgnMap* rg);
const char* mr_region_view_label(const RgnMap* rg);
bool        mr_region_next_view(const RgnMap* rg, uint8_t* arena, uint32_t arena_len);

void mr_region_cursor_reset(const RgnMap* rg, uint8_t mapsec);
bool mr_region_cursor_move(const RgnMap* rg, int dx, int dy);   /* true if it moved */
void mr_region_tick(const RgnMap* rg, int level);               /* glide + blink + scroll */
uint8_t mr_region_cursor_mapsec(const RgnMap* rg);              /* section under it, for the HUD */

/* The region-map grid cell for a mapsec, for the "you are here" marker. False if this
 * mapsec has no cell (FRLG stores placeholder rects for dungeon sections). */
/* `out_view` (may be NULL) receives WHICH view the section was found in — needed so the
 * player marker is only drawn on that view. */
bool mr_region_cell(const RgnMap* rg, uint8_t mapsec, int* gx, int* gy, int* out_view);

#endif /* MAP_REGION_H */

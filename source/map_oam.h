#ifndef MAP_OAM_H
#define MAP_OAM_H

#include <stdint.h>
#include <stdbool.h>
#include "rom_map.h"
#include "npc_gfx.h"
#include "gen3_trainer.h"   /* PkGame */

/*
 * Hardware sprites for the overworld map screen: the NPCs standing on the map, the
 * player's character, and the "carry" animation for grab & drop.
 *
 * WHY OBJ AND NOT SOFTWARE. The map view is Mode 0/1 with the two metatile layers (and at
 * zoom an affine layer) already occupying every background. There is no spare BG to draw
 * characters on, and compositing them into the tilemap would mean rebuilding metatiles.
 * OBJ composites for free, moves by writing two halfwords per sprite per frame, and sits
 * in front of the map at any zoom level.
 *
 * ---- VRAM CONTRACT (read this before changing anything) ---------------------
 * In tile modes OBJ VRAM is 0x06010000..0x06017FFF and OBJ tile id N lives at
 * 0x06010000 + N*32. map_gfx.c parks the METATILE TABLES at 0x06010000..0x06013FFF, which
 * is exactly OBJ tile ids 0..511. So:
 *
 *     **EVERY sprite tile id used here must be >= 512.**
 *
 * That leaves ids 512..1023 = 16,384 B = 512 4bpp tiles for sprites, which matches the
 * convention the box screen already documents (box_oam.h). A 16x16 NPC is 4 tiles and the
 * worst real map has 46 objects with 24 distinct graphics ids, so the budget is ample:
 * MEASURED worst case across Emerald/Ruby/FireRed is 46 objects, 24 distinct sprites and
 * 5 distinct palettes, against 128 OAM entries and 16 OBJ palette banks.
 *
 * Sprite tiles are uploaded ONCE PER GRAPHICS ID, not once per NPC, and OAM entries share
 * them. Palettes are keyed on the game's own paletteSlot, which measurement showed is a
 * safe direct index (distinct tags and distinct slots come out equal on every map).
 *
 * ---- OS-MODE ----------------------------------------------------------------
 * moam_load_map() does SD reads (npc_gfx walks the ROM). It must be called from the same
 * blanked/paused window as mgfx_load(), never from the render loop. Everything else here
 * is pure VRAM/OAM writes and is safe to call any frame.
 */

/* Zoom levels this module understands, matching map_gfx's ladder. Below 1:1 a 16 px sprite
 * would cover two whole metatiles and a busy town turns to mush, so sprites are replaced
 * by a single small dot per NPC. */
#define MOAM_Z_FULL   0      /* 16 px per metatile: real sprites            */

/* One-time setup: locates the ROM's sprite tables (npc_gfx_open) and clears all 128 OAM
 * entries. Call once when the map screen opens, with the screen blanked. */
bool moam_init(const RomCtx* rom);

/* Turn OBJ off and hide every sprite. Call before leaving the map view; the box screen
 * re-uploads its own sprites on entry, so nothing needs restoring beyond this. */
void moam_shutdown(void);

/* Load the NPCs for one map. `events_addr` is RomMapHeader.events; `sb1` and `game` are
 * used to drop NPCs whose flag is SET, which is what the game itself does — an NPC is
 * present iff its flag is CLEAR, and flag_id 0 means always present. So the overlay shows
 * the map as THIS save would actually see it.
 *
 * RESOLVE THE VAR IDS. MEASURED: 578 of Emerald's 2,941 placed objects and 631 of Ruby's
 * 2,268 use OBJ_EVENT_GFX_VAR_0..F, which the game resolves at runtime through
 * VarGet(VAR_OBJ_GFX_ID_0 + n). VAR_OBJ_GFX_ID_0 is 0x4010 in all three games and the var
 * array is in SaveBlock1, which the caller already has. Use npc_gfx_var_index() and
 * npc_gfx_info_var(); without this a fifth of RSE NPCs are holes in the overlay.
 *
 * Returns the number of NPCs actually placed. Never fails destructively: an unresolvable
 * graphics id is simply skipped. */
int moam_load_map(const RomCtx* rom, uint32_t events_addr,
                  const uint8_t* sb1, PkGame game);

/* Reposition everything for the current camera. `cam_px`/`cam_py` are the WORLD PIXEL
 * coordinates of screen (0,0) at the current zoom — the same quantity map_gfx feeds the
 * scroll registers — and `px_per_tile` is 16 at 1:1, 8 at Z1, 4 at Z2. Sprites fully off
 * screen must be hidden (OBJ_HIDE), not merely moved: a sprite at y=200 wraps.
 *
 * At px_per_tile < 16 draw each NPC as a single 8x8 dot sprite instead of its real sprite,
 * so a crowded town stays readable. */
void moam_set_camera(int cam_px, int cam_py, int px_per_tile);

/* The NPC template under a map cell, or NULL. For the cursor detail panel. */
const RomObjectEvent* moam_npc_at(int mx, int my);

/* Drop every loaded object whose graphics id is in [lo,hi], and add one at (mx,my).
 *
 * These exist for secret-base interiors. Every group-25 map carries 15 DUMMY object-event
 * templates: localId 1 is the owner, and localIds 2..15 are 14 decoration slots with
 * graphics ids 240..253 (OBJ_EVENT_GFX_VAR_0..VAR_D) parked at meaningless coordinates
 * (0,0)..(1,6). The game hides all 14 at map load and re-spawns only the ones the save
 * actually holds, at the save's coordinates and with the graphics id read out of the
 * decoration table. Drawing the templates as they sit in the ROM therefore puts up to
 * fourteen WRONG dolls in a column against the left wall. */
void moam_drop_gfx_range(uint16_t lo, uint16_t hi);
bool moam_add_sprite(uint16_t gfx_id, int mx, int my);

/* ---- the player, and grab & drop -------------------------------------------
 * The player's own sprite comes from the same tables (graphics id 0 = Brendan walking,
 * 8 = May; the caller decides from the save's playerGender).
 *
 * CARRY ANIMATION (Guy's request, 2026-07-31): while the character is picked up it is
 * drawn in "slight distress with sweat" — held a couple of pixels above the cursor,
 * bobbing gently, with a small sweat droplet that appears near the head, slides down and
 * flicks away on a loop. On DROP a dust puff plays at the landing tile and fades.
 *
 * Both are built from tiles this module generates procedurally into its own sprite tile
 * space — do NOT rip any art from the ROM for them. A sweat drop is a few light-blue
 * pixels with a white highlight; a dust puff is three expanding pale rings. Keep them
 * small and readable at 1:1; hide them below 1:1 where they would be sub-pixel. */
bool moam_set_player(const RomCtx* rom, uint16_t gfx_id, int mx, int my);
void moam_player_at(int mx, int my);        /* move the player marker (after a drop)   */
/* Show/hide the player WITHOUT reloading their sprite. The player's coordinates are
 * MAP-LOCAL and carry no map identity, so after crossing a seam the same (x,y) names a
 * cell on the NEW map — and the character would be drawn there too, reappearing on every
 * map you walk into. The caller knows which map is loaded; it must say. */
void moam_player_visible(bool on);
/* The player's uploaded sprite, for another screen to reuse while map_oam is idle. The
 * tiles and palette bank survive moam_shutdown(), so the region map can draw a player
 * icon with no extra ROM reads. A 16x16 OBJ at this tile id shows the sprite's top half
 * (head and shoulders) under 1D mapping — which is exactly the region-map icon.
 * False if the player's sprite never resolved. */
bool moam_player_sprite(uint16_t* tid, uint8_t* pal_bank, uint8_t* h);
void moam_carry(bool carrying, int mx, int my);  /* carried => follow the cursor        */
void moam_drop_puff(int mx, int my);        /* start the dust puff at this tile         */

/* Advance the bob / sweat / puff animations. Call once per frame from the map loop, AFTER
 * moam_set_camera, and only when nothing is being read off the SD. */
void moam_tick(void);

#endif /* MAP_OAM_H */

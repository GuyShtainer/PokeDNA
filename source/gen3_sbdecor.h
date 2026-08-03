#ifndef GEN3_SBDECOR_H
#define GEN3_SBDECOR_H

#include <stdint.h>
#include <stdbool.h>
#include "rom_map.h"

/*
 * Secret-base DECORATIONS: the furniture the owner placed, composited onto the interior map.
 *
 * ---- why this is needed --------------------------------------------------------
 * A secret-base interior map in group 25 is a bare TEMPLATE — four walls, a floor and the
 * PC. Everything that makes it someone's base (posters, mats, the bench, the balls, the
 * slide) lives in that owner's 160-byte save record, not in the ROM's map data, and the
 * game paints it into the block layout at map load. Rendering the template alone shows a
 * room that is not the one the player built.
 *
 * ---- how the game does it ------------------------------------------------------
 * Each record carries 16 decoration ids at +0x12 and 16 packed positions at +0x22:
 *
 *     id  = rec[0x12 + i]                 0 = empty slot
 *     pos = rec[0x22 + i]                 x = pos >> 4, y = pos & 0x0F
 *
 * `id` indexes gDecorations, a 121-entry table of 32-byte structs in the ROM. Only three
 * fields matter here: permission (byte 17), shape (byte 18) and a pointer to the
 * decoration's metatile list (u32 at byte 28). The shape gives a width x height in
 * metatiles, and the position is the decoration's BOTTOM-LEFT cell — it grows upward, which
 * is why y runs from `pos_y - h + 1`.
 *
 * Each covered cell takes metatile `rel + (512 | overlapsWall)`, where `rel` is the u16 at
 * `tiles + 2*(row*w + col)`. 512 is the secondary-tileset base; the +1 selects the variant
 * drawn against the base's back wall, and applies only when the cell underneath has
 * behaviour MB_SECRET_BASE_NORTH_WALL and the decoration is not itself wall-mounted.
 *
 * ---- what this does NOT do ----------------------------------------------------
 * Decorations whose permission is DECORPERM_SPRITE (dolls, cushions) are not metatiles at
 * all — the game spawns them as object events. They are reported in the count so the caller
 * can say how many were skipped, and never silently dropped.
 *
 * Pure C (no tonc, no GBA headers) so it dual-compiles in tests/host_*.
 */

#define SBD_SLOTS   16      /* decoration slots per secret-base record */

typedef struct {
  uint8_t placed;           /* decorations written into the block layout        */
  uint8_t sprites;          /* skipped because they are object events, not tiles */
  uint8_t bad;              /* skipped: unknown id, unreadable tiles, off-map    */
} SbDecorStat;

/* The ROM address of gDecorations for this ROM, or 0 if this game has no such table or the
 * pinned address does not pass its signature check. Cheap: reads well under 200 bytes. */
uint32_t sbdecor_table(const RomCtx* c);

/* Composite record `rec` (160 bytes, straight out of SaveBlock1) onto `cells`, the
 * width*height block layout already read for interior map `lay`. Returns false only if the
 * decoration table could not be resolved — a base with no decorations is a success with
 * stat->placed == 0. `cells` is modified in place: the metatile id and the COLLISION bits
 * always change, and elevation changes for the two decorations the game places with
 * MapGridSetMetatileEntryAt. Collision has to be recomputed rather than inherited — keeping
 * the template's was wrong on 70.7% of decorated cells, always in the direction of calling a
 * blocked tile walkable, which is exactly the direction that would mislead a placement. */
/* `slot` is the record's index in the save's 20-entry array. Slot 0 is always the player's
 * OWN base, and the game treats every other slot as someone else's — which changes the room:
 * in a foreign base the PC becomes the REGISTER PC. Pass -1 if the slot is genuinely unknown
 * and the swap will be skipped. */
bool sbdecor_apply(const RomCtx* c, const RomLayout* lay, const uint8_t* rec, int slot,
                   uint16_t* cells, SbDecorStat* stat);

#define SBD_MAX_SPRITES 14        /* the interior templates only provide this many slots */

typedef struct { uint16_t gfx; int16_t x, y; } SbDecorSprite;

/* The sprite decorations this record places, in the game's own order and subject to the
 * game's own holder-behaviour gate. `cells` must be the layout AFTER sbdecor_apply. Returns
 * how many were written to `out` (at most SBD_MAX_SPRITES). */
int sbdecor_sprites(const RomCtx* c, const RomLayout* lay, const uint8_t* rec,
                    const uint16_t* cells, SbDecorSprite* out);

#endif /* GEN3_SBDECOR_H */

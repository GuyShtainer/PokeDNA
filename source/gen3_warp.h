#ifndef GEN3_WARP_H
#define GEN3_WARP_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_trainer.h"   /* PkGame */

/*
 * Move the player somewhere else — the save side of the map screen's "grab and
 * place your character".
 *
 * ---- why this writes continueGameWarp and NOT pos/location ------------------
 * The obvious approach (patch `pos` at SB1+0x00 and `location` at SB1+0x04) forces
 * you to keep five interdependent fields consistent (mapLayoutId, weather,
 * flashLevel, savedMusic, mapView) and, worse, the game's LoadSavedMapView() blits
 * a 15x14 block into its 10,240-u16 sBackupMapData with NO bounds check — a large
 * `pos` on a small destination map smears past that buffer.
 *
 * Instead we write only `continueGameWarp` (SB1+0x0C) and set CONTINUE_GAME_WARP in
 * `specialSaveWarpFlags` (SB2+0x09). On Continue the game boots the map exactly as
 * it was saved (self-consistent, because we changed none of it), then runs its own
 * warp code, which rewrites location/pos/mapLayoutId/weather/music/flash level and
 * respawns object events for the destination. This is literally the mechanism retail
 * Emerald uses to pull the player out of a link room after a link-battle save
 * (Task_SaveAfterLinkBattle).
 *
 * The first 0x34 bytes of SaveBlock1 are byte-identical across Ruby/Sapphire,
 * Emerald and FireRed/LeafGreen, so one implementation covers all four. Beyond 0x34
 * they diverge hard (FRLG has playerPartyCount at 0x34 where RSE has mapView), which
 * is exactly why nothing here writes past 0x33.
 *
 * Coordinates are map-local metatiles WITHOUT the +7 MAP_OFFSET; the game applies
 * MAP_OFFSET at use time.
 *
 * Pure C — host-testable.
 */

/* SaveBlock1 offsets (identical RSE/FRLG for this whole range). */
#define G3W_POS_OFF          0x00   /* Coords16 — NOT written by g3warp_apply      */
#define G3W_LOCATION_OFF     0x04   /* WarpData — NOT written                      */
#define G3W_CONTINUE_OFF     0x0C   /* WarpData — THE field we write               */
#define G3W_DYNAMIC_OFF      0x14
#define G3W_LASTHEAL_OFF     0x1C   /* white-out respawn; deliberately untouched   */
#define G3W_ESCAPE_OFF       0x24   /* Dig / Escape Rope                           */
#define G3W_WARPDATA_SIZE    8      /* s8 group, s8 num, s8 warpId, pad, s16 x, s16 y */

/* SaveBlock2 offset of specialSaveWarpFlags. Emerald/FRLG treat it as a bitfield;
 * Ruby/Sapphire compare the whole byte with == 1. */
#define G3W_SPECIAL_WARP_OFF 0x09
#define G3W_CONTINUE_BIT     0x01

/* warpId sentinel. WARP_ID_NONE is (-1) => byte 0xFF (NOT 0x7F). */
#define G3W_WARP_ID_NONE     0xFF

/* Coordinate sentinel meaning "drop me at the centre of the map"
 * (SetPlayerCoordsFromWarp falls back to width/2, height/2 when x or y is negative).
 * This is what vanilla Fly-to-a-route uses, so it is a proven-safe landing mode. */
#define G3W_CENTRE           (-1)

typedef struct {
  uint8_t group, num;
  int16_t x, y;        /* map-local metatiles, or G3W_CENTRE for the map centre */
  bool    centre;      /* true => ignore x/y and land at the map's centre       */
} G3Warp;

/* Bounds for validating a destination. The caller fills this from the ROM reader
 * (or from a shipped table); g3warp_* never guesses. */
typedef struct {
  uint8_t  group, num;
  uint16_t width, height;
  bool     known;
} G3MapBounds;

/* Why a destination was refused / flagged. */
enum {
  G3W_OK = 0,
  G3W_RISKY_MAP,        /* frontier/pyramid/link/secret-base/underwater — save state
                         * the player does not have; content may not load right     */
  G3W_NO_ESCAPE,        /* indoor map with allowEscaping == false and no warps:
                         * landing badly here can strand the save for good           */
  G3W_OUT_OF_BOUNDS,    /* x/y outside the destination map                           */
  G3W_BAD_MAP,          /* (group,num) does not exist — NEVER write this             */
};

/* Validate a destination against known bounds. Returns one of the codes above.
 * G3W_BAD_MAP is not advisory: Overworld_GetMapHeaderByGroupAndId is a raw
 * gMapGroups[group][num] dereference with ZERO bounds checking, so writing a
 * nonexistent pair produces an arbitrary ROM read and an unrecoverable boot hang. */
int  g3warp_check(const G3MapBounds* b, const G3Warp* w);

/* Is this map one whose contents are driven by save state a teleporting player
 * won't have (Battle Frontier / Pyramid / Trainer Hill / secret bases / link rooms
 * / underwater)? Emerald group numbers; returns false for other games, which get
 * the generic confirm instead. */
bool g3warp_risky_map(PkGame game, uint8_t group, uint8_t num);

/* Write the teleport. Touches EXACTLY 8 bytes of SaveBlock1 (continueGameWarp) and
 * one byte of SaveBlock2. Returns false without writing anything on bad input.
 * Commit with app_commit_sb12() — both blocks change. */
bool g3warp_apply(uint8_t* sb1, uint8_t* sb2, PkGame game, const G3Warp* w);

/* Read back what continueGameWarp currently says (for the UI / undo display). */
bool g3warp_get_pending(const uint8_t* sb1, const uint8_t* sb2, PkGame game, G3Warp* out);

/* Where the player is RIGHT NOW, from `location` + `pos`. */
bool g3warp_get_current(const uint8_t* sb1, G3Warp* out);

/* ---- undo ------------------------------------------------------------------
 * A 0x34-byte snapshot of SaveBlock1's location block plus the SaveBlock2 warp byte,
 * taken BEFORE the edit so a bad placement is always recoverable. Small enough to
 * sit in a config file next to the save. */
#define G3W_SNAPSHOT_SIZE (0x34 + 1)
void g3warp_snapshot(const uint8_t* sb1, const uint8_t* sb2, uint8_t out[G3W_SNAPSHOT_SIZE]);
bool g3warp_restore(uint8_t* sb1, uint8_t* sb2, const uint8_t snap[G3W_SNAPSHOT_SIZE]);

#endif /* GEN3_WARP_H */

#ifndef PDNA_MAP_H
#define PDNA_MAP_H

#include <stdint.h>
#include "gen3_trainer.h"   /* PkGame */

/* Overworld map screen. Reads map data live out of the user's OWN Pokemon ROM on the
 * microSD (PokeDNA ships no Nintendo map data), cross-referenced with the open save
 * for the player's position and NPC visibility flags.
 *
 * `sb1`/`sb2` are the open save's blocks (sb1 for the player position and event
 * flags; sb2 for the teleport's specialSaveWarpFlags). B returns. */
void pdna_map(uint8_t* sb1, uint8_t* sb2, PkGame game);

/* Browse the SD for a .gba (the map's own picker, arena-backed). Returns true with
 * the full path in out. Fails (false) when the arena is unavailable — i.e. unsaved
 * box moves are pending — as well as on cancel. Used by Settings > Game ROM. */
bool app_pick_rom(char* out, int out_cap);
/* The same browser filtered to Game Boy battery files (.sav/.srm) for GB import. */
bool app_pick_gb_save(char* out, int out_cap);
/* The same browser filtered to Game Boy cartridge dumps (.gb/.gbc) — the runtime source
 * of Gen-1/2 sprite art, so an imported mon can wear the art of the game it came from.
 * The extension only narrows the list; the caller must identify the ROM by its header. */
bool app_pick_gb_rom(char* out, int out_cap);

#endif /* PDNA_MAP_H */

#ifndef GEN3_BOX_H
#define GEN3_BOX_H

#include <stdint.h>
#include "gen3_mon.h"

/* PC storage (struct PokemonStorage) lives in SaveBlock1 sections 5..13,
 * reassembled contiguously. Layout (same across R/S/E/FR/LG — it's a separate
 * save block, unlike the party which moves in FRLG):
 *   0x0000 u8  currentBox
 *   0x0004 BoxPokemon boxes[14][30]   (80 bytes each, 420 total)
 *   0x8344 u8  boxNames[14][9]
 *   0x83C2 u8  boxWallpapers[14]
 */
#define G3_PC_BYTES      (9 * 3968)   /* sections 5..13 reassembled = 35712 */
#define G3_TOTAL_BOXES   14
#define G3_IN_BOX        30           /* 6 cols x 5 rows */
#define G3_BOX_WALLPAPER_COUNT 16     /* 12 scenery + 4 special, ids 0..15 */

/* Reassemble PC storage into dst (>= G3_PC_BYTES). Returns bytes written, 0 on failure. */
uint32_t gen3_read_pc_storage(const uint8_t* save, int slot, uint8_t* dst);

uint8_t  pk_current_box(const uint8_t* pc);
void     pk_box_name(const uint8_t* pc, int box, char out[12]);
uint8_t  pk_box_wallpaper(const uint8_t* pc, int box);

/* Edit a box's name (<=8 ASCII chars, Gen-3-encoded) and wallpaper id (0..15).
 * Commit via sections 5..13 (the PC-storage block). */
void     pk_set_box_name(uint8_t* pc, int box, const char* s);
void     pk_set_box_wallpaper(uint8_t* pc, int box, uint8_t wp);

/* Decode all 30 slots of `box` into out[30]; empty slots get species 0. Box mons
 * are 80 bytes (no runtime stats) so level/stats are COMPUTED (pk_resolve).
 * Returns the count of occupied slots. */
int      pk_read_box(const uint8_t* pc, int box, PkMon out[30]);

/* Fill computed level + stats (box mons) and gender (any mon) using the data
 * tables. No-op stats for party mons (they carry plaintext stats already). */
void     pk_resolve(PkMon* m);

#endif /* GEN3_BOX_H */

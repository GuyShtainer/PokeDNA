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
#define G3_BOX_WALLPAPER_FRIENDS 16   /* Emerald "Walda"/secret slot (graphic from waldaPattern) */
#define G3_WALDA_COUNT 16             /* sWaldaWallpapers[] entries */

/* Reassemble PC storage into dst (>= G3_PC_BYTES). Returns bytes written, 0 on failure. */
uint32_t gen3_read_pc_storage(const uint8_t* save, int slot, uint8_t* dst);

uint8_t  pk_current_box(const uint8_t* pc);
void     pk_box_name(const uint8_t* pc, int box, char out[12]);
uint8_t  pk_box_wallpaper(const uint8_t* pc, int box);

/* Edit a box's name (<=8 ASCII chars, Gen-3-encoded) and wallpaper id (0..15).
 * Commit via sections 5..13 (the PC-storage block). */
void     pk_set_box_name(uint8_t* pc, int box, const char* s);
void     pk_set_box_wallpaper(uint8_t* pc, int box, uint8_t wp);

/* Raw box-name access (payload / ACE use). Bypasses the Gen-3 text encoder so ALL
 * byte values 0x00..0xFF can be stored, and — unlike pk_set_box_name — does NOT
 * append a 0xFF terminator (a terminator at byte 8 would corrupt a payload that
 * spans the 9-byte field boundary). The box-name region is boxNames[14][9] = 126
 * contiguous bytes; a payload may cross field boundaries, so use the blob writer.
 * See docs/kb/pokemon/ (gen3-text-encoding.md, walk-through-walls.md). */
#define G3_BOX_NAME_BYTES   9
#define G3_BOX_NAMES_BYTES  (G3_TOTAL_BOXES * G3_BOX_NAME_BYTES)   /* 14*9 = 126 */
void     pk_get_box_name_raw(const uint8_t* pc, int box, uint8_t out[9]);
void     pk_set_box_name_raw(uint8_t* pc, int box, const uint8_t* bytes, int len);
/* Write a contiguous blob into the 126-byte box-name region starting `off` bytes
 * from box 0's first name byte (crossing 9-byte field boundaries). Clamps to the
 * region; returns bytes actually written. */
int      pk_set_box_names_blob(uint8_t* pc, int off, const uint8_t* bytes, int len);

/* Emerald "Walda" secret-wallpaper pattern (0..15) in SaveBlock1. EMERALD ONLY. */
uint8_t  pk_walda_pattern(const uint8_t* sb1);
void     pk_set_walda_pattern(uint8_t* sb1, uint8_t pattern);   /* also sets patternUnlocked */
/* The save's two Walda wallpaper colors (RGB15: [0] background, [1] foreground) —
 * the game paints the Friends patterns IN these (palette entries 1..2 of both
 * wallpaper banks are overwritten at load). EMERALD ONLY. */
void     pk_walda_colors(const uint8_t* sb1, uint16_t out[2]);

/* Decode all 30 slots of `box` into out[30]; empty slots get species 0. Box mons
 * are 80 bytes (no runtime stats) so level/stats are COMPUTED (pk_resolve).
 * Returns the count of occupied slots. */
int      pk_read_box(const uint8_t* pc, int box, PkMon out[30]);

/* Same, but decode from a flat 30*80 = 2400-byte records block (no 0x0004 header).
 * Lets a box screen render any source laid out as raw box records (e.g. the bank),
 * not just the PokemonStorage blob. */
int      pk_decode_box_raw(const uint8_t* recs, PkMon out[30]);

/* Fill computed level + stats (box mons) and gender (any mon) using the data
 * tables. No-op stats for party mons (they carry plaintext stats already). */
void     pk_resolve(PkMon* m);

#endif /* GEN3_BOX_H */

#ifndef POKEBLOCK_BG_H
#define POKEBLOCK_BG_H

#include <stdint.h>
#include "lzblob.h"   /* the art ships LZ77-paged; accessors hand out a BgFrame */

/* Retail Pokeblock Case chrome, pre-rendered on the PC.
 *
 * The art comes from the pret decompilations and is GIT-IGNORED: this header is
 * committed, but source/pokeblock_bg.c + source/pokeblock_bg_data.s + the blob in
 * data/ are generated locally by
 *
 *     python3 tools/gen_pokeblock_bg.py --stage    # copy art out of the decomps
 *     python3 tools/gen_pokeblock_bg.py
 *
 * A clone without the art links the WEAK fallbacks in pdna_main.c instead: every
 * accessor returns NULL and the screen keeps its plain list. Same pattern the bag
 * and trainer card already use.
 *
 * One frame per game, no gender axis (the case is identical for both player
 * characters in both games — checked in pokeruby/pokeemerald src/pokeblock.c), and
 * nothing at all for FRLG, which has no Pokeblocks. */

#define PB_BG_W 240
#define PB_BG_H 160

/* Screen geometry, taken from each game's src/pokeblock.c rather than measured off a
 * screenshot, so the code carries no magic pixels. RS and Emerald agree on all of it. */
#define PB_TITLE_X    16    /* item-name box: 9x2 tiles at (2,1)                    */
#define PB_TITLE_Y     8
#define PB_TITLE_W    72
#define PB_LIST_X    120    /* list panel: 14x18 tiles at (15,1)                    */
#define PB_LIST_Y      8
#define PB_LIST_W    112
#define PB_LIST_H    144
#define PB_ROW_H      16    /* 2 tiles per row                                      */
#define PB_ROWS        9    /* MAX_MENU_ITEMS                                       */
#define PB_FEEL_X     88    /* FEEL value, 2 digits right-aligned at (11,17)        */
#define PB_FEEL_Y    136

/* Flavour LABEL positions: Spicy/Dry/Sweet down the left, Bitter/Sour beside them. */
#define PB_FLAVOR_LABEL_X { 16, 16, 16, 64, 64 }
#define PB_FLAVOR_LABEL_Y { 104, 120, 136, 104, 120 }
/* The "has this flavour" 8x16 icon sits one tile left of each label. */
#define PB_FLAVOR_ICON_X  { 8, 8, 8, 56, 56 }
#define PB_FLAVOR_ICON_Y  { 104, 120, 136, 104, 120 }

/* game is a PkGame (0 = RS, 1 = EMERALD, 2 = FRLG).
 * All three return NULL for FRLG and in an art-free build. */
BgFrame pokeblock_bg(int game);                        /* 240x160 RGB15      */
const uint16_t* pokeblock_hl(int game, int state);             /* 8x8; 0 none 1 blue 2 red */
const uint16_t* pokeblock_flavor_icon(int game, int flavor);   /* 8x16; flavor 0..4  */

#endif /* POKEBLOCK_BG_H */

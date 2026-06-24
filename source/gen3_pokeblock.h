#ifndef GEN3_POKEBLOCK_H
#define GEN3_POKEBLOCK_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_trainer.h"   /* PkGame */

/* Gen-3 Pokéblock case (pure C, host-testable).
 *
 * The case is an array of POKEBLOCKS_COUNT pokéblocks inside SaveBlock1, present in
 * Ruby/Sapphire and Emerald (FireRed/LeafGreen have no contests -> no case). Each block
 * is 7 meaningful bytes (color + the 5 contest flavors + feel) but occupies an 8-byte
 * on-save stride (byte 7 is padding; leave it untouched). A slot with color == NONE(0)
 * is empty. Offsets re-derived from each game's SaveBlock1 layout (clean-room):
 *   RS 0x7F8, Emerald 0x848  (both 40 * 8 bytes). */

#define PK_POKEBLOCK_COUNT  40
#define PK_POKEBLOCK_STRIDE 8

typedef struct { uint8_t color, spicy, dry, sweet, bitter, sour, feel; } PkPokeblock;

/* SaveBlock1 byte offset of the case for `g` (0 = the game has none, e.g. FRLG). */
uint32_t pk_pokeblock_offset(PkGame g);

bool pk_pokeblock_get  (const uint8_t* sb1, PkGame g, int i, PkPokeblock* out); /* false if no case / bad i */
void pk_pokeblock_set  (uint8_t* sb1, PkGame g, int i, const PkPokeblock* in);  /* writes 7 bytes; pad preserved */
void pk_pokeblock_clear(uint8_t* sb1, PkGame g, int i);                         /* zero the whole 8-byte slot */
bool pk_pokeblock_occupied(const PkPokeblock* p);                              /* color != NONE */
const char* pk_pokeblock_color_name(uint8_t c);                               /* "None".."Gold" */

#endif /* GEN3_POKEBLOCK_H */

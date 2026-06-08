#ifndef GEN3_ITEMS_H
#define GEN3_ITEMS_H

#include <stdint.h>
#include "gen3_trainer.h"   /* PkGame */

/* Item bag pockets in SaveBlock1. Each slot is {u16 itemId, u16 quantity}; the
 * QUANTITY is XOR'd with the low 16 bits of the SaveBlock2 security key on
 * Emerald/FRLG (Ruby/Sapphire is plaintext). Offsets + capacities are per-game. */
typedef enum { POCKET_ITEMS = 0, POCKET_KEY, POCKET_BALLS, POCKET_TMHM, POCKET_BERRIES, POCKET_COUNT } PkPocket;

const char* pk_pocket_name(int pocket);
int      pk_pocket_cap(PkGame g, int pocket);
uint16_t pk_bag_item(const uint8_t* sb1, PkGame g, int pocket, int slot);
uint16_t pk_bag_qty(const uint8_t* sb1, const uint8_t* sb2, PkGame g, int pocket, int slot);
void     pk_bag_set(uint8_t* sb1, const uint8_t* sb2, PkGame g, int pocket, int slot,
                    uint16_t item_id, uint16_t qty);

#endif /* GEN3_ITEMS_H */

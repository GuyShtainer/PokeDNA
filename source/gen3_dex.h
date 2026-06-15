#ifndef GEN3_DEX_H
#define GEN3_DEX_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_trainer.h"   /* PkGame */

/* Gen-3 Pokédex flags (pure C, host-testable).
 *
 * "owned"/caught and one "seen" copy live in SaveBlock2.pokedex (pokedex @ 0x18 in
 * SB2, owned @ +0x10, seen @ +0x44 -> SB2 +0x28 / +0x5C). TWO more "seen" copies
 * live in SaveBlock1 (per game). The game keeps the three seen copies in sync and
 * cross-checks them, so a registration must set all three. Flags are indexed by
 * NATIONAL dex number - 1 (byte = idx/8, bit = idx%8); arrays are 52 bytes. */
#define G3_DEX_NAT_MAX 386

/* Set/clear a species' SEEN flag (all three copies) / OWNED flag. natDex is the
 * National Dex number (1..386); out-of-range is ignored. */
void pk_dex_set_seen (uint8_t* sb1, uint8_t* sb2, PkGame g, uint16_t natDex, bool on);
void pk_dex_set_owned(uint8_t* sb2, uint16_t natDex, bool on);

bool pk_dex_seen (const uint8_t* sb2, uint16_t natDex);
bool pk_dex_owned(const uint8_t* sb2, uint16_t natDex);

/* Count of species seen (owned=false) or owned (owned=true), 1..386. */
int  pk_dex_count(const uint8_t* sb2, bool owned);

#endif /* GEN3_DEX_H */

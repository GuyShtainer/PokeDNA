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

/* National Dex unlock. Marking species #152..386 owned does NOT make the in-game
 * dex show them: the game gates "National mode" on THREE values set together by
 * EnableNationalPokedex — a magic byte in SB2.pokedex, VAR_NATIONAL_DEX in SB1, and
 * FLAG_SYS_NATIONAL_DEX. All three (offsets + values) differ per game family:
 *   RS/Emerald: magic @ pokedex+0x02 = 0xDA ; FRLG: magic @ pokedex+0x03 = 0xB9
 *   VAR_NATIONAL_DEX = 0x302 (RS/E) / 0x6258 (FRLG) ; flag E 0x896 / RS 0x836 / FRLG 0x840
 * pk_dex_national_on reports whether all three currently match (i.e. national is live);
 * pk_dex_set_national writes/clears all three (Emerald also flips the dex view to
 * National). Edits SB2 + SB1 in place — commit both (SB2 sec0 + SB1 sec1..4). */
bool pk_dex_national_on (const uint8_t* sb1, const uint8_t* sb2, PkGame g);
void pk_dex_set_national(uint8_t* sb1, uint8_t* sb2, PkGame g, bool on);
/* BACKLOG #380: the Emerald dex SORT-ORDER byte (SaveBlock2 pokedex+0x00 = sb2[0x18]) is zeroed by a
 * locked -> unlocked pk_dex_set_national(true) (Catch ALL on a locked dex). The Undo of that bulk op
 * needs to put it back, so the screen snapshots it with the dex and restores it through these. Only
 * Emerald has the byte the setter touches: get returns 0 and set is a no-op for every other game. */
uint8_t pk_dex_order_get(const uint8_t* sb2, PkGame g);
void    pk_dex_order_set(uint8_t* sb2, PkGame g, uint8_t v);

#endif /* GEN3_DEX_H */

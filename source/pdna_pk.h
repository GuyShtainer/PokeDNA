#ifndef PDNA_PK_H
#define PDNA_PK_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_mon.h"

/* Export one Pokémon to a PKHeX-compatible .pk3 file (the 80-byte box record) in
 * the bank folder /pokedna/bank/. `rec` is the live 80/100-byte slot record;
 * only the first 80 bytes (the box form) are written. Shows the result. Returns
 * true on success. EZ-Flash-Omega-only (it's an SD write). */
bool pdna_pk_export(const uint8_t* rec, const PkMon* m);

#define PDNA_BANK_DIR "/pokedna/bank"

#endif /* PDNA_PK_H */

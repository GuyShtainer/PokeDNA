#ifndef PDNA_PK_H
#define PDNA_PK_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_mon.h"
#include "savefile.h"     /* SfStatus */

/* Export one Pokémon to a PKHeX-compatible .pk3 file (the 80-byte box record) in
 * the bank folder /PokeDNA/bank/. `rec` is the live 80/100-byte slot record;
 * only the first 80 bytes (the box form) are written. Shows the result. Returns
 * true on success. EZ-Flash-Omega-only (it's an SD write). */
bool pdna_pk_export(const uint8_t* rec, const PkMon* m);

/* Silent variant for batch "export all" (drives its own progress screen): writes
 * the .pk3 and returns the verified-write status, no UI. Fills out_path (if
 * non-NULL) with the written path on success. */
SfStatus pdna_pk_export_silent(const uint8_t* rec, const PkMon* m, char* out_path, int cap);

#define PDNA_BANK_DIR "/PokeDNA/bank"

#endif /* PDNA_PK_H */

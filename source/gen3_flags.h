#ifndef GEN3_FLAGS_H
#define GEN3_FLAGS_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_trainer.h"   /* PkGame */

/* Event flags: a plaintext bit array in SaveBlock1 (per-game base offset). These
 * drive story progress / item-collected / trainer-defeated state. Toggling story
 * or system flags can soft-lock a save, so the UI gates the raw view. */
int  pk_flags_count(PkGame g);                       /* E 6496, RS/FRLG 6400 */
bool pk_flag_get(const uint8_t* sb1, PkGame g, int n);
void pk_flag_set(uint8_t* sb1, PkGame g, int n, bool v);

#endif /* GEN3_FLAGS_H */

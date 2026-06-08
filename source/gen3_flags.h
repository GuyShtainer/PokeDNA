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

/* Curated, per-game NAMED flags for the data editor's FLAGS tab (badges + key
 * system flags). The list is ordered with category-header rows where
 * `num == NAMED_FLAG_HEADER`; real rows carry the absolute flag number for
 * pk_flag_get/set. Flag numbers (and meanings) differ per game, so the table is
 * resolved per game. Generated into data_tables.c by tools/gen_data.py. */
#define NAMED_FLAG_HEADER 0xFFFF
typedef struct { uint16_t num; const char* name; } NamedFlag;
int  pk_named_flags(PkGame g, const NamedFlag** out);   /* returns row count */

#endif /* GEN3_FLAGS_H */

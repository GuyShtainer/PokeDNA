#ifndef PDNA_FLY_H
#define PDNA_FLY_H

#include <stdint.h>
#include "gen3_trainer.h"   /* PkGame */

/* Fly-destination screen: which towns the region map will let you Fly to.
 * Viewing is free on any cart; toggling is Omega-only. `sb1` is edited in place
 * and committed via app_commit_sb1(). B returns. */
void pdna_fly(uint8_t* sb1, PkGame game);

#endif /* PDNA_FLY_H */

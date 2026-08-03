#ifndef PDNA_FRONTIER_H
#define PDNA_FRONTIER_H

#include <stdint.h>
#include "gen3_trainer.h"   /* PkGame */

/* Battle Frontier win-streak screen (Emerald), or the Battle Tower record pair
 * (Ruby/Sapphire). Viewing is free on any cart; editing is Omega-only and goes
 * through the shared verified-write path. `sb2` is edited in place and committed
 * via app_commit_sb2(). FireRed/LeafGreen have no streak block and get a message.
 * B returns. */
void pdna_frontier(uint8_t* sb1, uint8_t* sb2, PkGame game);

#endif /* PDNA_FRONTIER_H */

#ifndef PKVIEW_BOX_H
#define PKVIEW_BOX_H

#include <stdint.h>

/* Game-faithful PC box screen: a left PKMN DATA panel + a 6x5 icon grid on a
 * wallpaper, with a box-name banner. D-pad moves the cursor, L/R change box,
 * A opens the 6-card summary. `pc` is reassembled PC storage.
 * Returns 0 if the user backed out (B), or 1 to switch to the party view (SELECT). */
int pkview_box(const uint8_t* pc);

#endif /* PKVIEW_BOX_H */

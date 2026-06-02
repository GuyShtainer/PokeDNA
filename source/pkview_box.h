#ifndef PKVIEW_BOX_H
#define PKVIEW_BOX_H

#include <stdint.h>

/* Game-faithful PC box screen: a left PKMN DATA panel + a 6x5 icon grid on a
 * wallpaper, with a box-name banner. D-pad moves the cursor, L/R change box,
 * A opens the 6-card summary, B returns. `pc` is reassembled PC storage. */
void pkview_box(const uint8_t* pc);

#endif /* PKVIEW_BOX_H */

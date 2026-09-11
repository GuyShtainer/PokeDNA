#ifndef PDNA_GBMAP_H
#define PDNA_GBMAP_H

#include "gb_session.h"

/*
 * BACKLOG #91 M1 -- read-only view of the Gen-1 (Red/Blue/Yellow) CURRENT
 * map, drawn on the shared GB-screen shell (pdna_gbscreen.h) from the
 * user's own ROM, located BY SHAPE (rom_gbmap.h/.c). No connections/
 * stitching (M2), no teleport (M3): the whole current map's own Blocks
 * array is read once, the D-pad/L/R pan a 5x5-block viewport inside it
 * (clamped to the map's own bounds -- nothing past the edge is ever
 * rendered, so there is no halo to get wrong), SELECT is the shell's own
 * 1:1<->stretched toggle, B closes.
 *
 * `s` is the active resident Gen-1 GbSession (the same one Trainer/Bag
 * already read the player's own position off of via gb_fields.c's
 * GBF_MAP_ID/POS_X/POS_Y) -- this screen only READS it, never writes.
 */
void pdna_gbmap_gen1(GbSession* s);

#endif /* PDNA_GBMAP_H */

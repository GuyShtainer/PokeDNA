#ifndef PDNA_GBMAP_H
#define PDNA_GBMAP_H

#include "gb_session.h"

/*
 * BACKLOG #91 M1+M3 -- view of the Gen-1 (Red/Blue/Yellow) CURRENT map,
 * drawn on the shared GB-screen shell (pdna_gbscreen.h) from the user's own
 * ROM, located BY SHAPE (rom_gbmap.h/.c). No connections/stitching (M2,
 * unbuilt): the whole current map's own Blocks array is read once, the
 * D-pad/L/R pan a 5x5-block viewport inside it (clamped to the map's own
 * bounds -- nothing past the edge is ever rendered, so there is no halo to
 * get wrong), SELECT is the shell's own 1:1<->stretched toggle, B closes.
 *
 * M3 (teleport, `can_edit` only): A enters a place-cursor mode (D-pad moves
 * a target block, panning the viewport as needed; A confirms through a
 * dialog, B cancels back to the plain view) that writes the player's own
 * wXCoord/wYCoord (gb1_warp.h) -- confined to THIS map (a cross-map
 * teleport needs the all-maps browser, M2, per the design doc's own phased
 * plan). START restores the pre-placement position once per visit.
 *
 * `s` is the active resident Gen-1 GbSession (the same one Trainer/Bag
 * already read the player's own position off of via gb_fields.c's
 * GBF_MAP_ID/POS_X/POS_Y). `can_edit` gates M3 exactly the way every other
 * Gen-1/2 write screen on this menu is gated by its caller (pdna_gen12.c);
 * a false `can_edit` still allows the read-only M1 view.
 */
void pdna_gbmap_gen1(GbSession* s, bool can_edit);

#endif /* PDNA_GBMAP_H */

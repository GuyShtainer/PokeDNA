#ifndef PDNA_GBMAP2_H
#define PDNA_GBMAP2_H

#include "gb_session.h"

/*
 * BACKLOG #91 M1-G2 -- read-only view of the Gen-2 (Gold/Silver/Crystal)
 * CURRENT map, drawn on the shared GB-screen shell (pdna_gbscreen.h) from
 * the user's own ROM, located BY SHAPE (rom_gbmap2.h/.c). Mirrors
 * pdna_gbmap.c's Gen-1 screen closely (docs/GB-MAP-DESIGN-G2.md §9); see
 * that doc + docs/briefs/map-g2-brief.md Step 4 for the differences:
 * decompress-once (no lazy per-tile cache), the VRAM tile-id remap, the
 * whole-blockset cache, border-block fill for out-of-map cells, and the
 * wrong-game refusal guard.
 *
 * `s` is the active resident Gen-2 GbSession -- this screen only READS it,
 * never writes.
 */
void pdna_gbmap_gen2(GbSession* s);

#endif /* PDNA_GBMAP2_H */

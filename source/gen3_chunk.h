#ifndef GEN3_CHUNK_H
#define GEN3_CHUNK_H

#include <stdint.h>
#include <stdbool.h>

/* Multi-select "chunk" of box mons: a rectangular region of a 6x5 PC/Bank box,
 * lifted as a group (Emerald-style multi-move). Pure C, GRID-ONLY (no records or
 * tonc here) so tests/host_chunk_test.c dual-compiles it on the PC; the caller keeps
 * the parallel 80-byte records indexed the same as this module's mon list.
 *
 * The Emerald PC multi-move preserves the selection's rectangular FOOTPRINT: the
 * relative positions of the lifted mons are kept, and the chunk drops only where the
 * whole footprint fits over free cells. That geometry — build from a rectangle, clamp
 * the carry anchor, and test a drop — is all this module does. */

#define G3_BOX_COLS  6
#define G3_BOX_ROWS  5
#define G3_BOX_SLOTS 30                 /* == gen3_box.h G3_IN_BOX */

typedef struct {
  int     n;                            /* mons lifted (0 = nothing selected)        */
  int     h, w;                         /* footprint size in rows/cols               */
  uint8_t rr[G3_BOX_SLOTS];             /* mon i's row within the footprint (0..h-1) */
  uint8_t cc[G3_BOX_SLOTS];             /* mon i's col within the footprint (0..w-1) */
  uint8_t src[G3_BOX_SLOTS];            /* mon i's source slot index (0..29)         */
} Chunk;

static inline int g3_slot(int r, int c) { return r * G3_BOX_COLS + c; }
static inline int g3_row(int slot)      { return slot / G3_BOX_COLS; }
static inline int g3_col(int slot)      { return slot % G3_BOX_COLS; }

/* Build a chunk from the rectangle whose two (any-order) corners are slots a and b,
 * over `occupied` (1 = that slot holds a mon). Collects only OCCUPIED cells in
 * reading order, storing each mon's footprint-relative position and source slot. The
 * footprint (h,w) spans the FULL rectangle even where its border cells are empty, so
 * relative geometry is preserved on drop. Returns c->n (0 if the rectangle holds no
 * mon). */
int chunk_build(Chunk* c, const uint8_t occupied[G3_BOX_SLOTS], int a, int b);

/* Legal anchor range for the chunk's top-left so the footprint stays in-grid. */
static inline int chunk_anchor_rmax(const Chunk* c) { return G3_BOX_ROWS - c->h; }
static inline int chunk_anchor_cmax(const Chunk* c) { return G3_BOX_COLS - c->w; }

/* Can the chunk drop with its top-left at (tr,tc) into a box with occupancy `dest`?
 * `vacating` (may be NULL): a 30-entry mask of destination cells this very drop will
 * empty first (the chunk's own source cells when dropping back into the SAME box);
 * those count as free. Returns true iff the footprint is in-grid AND every target cell
 * is free-or-vacating. On success fills tgt[i] = destination slot of mon i (indexed
 * the same as the chunk's mon list). */
bool chunk_can_drop(const Chunk* c, int tr, int tc, const uint8_t dest[G3_BOX_SLOTS],
                    const uint8_t* vacating, uint8_t tgt[G3_BOX_SLOTS]);

#endif /* GEN3_CHUNK_H */

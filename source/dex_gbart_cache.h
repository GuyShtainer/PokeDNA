#ifndef DEX_GBART_CACHE_H
#define DEX_GBART_CACHE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * dex_gbart_cache.h/.c -- BACKLOG #208's pure-C core: the FIFO bookkeeping for a
 * real per-page GB-art cache over the dex grid. PURE C: <stdint.h>/<stdbool.h>
 * only, no tonc, no FatFs -- tests/host_dexgbartcache_test.c runs this exact code
 * on the PC. The GBA-only half (pdna_gbdex.c: borrowing gb12_arena_tail() for the
 * backing store, packing/unpacking pixels, driving the fetch ladder) is a thin
 * caller of the functions below; this module knows nothing about pixels, PdnaArt,
 * the GB session, or the arena.
 *
 * WHY THIS EXISTS (BACKLOG #196 review, BACKLOG #208): a Pokedex grid page is 21
 * cells (7 cols x 3 rows), painted in the SAME order on a cold entry, a one-row
 * scroll (14 of the 21 cells unchanged) and a same-page repaint (all 21 unchanged).
 * BACKLOG #196 shipped an 11-slot FIFO/LRU over that access pattern and the review
 * instrumented it at 21 MISS / 0 HIT on EVERY one of those events: a capacity
 * smaller than the page means the (N+1)th cell of a 21-cell sweep always evicts the
 * 1st, which is needed again 20 accesses later, forever -- the eviction POLICY does
 * not matter once capacity < page size (LRU has the identical pathology; see the
 * BACKLOG #208 report for the count). The fix is capacity, not policy: a plain FIFO
 * with n_slots >= 21 cannot exhibit this -- a same-page repaint asks for nothing
 * new (0 evictions), and a one-row scroll only evicts the 7 cells that just
 * scrolled off screen (list order matches paint order under the default No.-sort
 * filter). tests/host_dexgbartcache_test.c PINS both halves of this claim: 21 keys
 * over 23 slots all still hit; the SAME 21 keys over an 11-slot cache thrash
 * exactly like the pre-#208 code measured.
 *
 * KEYING. `dexcache_key(gen, dex)` packs a (generation, national-dex) pair into one
 * uint16_t (gen in bits 9-10, dex in bits 0-8 -- dex is 1..251, gen is 1 or 2) so
 * Gen-1 and Gen-2 entries can never collide even though a GB session is only ever
 * one generation at a time in practice.
 *
 * STORAGE. The caller owns the backing array (DexArtSlot[n_slots]) -- on the real
 * hardware/emulator build that memory is a borrowed slice of the GB12 arena's tail
 * (pdna_gbdex.c, gb12_arena_tail()); a host test just uses a plain C array. This
 * module only ever writes the `key` field; the `payload` bytes are the GBA-side
 * caller's business (pixel format differs per generation -- see pdna_gbdex.c's own
 * header comment above gbdex_cell_blit()).
 */

#define DEXCACHE_PAYLOAD 1024   /* Gen 1 uses all of it (1 B/px, 32x32 cell); Gen 2 uses the first 512 */

typedef struct {
  uint16_t key;                 /* dexcache_key()'s packed (gen, dex); never 0 for a real entry */
  uint8_t  payload[DEXCACHE_PAYLOAD];
} DexArtSlot;

typedef struct {
  DexArtSlot* slots;   /* caller-owned backing store, `n_slots` entries; NULL disables the cache */
  int n_slots;
  int head;            /* FIFO: the next slot index a fresh key overwrites */
  int count;           /* how many of slots[0 .. count) are valid (<= n_slots)  */
} DexArtCache;

/* (Re)start empty over a caller-owned backing store of `n_slots` DexArtSlot records.
 * `slots` may be NULL / `n_slots` may be <= 0 -- every other call below then degrades
 * to "always miss, insert is a no-op", the same "not enough memory right now"
 * posture every other gate in this codebase already has. */
void dexcache_reset(DexArtCache* c, DexArtSlot* slots, int n_slots);

uint16_t dexcache_key(uint8_t gen, uint16_t dex);

/* -1 if `key` is not currently cached, else the slot index (0 <= idx < c->count). */
int dexcache_find(const DexArtCache* c, uint16_t key);

/* Returns the slot index the caller should now (over)write with `key`'s payload:
 * an existing entry for `key` is returned UNCHANGED (no FIFO motion -- re-caching
 * the same key must not cost it its queue position); otherwise the FIFO's head slot
 * is claimed and the pointer advances circularly. Returns -1 iff the cache is
 * disabled (c->slots == NULL or c->n_slots <= 0) -- the caller must not write
 * payload bytes in that case. */
int dexcache_claim(DexArtCache* c, uint16_t key);

#endif

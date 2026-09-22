#ifndef DEX_GB_ART_CACHE_H
#define DEX_GB_ART_CACHE_H
#include <stdint.h>
#include <stdbool.h>

/*
 * dex_gb_art_cache -- BACKLOG #196's per-page GB-art cache POLICY, factored out as a
 * pure-C module (no tonc/FatFs/GBA headers, no I/O, no allocation) exactly like
 * dex_cell_art_rule.c: tests/host_dex_gb_art_cache_test.c compiles and runs this
 * exact file on the PC.
 *
 * OWNERSHIP SPLIT (deliberate, same shape icon_store.h uses for its own Tier B): this
 * module owns none of the memory it caches into. The CALLER (pdna_gbdex.c) is the
 * only thing that knows how to obtain a slice of GBA arena memory
 * (gb12_arena_tail(), a GBA-only, single-holder-per-visit resource this module has
 * no business knowing exists) and hands this module a plain pointer + a slot count;
 * every function below degrades to a safe, cheap no-op when that pointer is NULL or
 * that count is 0 (an "unopened" or "arena refused" cache), so a caller never has to
 * special-case "the cache did not fit this visit" -- every cell just goes through
 * the ordinary uncached fetch instead, exactly as it did before this feature.
 *
 * WHY 32x32, NOT the source picture's native size: the cache stores the DECODED,
 * DOWNSCALED cell a dex grid cell actually blits (pdna_origin_cell_render()'s own
 * output, RGB15 + 0x8000 opacity, 32*32 pixels = 2,048 B) -- exactly what
 * gbdex_cell_blit()'s own stack buffer already holds right before it calls
 * ui_sprite(), so caching costs one extra memcpy on a MISS and zero extra work on a
 * HIT (a cache hit skips the whole fetch chain: have()/stack-room/f_open/decode, the
 * SD read this feature exists to save).
 *
 * KEYED BY DEX NUMBER, NOT BY SCREEN POSITION: a cached cell is immutable derived
 * art (nothing in this app writes a GB ROM), so it never goes stale from a scroll,
 * a filter change or a different page turning up the same species again -- the ONLY
 * event that can make dex N's cached pixels wrong is a ROM (re)registration, which
 * changes what dex N's OWN picture even is. That is pdna_origin_art_invalidate_
 * epoch()'s job (pdna_gbdex.c polls it and calls dex_gb_art_cache_clear() when it has
 * moved); this module has no I/O and no epoch of its own to compare against.
 *
 * EVICTION: plain FIFO over a tiny fixed capacity (round-robin `next` cursor) -- no
 * LRU bookkeeping, on purpose. The capacity this feature can actually afford (see
 * pdna_gbdex.c's own sizing comment) is well under one full 21-cell page, so ANY
 * eviction policy loses some cells on a full page turn; FIFO is the simplest policy
 * that is still provably correct (see the mutation-proof host test for "no write
 * past the rented rows").
 */

#define DEX_GB_ART_CELL_PX 1024   /* 32*32 -- one decoded, downscaled dex cell */

typedef struct {
  uint16_t dex;                    /* 0 = empty slot -- never a valid national dex # */
  uint16_t px[DEX_GB_ART_CELL_PX]; /* RGB15 + 0x8000 opacity, row-major, 32x32        */
} DexGbArtSlot;

typedef struct {
  DexGbArtSlot* slot;   /* caller-owned storage, `cap` entries -- NEVER allocated or
                         * freed here (rule 3: no dynamic allocation in this module) */
  int cap;               /* 0 when `slot` is NULL: every op below is then a no-op    */
  int next;               /* FIFO eviction cursor, 0 <= next < cap (or 0 when cap==0) */
} DexGbArtCache;

/* Bind the cache to caller-owned storage and mark every slot empty. `slot`/`cap` may
 * be NULL/0 (an unopened/unavailable cache this visit) -- every other function below
 * is then a safe, cheap no-op. `c` must be non-NULL (a caller's own file-static
 * struct, never allocated here). */
void dex_gb_art_cache_open(DexGbArtCache* c, DexGbArtSlot* slot, int cap);

/* Drop every entry (mark every slot empty, reset the eviction cursor). Does NOT
 * touch storage ownership -- the caller still owns `slot` and decides separately
 * when to actually release it (gb12_arena_tail_release(), a GBA-only concern this
 * module never sees). Safe on an unopened cache (cap == 0): a no-op. */
void dex_gb_art_cache_clear(DexGbArtCache* c);

/* NULL on a miss (including an unopened cache, or dex == 0 -- 0 is never a real
 * request, see DexGbArtSlot.dex). A pointer to DEX_GB_ART_CELL_PX cached pixels on a
 * hit, valid until the next dex_gb_art_cache_put() call (the same "one live pointer"
 * discipline icon_store_row() documents, and for the same reason: the pointer is
 * INTO the shared pool, not a copy). */
const uint16_t* dex_gb_art_cache_find(const DexGbArtCache* c, uint16_t dex);

/* Store `px` (DEX_GB_ART_CELL_PX pixels, COPIED -- the caller's own buffer stays the
 * caller's to reuse the instant this returns) under `dex`, evicting the oldest entry
 * (FIFO) when the cache is already full. No-op when the cache is unopened (cap==0),
 * `px` is NULL, or `dex` is 0 (rule 7: every parameter validated, nothing written on
 * a bad one -- in particular NEVER writes to `slot[i]` for any i >= cap, the "no
 * write past the rented rows" the mutation test below pins). */
void dex_gb_art_cache_put(DexGbArtCache* c, uint16_t dex, const uint16_t px[DEX_GB_ART_CELL_PX]);

#endif /* DEX_GB_ART_CACHE_H */

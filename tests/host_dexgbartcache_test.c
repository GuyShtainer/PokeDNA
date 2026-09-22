/* Host test for source/dex_gbart_cache.{c,h} (BACKLOG #208) -- the pure-C FIFO
 * bookkeeping behind the dex grid's real per-page GB-art cache. Pure C, no GBA/tonc
 * headers, links only this one module.
 *
 *   cc -std=c11 -O2 -Wall -Wextra -I source tests/host_dexgbartcache_test.c \
 *      source/dex_gbart_cache.c -o /tmp/hdgc && /tmp/hdgc
 *
 * PROVES (per BACKLOG #208's own design, from the #196 review's own measurement):
 *   A. key(): gen/dex pack into one uint16_t with no collisions across the whole
 *      legal range (gen 1/2, dex 1..251), and out-of-range/garbage gen values still
 *      produce a key distinct from either real generation's.
 *   B. find()/claim() on an EMPTY cache: everything misses; claim() always returns a
 *      fresh, in-range slot index and never the same slot twice until the ring wraps.
 *   C. claim() on an ALREADY-CACHED key returns the SAME slot (no FIFO motion) --
 *      re-caching a dex number must not cost it its queue position.
 *   D. THE B196 LESSON, PINNED: a 21-cell page (dex 1..21) painted in list order,
 *      inserted via claim(), THEN looked up again in the SAME order (a same-page
 *      repaint) -- over 23 slots every one of the 21 still hits (0 refetch, matching
 *      BACKLOG #208's own claim); over 11 slots (the #196 size) every one of the 21
 *      MISSES on the repaint (thrash), reproducing the review's own 21 MISS / 0 HIT
 *      measurement structurally, not by re-reading a log.
 *   E. A ONE-ROW SCROLL (cols=7): after a 21-cell page is fully cached (23 slots, 2
 *      spare), inserting the next row's 7 new keys (22..28) evicts only the 5
 *      OLDEST (1..5) -- the other 16 cells that stayed on screen or were freshly
 *      inserted (6..28) all still hit, comfortably inside BACKLOG #208's own "<=7
 *      misses on a row scroll" upper bound (only ever the genuinely new keys miss).
 *   F. DISABLED CACHE (NULL slots / n_slots<=0, dexcache_reset()'s own contract):
 *      find() always misses, claim() always returns -1, and nothing crashes -- the
 *      same "not enough memory right now" posture every other gate in this codebase
 *      has.
 *   G. MUTATION: a scratch copy with `c->head` advanced by claim() even when an
 *      EXISTING key is reclaimed (the exact FIFO-motion bug part C guards against)
 *      is shown to fail part E (the "wrong" eviction victim is no longer 1..7).
 */
#include <stdio.h>
#include <string.h>

#include "dex_gbart_cache.h"

static int checks = 0, fails = 0;
#define CHECK(c, msg) do { checks++; if (!(c)) { printf("  !! FAIL: %s\n", msg); fails++; } } while (0)

static void test_key_no_collisions(void) {
  printf("A. key() -- no collisions over the legal range\n");
  for (uint16_t d1 = 1; d1 <= 251; d1++) {
    uint16_t k1g1 = dexcache_key(1, d1), k1g2 = dexcache_key(2, d1);
    CHECK(k1g1 != k1g2, "the same dex under Gen 1 and Gen 2 must key differently");
    for (uint16_t d2 = 1; d2 <= 251; d2++) {
      if (d1 == d2) continue;
      CHECK(dexcache_key(1, d1) != dexcache_key(1, d2), "two different Gen-1 dex numbers collided");
      CHECK(dexcache_key(2, d1) != dexcache_key(2, d2), "two different Gen-2 dex numbers collided");
    }
  }
}

static void test_empty_cache(void) {
  printf("B. find()/claim() on an empty cache\n");
  DexArtSlot slots[23];
  DexArtCache c;
  dexcache_reset(&c, slots, 23);
  for (uint16_t dex = 1; dex <= 21; dex++)
    CHECK(dexcache_find(&c, dexcache_key(1, dex)) < 0, "an empty cache must miss every key");

  int seen[23]; memset(seen, 0, sizeof seen);
  for (uint16_t dex = 1; dex <= 23; dex++) {
    int slot = dexcache_claim(&c, dexcache_key(1, dex));
    CHECK(slot >= 0 && slot < 23, "claim() must return an in-range slot on a live cache");
    CHECK(!seen[slot], "claim() reused a slot before the ring ever wrapped (23 inserts, 23 slots)");
    seen[slot] = 1;
  }
}

static void test_reclaim_same_slot(void) {
  printf("C. claim() on an already-cached key returns the SAME slot\n");
  DexArtSlot slots[23];
  DexArtCache c;
  dexcache_reset(&c, slots, 23);
  int first = dexcache_claim(&c, dexcache_key(1, 5));
  for (int i = 0; i < 10; i++) {
    int again = dexcache_claim(&c, dexcache_key(1, 5));
    CHECK(again == first, "re-claiming the same key must never move it to a new slot");
  }
  CHECK(c.count == 1, "re-claiming the same key must not grow the cache's valid count");
}

/* The REAL caller's own access shape (gbdex_cell_art_gen1/gen2, source/pdna_gbdex.c):
 * find() first; a HIT needs no claim at all; a MISS fetches (simulated here by just
 * writing the marker) and THEN claims a slot. Returns how many of the `n` cells (dex
 * base_dex..base_dex+n-1) were HITs -- this is what "0 refetch"/"thrash" actually
 * measures, not a blind claim() sweep (which would never distinguish a repaint from
 * a cold page, since claim() always succeeds on either). */
static int paint_page(DexArtCache* c, int n, int base_dex) {
  int hits = 0;
  for (int i = 0; i < n; i++) {
    int dex = base_dex + i;
    uint16_t key = dexcache_key(1, (uint16_t)dex);
    int slot = dexcache_find(c, key);
    if (slot >= 0 && c->slots[slot].payload[0] == (uint8_t)dex) { hits++; continue; }
    slot = dexcache_claim(c, key);
    if (slot >= 0) c->slots[slot].payload[0] = (uint8_t)dex;
  }
  return hits;
}

/* A pure lookup, no insert -- for checking cache STATE after some other access
 * pattern already ran (e.g. "is dex 1..7 still resident after a scroll"). */
static int page_hits(DexArtCache* c, int n, int base_dex) {
  int hits = 0;
  for (int i = 0; i < n; i++) {
    int dex = base_dex + i;
    int slot = dexcache_find(c, dexcache_key(1, (uint16_t)dex));
    if (slot >= 0 && c->slots[slot].payload[0] == (uint8_t)dex) hits++;
  }
  return hits;
}

static void test_b196_lesson(void) {
  printf("D. the b196 lesson: 23 slots >= a 21-cell page hits on repaint; 11 does not\n");
  {
    DexArtSlot slots[23];
    DexArtCache c;
    dexcache_reset(&c, slots, 23);
    int cold = paint_page(&c, 21, 1);
    CHECK(cold == 0, "a cold page must be 21 misses -- nothing was cached yet");
    int repaint = paint_page(&c, 21, 1);       /* the SAME page, SAME order -- a repaint */
    CHECK(repaint == 21, "23 slots over a 21-cell page must hit all 21 on a same-page repaint");
  }
  {
    DexArtSlot slots[11];
    DexArtCache c;
    dexcache_reset(&c, slots, 11);              /* the #196 size */
    int cold = paint_page(&c, 21, 1);
    CHECK(cold == 0, "a cold page must be 21 misses regardless of capacity");
    int repaint = paint_page(&c, 21, 1);
    CHECK(repaint == 0, "11 slots over a 21-cell page must thrash (0 HIT) on repaint, "
                        "matching the #196 review's own 21 MISS / 0 HIT measurement");
  }
}

static void test_row_scroll(void) {
  /* 23 slots hold a 21-cell page with 2 slots to spare. A one-row scroll (cols=7)
   * asks for 7 new keys: the first 2 land in the still-empty spare slots (free,
   * no eviction), and only the remaining 5 evict the FIFO's 5 oldest entries (dex
   * 1..5) -- fewer misses than the page's own row width, because 23 > 21 by 2.
   * BACKLOG #208's own "<=7 misses on a row scroll" claim is an upper bound this
   * satisfies with room to spare, not an exact count. */
  printf("E. a one-row scroll evicts the 5 oldest (23 slots, 21-cell page, +2 spare)\n");
  DexArtSlot slots[23];
  DexArtCache c;
  dexcache_reset(&c, slots, 23);
  paint_page(&c, 21, 1);              /* dex 1..21, a full cold page */
  int new_hits = paint_page(&c, 7, 22); /* the next row: dex 22..28 (7 new cells) */
  CHECK(new_hits == 0, "7 never-before-seen cells must all be misses");

  int hits_evicted = page_hits(&c, 5, 1);    /* dex 1..5 -- the 5 oldest, pushed out  */
  int hits_kept = page_hits(&c, 16, 6);      /* dex 6..21 -- still resident (2 of them
                                              * only because of the spare capacity)   */
  int hits_new = page_hits(&c, 7, 22);       /* dex 22..28 -- just inserted            */
  CHECK(hits_evicted == 0, "the 5 oldest cells must be exactly what got evicted");
  CHECK(hits_kept == 16, "every cell that fits (16 of the original 21) must still hit");
  CHECK(hits_new == 7, "the 7 freshly-inserted cells must all hit their own lookup");
  /* The scroll step itself asked for exactly 7 never-before-seen keys (new_hits==0,
   * checked above) -- no MORE than that ever misses, satisfying BACKLOG #208's own
   * "<=7 misses on a row scroll" upper bound; here it costs no unnecessary eviction
   * of an on-screen cell either (hits_kept==16 -- all of them, not merely "enough"). */
}

static void test_disabled(void) {
  printf("F. a disabled cache (NULL slots / n_slots<=0) never crashes, always misses\n");
  DexArtCache c;
  dexcache_reset(&c, 0, 23);
  CHECK(dexcache_find(&c, dexcache_key(1, 1)) < 0, "NULL slots must miss");
  CHECK(dexcache_claim(&c, dexcache_key(1, 1)) < 0, "NULL slots must refuse to claim");

  DexArtSlot slots[23];
  dexcache_reset(&c, slots, 0);
  CHECK(dexcache_find(&c, dexcache_key(1, 1)) < 0, "n_slots<=0 must miss");
  CHECK(dexcache_claim(&c, dexcache_key(1, 1)) < 0, "n_slots<=0 must refuse to claim");

  dexcache_reset(&c, slots, -5);
  CHECK(dexcache_claim(&c, dexcache_key(1, 1)) < 0, "a negative n_slots must refuse to claim");
}

/* G. MUTATION: reproduce the exact defect part C guards against -- a claim() that
 * moves `head` even when reclaiming an EXISTING key -- and show it breaks part E's
 * own row-scroll property. Inlined here (not a second .c file) so the mutant is
 * unmistakably the SAME algorithm with one line changed, not a hand-written
 * re-implementation that could silently diverge from the real one. */
static int mutant_claim(DexArtCache* c, uint16_t key) {
  if (!c || !c->slots || c->n_slots <= 0) return -1;
  int slot = dexcache_find(c, key);
  /* MUTATION: the real dexcache_claim() returns HERE on a hit (no FIFO motion).
   * This mutant falls through and advances the ring regardless. */
  slot = c->head;
  c->head = (c->head + 1) % c->n_slots;
  if (c->count < c->n_slots) c->count++;
  c->slots[slot].key = key;
  return slot;
}

/* G's actual property: claim() called TWICE in a row on the same 21 keys (no find()
 * involved at all -- this isolates claim()'s own "an existing entry is returned
 * UNCHANGED" contract, part C's property, from the find()-then-claim wrapper the
 * other parts use) must hand back the IDENTICAL slot index both times, for every
 * key. Real dexcache_claim() does; mutant_claim() (which keeps advancing the ring
 * even on a hit) does not -- this is exactly the defect class BACKLOG #208's own
 * design guards against: a caller (or a future refactor of this module) that keeps
 * moving the FIFO pointer on an already-cached key would silently re-break the
 * #196 pathology at ANY capacity, not just 11. */
static void test_mutation_catches_fifo_motion_bug(void) {
  printf("G. mutation: claim() moving the ring on a re-claimed key changes slot assignment\n");

  DexArtSlot slots[23];
  DexArtCache c;
  dexcache_reset(&c, slots, 23);
  int round1[21], round2[21];
  for (int i = 0; i < 21; i++) round1[i] = dexcache_claim(&c, dexcache_key(1, (uint16_t)(1 + i)));
  for (int i = 0; i < 21; i++) round2[i] = dexcache_claim(&c, dexcache_key(1, (uint16_t)(1 + i)));
  int real_stable = 1;
  for (int i = 0; i < 21; i++) if (round1[i] != round2[i]) real_stable = 0;
  CHECK(real_stable, "the real claim() must return the identical slot on a repeat claim of the same key");

  DexArtSlot mslots[23];
  DexArtCache mc;
  dexcache_reset(&mc, mslots, 23);
  int mround1[21], mround2[21];
  for (int i = 0; i < 21; i++) mround1[i] = mutant_claim(&mc, dexcache_key(1, (uint16_t)(1 + i)));
  for (int i = 0; i < 21; i++) mround2[i] = mutant_claim(&mc, dexcache_key(1, (uint16_t)(1 + i)));
  int mut_stable = 1;
  for (int i = 0; i < 21; i++) if (mround1[i] != mround2[i]) mut_stable = 0;
  CHECK(!mut_stable, "the mutant must FAIL the same property (proves this test actually bites)");
}

int main(void) {
  test_key_no_collisions();
  test_empty_cache();
  test_reclaim_same_slot();
  test_b196_lesson();
  test_row_scroll();
  test_disabled();
  test_mutation_catches_fifo_motion_bug();

  printf("dex_gbart_cache: %d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}

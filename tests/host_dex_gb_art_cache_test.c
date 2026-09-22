/* Host test for source/dex_gb_art_cache.{c,h} (BACKLOG #196) -- the pure-C per-page
 * GB-art cache policy pdna_gbdex.c's gbdex_cell_art() reads/writes through. Pure C,
 * no GBA/tonc headers, links only this one module.
 *
 *   cc -std=c11 -O2 -Wall -Wextra -I source tests/host_dex_gb_art_cache_test.c \
 *      source/dex_gb_art_cache.c -o /tmp/hdgc && /tmp/hdgc
 *
 * A "rented rows" GUARD BAND surrounds the test's own storage array on both sides
 * (canary bytes, checked at the end) so a put() that ever writes past `cap` entries
 * -- the exact class of bug an off-by-one on capacity produces -- is caught even if
 * it happens to land in this process's own unmapped/unused memory rather than
 * segfaulting outright.
 */
#include <stdio.h>
#include <string.h>

#include "dex_gb_art_cache.h"

static int checks = 0, fails = 0;
#define CHECK(c, msg) do { checks++; if (!(c)) { printf("  !! FAIL: %s\n", msg); fails++; } } while (0)

#define CAP 11
#define GUARD_WORDS 64

typedef struct {
  uint32_t guard_lo[GUARD_WORDS];
  DexGbArtSlot slots[CAP];
  uint32_t guard_hi[GUARD_WORDS];
} GuardedStorage;

static GuardedStorage g_store;

static void guard_fill(void) {
  for (int i = 0; i < GUARD_WORDS; i++) { g_store.guard_lo[i] = 0xC0DEC0DEu; g_store.guard_hi[i] = 0xC0DEC0DEu; }
}
static bool guard_intact(void) {
  for (int i = 0; i < GUARD_WORDS; i++)
    if (g_store.guard_lo[i] != 0xC0DEC0DEu || g_store.guard_hi[i] != 0xC0DEC0DEu) return false;
  return true;
}

static uint16_t g_pattern[DEX_GB_ART_CELL_PX];

static void make_pattern(uint16_t seed) {
  for (int i = 0; i < DEX_GB_ART_CELL_PX; i++) g_pattern[i] = (uint16_t)(seed * 31u + (uint16_t)i);
}

/* (A) open/find/put basics: an unopened cache always misses and put() is a no-op. */
static void test_unopened_is_noop(void) {
  DexGbArtCache c;
  dex_gb_art_cache_open(&c, 0, 0);
  CHECK(dex_gb_art_cache_find(&c, 1) == 0, "unopened cache: find always misses");
  make_pattern(1);
  dex_gb_art_cache_put(&c, 1, g_pattern);   /* must not crash, must not "work" */
  CHECK(dex_gb_art_cache_find(&c, 1) == 0, "unopened cache: put() is a no-op, still misses");
  printf("(A) unopened-is-noop ok\n");
}

/* (B) fill exactly CAP distinct entries, then confirm every one is a hit with the
 * exact pixels it was put with. */
static void test_fill_and_hit(void) {
  guard_fill();
  DexGbArtCache c;
  dex_gb_art_cache_open(&c, g_store.slots, CAP);
  for (int i = 0; i < CAP; i++) {
    make_pattern((uint16_t)(100 + i));
    dex_gb_art_cache_put(&c, (uint16_t)(1 + i), g_pattern);
  }
  for (int i = 0; i < CAP; i++) {
    make_pattern((uint16_t)(100 + i));
    const uint16_t* got = dex_gb_art_cache_find(&c, (uint16_t)(1 + i));
    char msg[64]; snprintf(msg, sizeof msg, "slot %d must hit after fill", i);
    CHECK(got != 0, msg);
    if (got) CHECK(memcmp(got, g_pattern, sizeof g_pattern) == 0, "hit pixels must match what was put");
  }
  CHECK(guard_intact(), "fill: no write outside the rented [0,CAP) slots");
  printf("(B) fill-and-hit ok\n");
}

/* (C) a species never put() is always a miss; dex==0 is always a miss (the empty
 * sentinel) even if put() is called with dex==0 (which must be rejected, not stored
 * as a wildcard-matching-everything empty slot). */
static void test_miss_and_zero_sentinel(void) {
  guard_fill();
  DexGbArtCache c;
  dex_gb_art_cache_open(&c, g_store.slots, CAP);
  make_pattern(7);
  dex_gb_art_cache_put(&c, 7, g_pattern);
  CHECK(dex_gb_art_cache_find(&c, 8) == 0, "never-put species -> miss");
  make_pattern(0);
  dex_gb_art_cache_put(&c, 0, g_pattern);         /* must be rejected */
  CHECK(dex_gb_art_cache_find(&c, 0) == 0, "dex==0 is never a valid key, find(0) always misses");
  CHECK(guard_intact(), "miss/zero: no write outside the rented [0,CAP) slots");
  printf("(C) miss-and-zero-sentinel ok\n");
}

/* (D) capacity bound / FIFO eviction: putting CAP+1 distinct species evicts exactly
 * the OLDEST one (species #1, put first) and leaves every later one resident. This
 * is the mutation test's own target: an off-by-one on capacity (e.g. looping
 * `i <= cap` instead of `i < cap` in open()/clear(), or writing slot[cap] on a full
 * cache in put()) either corrupts the guard band or evicts/keeps the wrong entry. */
static void test_capacity_bound_fifo(void) {
  guard_fill();
  DexGbArtCache c;
  dex_gb_art_cache_open(&c, g_store.slots, CAP);
  for (int i = 0; i < CAP; i++) { make_pattern((uint16_t)(1 + i)); dex_gb_art_cache_put(&c, (uint16_t)(1 + i), g_pattern); }
  /* one more, past capacity -- must evict species #1 (the oldest, FIFO) */
  make_pattern(200);
  dex_gb_art_cache_put(&c, (uint16_t)(1 + CAP), g_pattern);

  CHECK(dex_gb_art_cache_find(&c, 1) == 0, "capacity bound: the OLDEST entry (species 1) was evicted");
  for (int i = 1; i < CAP; i++) {
    char msg[64]; snprintf(msg, sizeof msg, "capacity bound: species %d must still be resident", 1 + i);
    CHECK(dex_gb_art_cache_find(&c, (uint16_t)(1 + i)) != 0, msg);
  }
  const uint16_t* newest = dex_gb_art_cache_find(&c, (uint16_t)(1 + CAP));
  CHECK(newest != 0, "capacity bound: the newest entry (species CAP+1) must be resident");
  if (newest) { make_pattern(200); CHECK(memcmp(newest, g_pattern, sizeof g_pattern) == 0, "newest entry's pixels must be the ones just put"); }
  CHECK(guard_intact(), "capacity bound: no write past the rented [0,CAP) slots even on overflow");
  printf("(D) capacity-bound-fifo ok\n");
}

/* (E) clear(): every entry becomes a miss again, guard band untouched. */
static void test_clear(void) {
  guard_fill();
  DexGbArtCache c;
  dex_gb_art_cache_open(&c, g_store.slots, CAP);
  for (int i = 0; i < CAP; i++) { make_pattern((uint16_t)(1 + i)); dex_gb_art_cache_put(&c, (uint16_t)(1 + i), g_pattern); }
  dex_gb_art_cache_clear(&c);
  for (int i = 0; i < CAP; i++) {
    char msg[64]; snprintf(msg, sizeof msg, "species %d must miss after clear()", 1 + i);
    CHECK(dex_gb_art_cache_find(&c, (uint16_t)(1 + i)) == 0, msg);
  }
  CHECK(guard_intact(), "clear: no write outside the rented [0,CAP) slots");
  printf("(E) clear ok\n");
}

/* (F) put() with a NULL pixel pointer must be a safe no-op (rule 7: validate every
 * parameter), not a crash and not a stored garbage entry. */
static void test_put_null_px(void) {
  DexGbArtCache c;
  dex_gb_art_cache_open(&c, g_store.slots, CAP);
  dex_gb_art_cache_put(&c, 9, 0);
  CHECK(dex_gb_art_cache_find(&c, 9) == 0, "put() with px==NULL must not store anything");
  printf("(F) put-null-px ok\n");
}

int main(void) {
  test_unopened_is_noop();
  test_fill_and_hit();
  test_miss_and_zero_sentinel();
  test_capacity_bound_fifo();
  test_clear();
  test_put_null_px();

  printf("\n%d checks, %d FAILED\n", checks, fails);
  return fails ? 1 : 0;
}

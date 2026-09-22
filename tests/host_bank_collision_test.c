/* Host test for source/bank_collision.{c,h} -- the pure ident32-collision scan
 * BACKLOG #168a pulled out of drop_held()'s UP branch (source/pdna_box.c) so it can
 * be exercised on a SYNTHESIZED 16-box Bank without linking pdna_box.c or
 * pdna_bank.c (both tonc-dependent). Nothing here touches the real GBA build.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_bank_collision_test.c \
 *      source/bank_collision.c -o /tmp/hbc && /tmp/hbc
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "bank_collision.h"

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { \
    g_check++; \
    if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
  } while (0)

#define NUM_BOXES 16
#define SLOTS     30
#define REC_BYTES 80

static uint8_t g_bank[NUM_BOXES][SLOTS][REC_BYTES];
static bool    g_box_present[NUM_BOXES];   /* false = "unreadable/never written" -> get_box returns NULL */

static void reset_bank(void) {
  memset(g_bank, 0, sizeof g_bank);
  for (int b = 0; b < NUM_BOXES; b++) g_box_present[b] = true;
}

/* One record with a given 8-byte ident (magic+ident32) at g_bank[box][slot]. */
static void plant(int box, int slot, uint32_t ident32) {
  uint8_t* p = g_bank[box][slot];
  memcpy(p, "GBC1", 4);
  p[4] = (uint8_t)(ident32 & 0xFF);
  p[5] = (uint8_t)((ident32 >> 8) & 0xFF);
  p[6] = (uint8_t)((ident32 >> 16) & 0xFF);
  p[7] = (uint8_t)((ident32 >> 24) & 0xFF);
}

static const uint8_t* get_box(int b, void* ctx) {
  (void)ctx;
  if (b < 0 || b >= NUM_BOXES || !g_box_present[b]) return NULL;
  return &g_bank[b][0][0];
}

static void ident8(uint8_t out[8], uint32_t ident32) {
  memcpy(out, "GBC1", 4);
  out[4] = (uint8_t)(ident32 & 0xFF);
  out[5] = (uint8_t)((ident32 >> 8) & 0xFF);
  out[6] = (uint8_t)((ident32 >> 16) & 0xFF);
  out[7] = (uint8_t)((ident32 >> 24) & 0xFF);
}

int main(void) {
  /* 1) No collision anywhere -> false, out params untouched. */
  {
    reset_bank();
    uint8_t id[8]; ident8(id, 0xAABBCCDD);
    int ob = -7, os = -7;
    bool hit = bank_ident32_collision(get_box, NULL, NUM_BOXES, SLOTS, /*self*/ 3, 5, id, &ob, &os);
    CHECK(!hit, "empty bank falsely reports a collision");
    CHECK(ob == -7 && os == -7, "out params touched on a no-collision result");
  }

  /* 2) BACKLOG #168's own scenario: the colliding cell sits in box 9, the drop is
   * landing in box 2 slot 4 -> refusal must name box 9 (not the destination box). */
  {
    reset_bank();
    uint32_t ident = 0x12345678;
    plant(9, 17, ident);
    uint8_t id[8]; ident8(id, ident);
    int ob = -1, os = -1;
    bool hit = bank_ident32_collision(get_box, NULL, NUM_BOXES, SLOTS, /*self*/ 2, 4, id, &ob, &os);
    CHECK(hit, "collision in box 9 not found");
    CHECK(ob == 9, "wrong box reported: got %d want 9", ob);
    CHECK(os == 17, "wrong slot reported: got %d want 17", os);
  }

  /* 3) A collision in the SAME box as the destination, at a different slot, is
   * still caught. */
  {
    reset_bank();
    uint32_t ident = 0x00000001;
    plant(2, 11, ident);
    uint8_t id[8]; ident8(id, ident);
    int ob = -1, os = -1;
    bool hit = bank_ident32_collision(get_box, NULL, NUM_BOXES, SLOTS, /*self*/ 2, 4, id, &ob, &os);
    CHECK(hit, "same-box collision not found");
    CHECK(ob == 2 && os == 11, "wrong same-box location: got box %d slot %d want 2/11", ob, os);
  }

  /* 4) The destination cell itself is excluded (comparing a cell to itself is not
   * a collision -- this is the in-place "same serial, same cell" case). */
  {
    reset_bank();
    uint32_t ident = 0x99999999;
    plant(2, 4, ident);   /* self_box=2 self_slot=4 below */
    uint8_t id[8]; ident8(id, ident);
    int ob = -1, os = -1;
    bool hit = bank_ident32_collision(get_box, NULL, NUM_BOXES, SLOTS, /*self*/ 2, 4, id, &ob, &os);
    CHECK(!hit, "self cell wrongly reported as its own collision");
  }

  /* 5) An unreadable box (get_box returns NULL) is skipped, not treated as an
   * error -- a collision planted "inside" a NULL box must never be seen (the plant
   * call below writes into g_bank directly; get_box must still return NULL for it,
   * proving the scan trusts the getter, not the backing array). */
  {
    reset_bank();
    uint32_t ident = 0x77777777;
    plant(5, 0, ident);
    g_box_present[5] = false;
    uint8_t id[8]; ident8(id, ident);
    int ob = -1, os = -1;
    bool hit = bank_ident32_collision(get_box, NULL, NUM_BOXES, SLOTS, /*self*/ 2, 4, id, &ob, &os);
    CHECK(!hit, "an unreadable box was scanned anyway");
  }

  /* 6) Bound mutation pin (BACKLOG #168's own explicit pin request): a scan whose
   * num_boxes is clamped to 1 must MISS the box-9 collision that test (2) proves a
   * real 16-box scan catches -- this is the "mutate the loop bound to 1 -> the test
   * fails" check, run against the REAL bank_ident32_collision() by handing it a
   * mutated bound directly (no separate mutant binary needed: num_boxes is a plain
   * parameter, so calling with 1 here IS the mutation). */
  {
    reset_bank();
    uint32_t ident = 0x12345678;
    plant(9, 17, ident);
    uint8_t id[8]; ident8(id, ident);
    int ob = -1, os = -1;
    bool hit = bank_ident32_collision(get_box, NULL, /*num_boxes*/ 1, SLOTS, /*self*/ 2, 4, id, &ob, &os);
    CHECK(!hit, "a 1-box scan must NOT find the box-9 collision (bound not exercised)");
  }

  printf("bank_collision: %d checks, %d failed\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}

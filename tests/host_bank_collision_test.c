/* Host test for source/bank_collision.{c,h} -- the pure ident32-collision scan
 * BACKLOG #168a pulled out of drop_held()'s UP branch (source/pdna_box.c) so it can
 * be exercised on a SYNTHESIZED 16-box Bank without linking pdna_box.c or
 * pdna_bank.c (both tonc-dependent). Nothing here touches the real GBA build.
 *
 * BACKLOG #223 adds bank_serial_max() coverage at the bottom (the pure walker a
 * stale-serial resync needs; pdna_bank_serial_resync() itself is not host-buildable,
 * same class of gap as gbpc_restore_up -- see tests/host_escape_gate_sites_test.py's
 * structural check (am)/MUT AM for that half). Needs a REAL bc_pack()ed cell
 * (bank_serial_max only counts bc_is_native() slots, which recomputes bc_ident32()),
 * so tests/gen12_fixture.c supplies a synthetic Gen-1 save image in memory -- Guy owns
 * no Gen-1/2 saves, same reasoning as host_gen12_test.c's own header comment.
 *
 *   cc -std=c11 -Wall -Wextra -I source -I tests tests/host_bank_collision_test.c \
 *      source/bank_collision.c source/bank_cell.c source/gb_edit.c source/gen1_save.c \
 *      source/gen2_save.c source/data_tables.c tests/gen12_fixture.c -o /tmp/hbc && /tmp/hbc
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "bank_collision.h"
#include "bank_cell.h"
#include "gb_edit.h"
#include "gen1_save.h"
#include "gen12_fixture.h"

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

/* BACKLOG #168a review D1: models the REAL bug in pdna_box.c's old bank_scan_get
 * composition -- one shared 2,400-B box buffer, re-paged by every peek() call, the
 * exact contract pdna_bank_peek_box() has. `shared_page(b)` is the reader every
 * OTHER box goes through (mirrors pdna_bank_peek_box). `old_composed_get(b, ctx)`
 * is the BUGGY getter drop_held_up used to build: for self_box it handed back a
 * pointer captured BEFORE the scan started instead of re-paging, so by the time the
 * loop reaches self_box the shared buffer has already been overwritten by whatever
 * box was paged last (self_box - 1, in box order 0..num_boxes-1) -- self_box's own
 * data is never actually read unless self_box is 0. `fixed_get(b, ctx)` is the
 * shipped bank_scan_get(): pages every box, self included, through the one reader. */
static uint8_t s_shared_buf[SLOTS][REC_BYTES];
static const uint8_t* shared_page(int b) {
  if (b < 0 || b >= NUM_BOXES || !g_box_present[b]) return NULL;
  memcpy(s_shared_buf, g_bank[b], sizeof s_shared_buf);
  return &s_shared_buf[0][0];
}
typedef struct { const uint8_t* recs; int self_box; } OldScanCtx;
static const uint8_t* old_composed_get(int b, void* ctx) {
  const OldScanCtx* c = (const OldScanCtx*)ctx;
  return (b == c->self_box) ? c->recs : shared_page(b);
}
static const uint8_t* fixed_get(int b, void* ctx) {
  (void)ctx;
  return shared_page(b);
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

  /* 7) BACKLOG #168a review D1, TEST WITH TEETH: same shared-buffer discipline the
   * real GBA build uses (a single 2,400-B box buffer re-paged by every read), driven
   * through both getter compositions. The old composed getter must MISS a duplicate
   * planted in self_box itself (it never actually re-reads self_box's real bytes
   * unless self_box == 0); the fixed getter must REFUSE every time. Four cases: the
   * reviewer's primary scenario (self_box=9, duplicate in box 9) plus three more
   * (dest 2/dup 2 slot 11, dest 9/dup 9, dest 15/dup 15 slot 29). */
  {
    struct { int self_box, self_slot, dup_box, dup_slot; } cases[] = {
      { 9,  5, 9,  17 },   /* reviewer's primary scenario */
      { 2,  3, 2,  11 },   /* dest 2 / dup 2 slot 11 */
      { 9,  5, 9,  17 },   /* dest 9 / dup 9 (restated) */
      { 15, 0, 15, 29 },   /* dest 15 / dup 15 slot 29 */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
      int self_box = cases[i].self_box, self_slot = cases[i].self_slot;
      int dup_box = cases[i].dup_box, dup_slot = cases[i].dup_slot;
      reset_bank();
      uint32_t ident = 0xC0FFEE00u + (uint32_t)i;
      plant(dup_box, dup_slot, ident);
      uint8_t id[8]; ident8(id, ident);

      /* Old composed getter: capture `recs` the way drop_held_up used to, via a
       * page-in of self_box BEFORE the scan (buffer holds self_box's real data at
       * this instant -- the bug is that this snapshot goes stale as soon as the
       * scan pages any other box). */
      const uint8_t* recs = shared_page(self_box);
      OldScanCtx octx = { recs, self_box };
      int ob = -1, os = -1;
      bool old_hit = bank_ident32_collision(old_composed_get, &octx, NUM_BOXES, SLOTS,
                                             self_box, self_slot, id, &ob, &os);
      CHECK(!old_hit, "case %zu: old getter composition unexpectedly found the box-%d collision (bug should hide it unless self_box==0)", i, dup_box);

      /* Fixed getter: pages every box, self included, through the one reader. */
      int fb = -1, fs = -1;
      bool new_hit = bank_ident32_collision(fixed_get, NULL, NUM_BOXES, SLOTS,
                                             self_box, self_slot, id, &fb, &fs);
      CHECK(new_hit, "case %zu: fixed getter failed to find the box-%d collision", i, dup_box);
      CHECK(fb == dup_box, "case %zu: fixed getter named the wrong box: got %d want %d", i, fb, dup_box);
      CHECK(fs == dup_slot, "case %zu: fixed getter named the wrong slot: got %d want %d", i, fs, dup_slot);
    }
  }

  /* ---- BACKLOG #223: bank_serial_max() over a synthesized Bank of REAL native cells ---- */
  {
    static uint8_t img[GBF_MAX_BYTES];
    uint32_t ilen = gbf_build(GBF_RBY, img, 0);
    Gen1Save s;
    CHECK(gen1_open(img, ilen, &s) == GEN1_OK, "223: the synthetic Gen-1 fixture opens");
    GbEditMon mon;
    bool found = false;
    for (int box = 0; box <= GEN1_PARTY_BOX && !found; box++) {
      uint32_t off = gen1_list_offset(&s, box);
      int count = gen1_list_count(img + off, box);
      if (count < 0) continue;
      for (int slot = 0; slot < count; slot++) {
        if (!gb_load(&mon, GB_GEN1, img + off, box, slot)) continue;
        found = true;
        break;
      }
    }
    CHECK(found, "223: the fixture yields at least one loadable Gen-1 record");

    if (found) {
      /* 8) Empty bank -> 0 (no box holds a native cell -- 0 is never a real
       * allocated bank_serial, see bank_serial_max()'s own doc comment). */
      reset_bank();
      CHECK(bank_serial_max(get_box, NULL, NUM_BOXES, SLOTS) == 0,
            "223: an empty bank must report max serial 0");

      /* 9) One native cell in box 3 slot 5 with serial 42 -> max is exactly 42. */
      reset_bank();
      uint8_t cell1[BC_CELL_BYTES];
      CHECK(bc_pack(&mon, 0, BC_ORIGIN_RED, 0, 42u, cell1) == 0, "223: bc_pack serial 42");
      memcpy(g_bank[3][5], cell1, BC_CELL_BYTES);
      CHECK(bank_serial_max(get_box, NULL, NUM_BOXES, SLOTS) == 42u,
            "223: a single planted native cell's own serial is the max");

      /* 10) A SECOND, higher serial in a DIFFERENT box (15) must win, regardless of
       * box order -- the walker must not stop at the first native cell found. */
      uint8_t cell2[BC_CELL_BYTES];
      CHECK(bc_pack(&mon, 0, BC_ORIGIN_RED, 0, 999u, cell2) == 0, "223: bc_pack serial 999");
      memcpy(g_bank[15][29], cell2, BC_CELL_BYTES);
      CHECK(bank_serial_max(get_box, NULL, NUM_BOXES, SLOTS) == 999u,
            "223: the higher serial in a later box wins over the earlier, lower one");

      /* 11) A THIRD, LOWER serial added afterward must not lower the max. */
      uint8_t cell3[BC_CELL_BYTES];
      CHECK(bc_pack(&mon, 0, BC_ORIGIN_RED, 0, 7u, cell3) == 0, "223: bc_pack serial 7");
      memcpy(g_bank[0][0], cell3, BC_CELL_BYTES);
      CHECK(bank_serial_max(get_box, NULL, NUM_BOXES, SLOTS) == 999u,
            "223: a lower serial elsewhere does not pull the max down");

      /* 11b) BACKLOG #223 review D6: the reviewer's own mutant -- `if (serial > max)
       * max = serial;` weakened to an unconditional `max = serial;` -- survives every
       * case above unnoticed, because (9)/(10) always leave the HIGHEST serial (999)
       * in the LAST slot the walker visits (box 15 slot 29), so an unconditional
       * last-write-wins assignment happens to land on the same answer a correct
       * max-tracking walk would. This case inverts that: the HIGHER serial (999) sits
       * in an EARLIER box (3, slot 5), the LOWER serial (7) sits in the LAST slot of
       * the LAST box (15, slot 29) -- the unconditional mutant ends the scan having
       * just overwritten max with 7, while the real `>` comparison correctly keeps
       * 999. */
      reset_bank();
      uint8_t cell11b_hi[BC_CELL_BYTES];
      CHECK(bc_pack(&mon, 0, BC_ORIGIN_RED, 0, 999u, cell11b_hi) == 0, "223 (11b): bc_pack serial 999");
      memcpy(g_bank[3][5], cell11b_hi, BC_CELL_BYTES);
      uint8_t cell11b_lo[BC_CELL_BYTES];
      CHECK(bc_pack(&mon, 0, BC_ORIGIN_RED, 0, 7u, cell11b_lo) == 0, "223 (11b): bc_pack serial 7");
      memcpy(g_bank[NUM_BOXES - 1][SLOTS - 1], cell11b_lo, BC_CELL_BYTES);
      CHECK(bank_serial_max(get_box, NULL, NUM_BOXES, SLOTS) == 999u,
            "223 (11b): an earlier-box higher serial (999) beats a lower one in the "
            "LAST slot of the LAST box (7) -- kills the `max = serial;` mutant");

      /* 12) A non-native slot (bank_ident32_collision's own plant() -- an arbitrary
       * "GBC1"+ident32 with no valid bc_ident32() hash over the rest of the record)
       * must be IGNORED, never misread as a serial -- bc_is_native() is the gate.
       * The fake record's OWN bank_serial bytes are set to a large, deliberately
       * non-zero value (BC_OFF_BANK_SERIAL, not just left at plant()'s zero fill) so
       * a mutant that drops the bc_is_native() filter reads a WRONG, non-zero max
       * here instead of coincidentally landing on 0 anyway. */
      reset_bank();
      plant(4, 4, 0xDEADBEEFu);   /* magic + ident32 only, fails bc_is_native()'s hash check */
      g_bank[4][4][BC_OFF_BANK_SERIAL + 0] = 0x78;
      g_bank[4][4][BC_OFF_BANK_SERIAL + 1] = 0x56;
      g_bank[4][4][BC_OFF_BANK_SERIAL + 2] = 0x34;
      g_bank[4][4][BC_OFF_BANK_SERIAL + 3] = 0x12;   /* 0x12345678 if misread */
      CHECK(bank_serial_max(get_box, NULL, NUM_BOXES, SLOTS) == 0,
            "223: a non-native (fake-ident-only) slot must not be read as a real serial");

      /* 13) An unreadable box (get_box returns NULL) is skipped, same contract as
       * bank_ident32_collision's own case (5) above. */
      reset_bank();
      uint8_t cell4[BC_CELL_BYTES];
      CHECK(bc_pack(&mon, 0, BC_ORIGIN_RED, 0, 5000u, cell4) == 0, "223: bc_pack serial 5000");
      memcpy(g_bank[6][0], cell4, BC_CELL_BYTES);
      g_box_present[6] = false;
      CHECK(bank_serial_max(get_box, NULL, NUM_BOXES, SLOTS) == 0,
            "223: an unreadable box's own native cell must not be counted");

      /* 14) Bound mutation pin, same style as case (6) above: a scan clamped to
       * num_boxes=1 must MISS the box-15 serial 999 case (9)+(10) prove a real
       * 16-box scan finds -- calling with 1 here IS the mutation. */
      reset_bank();
      memcpy(g_bank[3][5], cell1, BC_CELL_BYTES);    /* serial 42 */
      memcpy(g_bank[15][29], cell2, BC_CELL_BYTES);  /* serial 999 */
      CHECK(bank_serial_max(get_box, NULL, /*num_boxes*/ 1, SLOTS) == 0,
            "223: a 1-box scan must NOT find box 3's or box 15's serial (bound not exercised)");
    }
  }

  printf("bank_collision: %d checks, %d failed\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}

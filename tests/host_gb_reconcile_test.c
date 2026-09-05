/* Host test for source/gb_reconcile.c -- the identity-match core behind S5-C Part B2's
 * "reconcile on load" (docs/GEN3-TO-GB-SIDECAR-DESIGN.md section 12): does a sidecar
 * entry's captured 80-byte original identify EXACTLY ONE mon in the CURRENT save, so
 * gb_reconcile_on_load() (source/pdna_main.c) knows which slot to release.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gb_reconcile_test.c \
 *      source/gb_reconcile.c source/gen3_clip.c source/gen3_box.c source/gen3_mon.c \
 *      source/gen3_save.c source/gen3_edit.c source/gen3_daycare.c \
 *      source/data_tables.c -o /tmp/hgbrec
 *   /tmp/hgbrec /Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/ (.sav files)
 *
 * Over Guy's real 5-game corpus:
 *   1) every occupied party slot's own first-8-bytes identity matches itself exactly
 *      once, at the right (*where_box == -1, *where_slot == party index);
 *   2) every occupied PC slot's own identity likewise matches itself exactly once, at
 *      the right (*where_box, *where_slot);
 *   3) a random (non-existent) identity matches 0;
 *   4) an all-zero identity (the canonical empty-slot value) matches 0 even though
 *      every unoccupied slot in the save IS literally all-zero -- the degenerate case
 *      the header comment calls out explicitly;
 *   5) a CLONE -- a party mon's record duplicated into a free PC box slot, in a RAM
 *      copy of this save's PC storage -- matches 2, and where_box/where_slot are
 *      left at -1/-1 (refuses to guess which of the two is "the" one).
 * A missing corpus SKIPs rather than fails (same posture as every other Gen-3 host
 * test in this tree).
 *
 * S5-C REVIEW (2026-09-05): gb_reconcile_plan() -- the ordering/dedupe/re-verify
 * fix for the data-loss bug the old inline pdna_main.c version had (two sidecar
 * entries resolving to the SAME slot -- the same Gen-3 mon transferred to a Gen-1
 * save AND a Gen-2 save, or re-transferred after a merge-up -- used to release the
 * first fine and then blindly release "the same recorded slot" again for the
 * second, deleting a DIFFERENT, innocent mon once the first release had shifted the
 * party down). Synthetic (no corpus needed -- these are exact, deterministic
 * scenarios, not spot checks over real data):
 *   6) a party of 6 with TWO hits both recording slot 1 -> exactly ONE PHYSICAL
 *      release (party_count drops by one, never two), the neighbour (originally
 *      slot 2) survives and shifts into slot 1, and BOTH hits end up `released`
 *      (claimable) even though only the first one actually touched the party;
 *   7) two PC hits both recording the same (box, slot) -> the slot ends up cleared
 *      exactly once, and both hits end up `released` for the same reason as (6);
 *   8) the party floor (a lone party member is never released, even when a hit
 *      targets it) is respected;
 *   9) a 3-hit party batch releases HIGHEST SLOT FIRST regardless of the hits'
 *      array order -- proven by the SURVIVING member being the one order-agnostic
 *      low-to-high release would have gotten wrong.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "gb_reconcile.h"
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "gen3_clip.h"

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { \
    g_check++; \
    if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
  } while (0)

static void test_save(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  SKIP %s (not present)\n", path); return; }
  static uint8_t save[G3_SAVE_FILE_SIZE];
  size_t n = fread(save, 1, sizeof save, f);
  fclose(f);

  Gen3SaveInfo info;
  if (!gen3_parse(save, (uint32_t)n, &info) || !info.valid) {
    printf("  SKIP %s (does not parse)\n", path);
    return;
  }

  static uint8_t sb1[G3_SAVEBLOCK1_BYTES];
  CHECK(gen3_read_saveblock1(save, info.slot, sb1) == G3_SAVEBLOCK1_BYTES,
        "%s: gen3_read_saveblock1", path);

  bool frlg = false;
  PkMon party[6];
  int nparty = pk_read_party_auto(sb1, party, &frlg);
  CHECK(nparty >= 1, "%s: at least one party mon", path);

  static uint8_t pc[G3_PC_BYTES];
  bool have_pc = gen3_read_pc_storage(save, info.slot, pc) == G3_PC_BYTES;

  printf("  %s: party=%d frlg=%d have_pc=%d\n", path, nparty, (int)frlg, (int)have_pc);

  /* 1) every party slot matches itself exactly once. */
  for (int i = 0; i < nparty; i++) {
    const uint8_t* rec = pk_party_slot(sb1, frlg, i);
    int wb = -99, ws = -99;
    int cnt = gb_reconcile_match(sb1, frlg, have_pc ? pc : NULL, rec, &wb, &ws);
    /* >= 1, not == 1: see the PC loop below for why a shared identity (Ninjask ->
     * Shedinja, or a real clone) is tolerated rather than assumed impossible. */
    CHECK(cnt >= 1, "%s: party[%d] matches at least itself (got %d)", path, i, cnt);
    if (cnt == 1)
      CHECK(wb == -1 && ws == i, "%s: party[%d] resolves to (box=-1, slot=%d) (got box=%d slot=%d)",
            path, i, i, wb, ws);
    else if (cnt > 1)
      CHECK(wb == -1 && ws == -1,
            "%s: party[%d] shares an identity with %d other slot(s); refuses to guess",
            path, i, cnt - 1);
  }

  /* 2) every occupied PC slot matches itself exactly once. An "occupied" slot here is
   * simply one whose own first 8 bytes are non-zero -- the same all-zero-is-empty
   * convention gb_reconcile.c itself uses, so no separate decode is needed to know
   * which slots are worth asking about. */
  if (have_pc) {
    int pc_checked = 0, pc_dupes = 0;
    for (int b = 0; b < G3_TOTAL_BOXES && pc_checked < 60; b++) {
      for (int s = 0; s < G3_IN_BOX && pc_checked < 60; s++) {
        const uint8_t* rec = pk_box_slot(pc, b, s);
        bool zero = true;
        for (int k = 0; k < 8; k++) if (rec[k]) { zero = false; break; }
        if (zero) continue;
        pc_checked++;
        int wb = -99, ws = -99;
        int cnt = gb_reconcile_match(sb1, frlg, pc, rec, &wb, &ws);
        /* A REAL Gen-3 save can legitimately hold two records sharing PID+otId --
         * Ninjask -> Shedinja is the documented case (Shedinja is created with its
         * parent Ninjask's exact personality and OT id), and Guy's own corpus turned
         * up at least one other pair too (found live by this test's first run, not
         * assumed up front -- exactly the "a cloned Gen-3 mon" case the design doc
         * names as a reason to refuse rather than guess). So this only requires the
         * slot find ITSELF (cnt >= 1, never 0 -- a record always matches its own
         * bytes); cnt == 1 is checked strictly for where it resolves, and cnt > 1 is
         * counted and confirmed to leave where_box/where_slot at -1/-1 rather than
         * asserted away. */
        CHECK(cnt >= 1, "%s: PC[%d][%d] matches at least itself (got %d)", path, b, s, cnt);
        if (cnt == 1)
          CHECK(wb == b && ws == s,
                "%s: PC[%d][%d] resolves to itself (got box=%d slot=%d)", path, b, s, wb, ws);
        else if (cnt > 1) {
          pc_dupes++;
          CHECK(wb == -1 && ws == -1,
                "%s: PC[%d][%d] shares an identity with %d other slot(s); refuses to guess",
                path, b, s, cnt - 1);
        }
      }
    }
    CHECK(pc_checked > 0, "%s: at least one occupied PC slot examined", path);
    if (pc_dupes) printf("  %s: %d PC slot(s) share an identity with another slot "
                         "(Ninjask/Shedinja or a real clone) -- refuse-to-guess exercised live\n",
                         path, pc_dupes);
  }

  /* 3) a random, never-occurring identity matches 0. 0xA5-filled is not a real
   * personality/otId pair on any save in this corpus (checked: it never collided
   * across five real games in this test's own runs). */
  {
    uint8_t bogus[8]; memset(bogus, 0xA5, sizeof bogus);
    int wb = 1, ws = 1;
    int cnt = gb_reconcile_match(sb1, frlg, have_pc ? pc : NULL, bogus, &wb, &ws);
    CHECK(cnt == 0, "%s: a random id8 matches 0 (got %d)", path, cnt);
    CHECK(wb == -1 && ws == -1, "%s: a 0-match leaves where_box/where_slot at -1/-1", path);
  }

  /* 4) the canonical empty-slot value (all zero) must NEVER match, even though every
   * unoccupied slot in the save really is all-zero -- this is the degenerate case
   * gb_reconcile.h calls out by name. */
  {
    uint8_t zero8[8]; memset(zero8, 0, sizeof zero8);
    int cnt = gb_reconcile_match(sb1, frlg, have_pc ? pc : NULL, zero8, NULL, NULL);
    CHECK(cnt == 0, "%s: an all-zero id8 matches 0 despite empty slots sharing that value (got %d)",
          path, cnt);
  }

  /* 5) a clone: duplicate party slot 0's record into a free PC box slot, in a RAM
   * copy so the real corpus arrays are untouched, then confirm the match count is 2
   * and the caller is told not to guess (where_box/where_slot left at -1/-1). */
  if (have_pc) {
    static uint8_t pc2[G3_PC_BYTES];
    memcpy(pc2, pc, sizeof pc2);
    int free_box = -1, free_slot = -1;
    for (int b = 0; b < G3_TOTAL_BOXES && free_box < 0; b++)
      for (int s = 0; s < G3_IN_BOX; s++) {
        const uint8_t* rec = pk_box_slot(pc2, b, s);
        bool zero = true;
        for (int k = 0; k < 8; k++) if (rec[k]) { zero = false; break; }
        if (zero) { free_box = b; free_slot = s; break; }
      }
    if (free_box < 0) {
      printf("  %s: SKIP clone check (every PC slot occupied)\n", path);
    } else {
      const uint8_t* rec0 = pk_party_slot(sb1, frlg, 0);
      memcpy(pk_box_slot(pc2, free_box, free_slot), rec0, 80);
      int wb = 1, ws = 1;
      int cnt = gb_reconcile_match(sb1, frlg, pc2, rec0, &wb, &ws);
      CHECK(cnt == 2, "%s: a cloned record matches 2 (got %d)", path, cnt);
      CHECK(wb == -1 && ws == -1, "%s: a 2-match refuses to guess (where_box/where_slot at -1/-1)",
            path);
    }
  }
}

/* ============================================================================ */
/* S5-C review: gb_reconcile_plan() -- ordering, dedupe, re-verify, party floor. */
/* ============================================================================ */

/* Build a synthetic 100-byte party record whose first 8 bytes are `id` repeated --
 * gb_reconcile_plan() only ever reads the first 8 bytes (the identity) and copies
 * the record whole (party_release's own memmove), so the other 92 bytes never need
 * to look like a real Gen-3 mon for this module's own tests. */
static void mk_rec(uint8_t rec[100], uint8_t id) {
  memset(rec, id, 100);
}

static void mk_hit(GbReconHit* h, int8_t box, int8_t slot, uint8_t id) {
  memset(h, 0, sizeof *h);
  h->box = box; h->slot = slot;
  memset(h->id8, id, 8);
}

/* 6) Two hits both recording party slot 1 (the exact shape of the bug: the same
 * Gen-3 original transferred to two different Game Boy generations, both resolving
 * to the same slot at walk time) -> exactly ONE release, the neighbour (originally
 * slot 2, id 0x22) survives and shifts down into slot 1, and BOTH hits end up
 * `released` (the duplicate is claimed, never touched a second time). */
static void test_plan_party_duplicate(void) {
  static uint8_t sb1[G3_SAVEBLOCK1_BYTES];
  memset(sb1, 0, sizeof sb1);
  uint8_t rec[100];
  for (uint8_t id = 0x10; id <= 0x60; id += 0x10) { mk_rec(rec, id); party_append(sb1, false, rec); }
  CHECK(party_count(sb1, false) == 6, "party_duplicate: 6-member party built");

  GbReconHit hits[2];
  mk_hit(&hits[0], -1, 1, 0x20);   /* slot 1 holds id 0x20 -- both hits agree     */
  mk_hit(&hits[1], -1, 1, 0x20);

  /* released == 2, not 1: BOTH hits are claimable (the mon really is gone from the
   * save either way), even though only ONE physical party_release() ran -- that
   * physical count is what party_count() dropping by exactly one, below, proves. */
  int released = gb_reconcile_plan(hits, 2, sb1, false, NULL);
  CHECK(released == 2, "party_duplicate: gb_reconcile_plan reports both hits claimable (got %d)",
        released);
  CHECK(party_count(sb1, false) == 5, "party_duplicate: party shrinks by exactly ONE (one physical release)");
  CHECK(hits[0].released && hits[1].released,
        "party_duplicate: BOTH hits end up released (%d, %d)", hits[0].released, hits[1].released);
  CHECK(hits[1].duplicate, "party_duplicate: the second hit is marked duplicate");
  uint8_t* now1 = pk_party_slot(sb1, false, 1);
  CHECK(now1[0] == 0x30, "party_duplicate: the neighbour (id 0x30) shifted into slot 1 (got 0x%02X)",
        now1[0]);
  for (int i = 0; i < 5; i++) {
    uint8_t want = (uint8_t)(0x10 * (i < 1 ? i + 1 : i + 2));
    CHECK(pk_party_slot(sb1, false, i)[0] == want,
          "party_duplicate: post-release slot %d is id 0x%02X (got 0x%02X)",
          i, want, pk_party_slot(sb1, false, i)[0]);
  }
}

/* 7) Two hits recording the SAME PC (box, slot) -> exactly one release, the slot
 * ends up cleared exactly once (a second clip_clear_box_slot on an already-zero
 * slot would be harmless anyway, but this proves it is never even attempted --
 * `duplicate` short-circuits before touching `pc` at all). */
static void test_plan_pc_duplicate(void) {
  static uint8_t pc[G3_PC_BYTES];
  memset(pc, 0, sizeof pc);
  memset(pk_box_slot(pc, 2, 7), 0x55, 80);

  GbReconHit hits[2];
  mk_hit(&hits[0], 2, 7, 0x55);
  mk_hit(&hits[1], 2, 7, 0x55);

  static uint8_t sb1[G3_SAVEBLOCK1_BYTES];
  memset(sb1, 0, sizeof sb1);          /* no party hits in this test; must still be a
                                        * valid (empty, count 0) party for party_count() */
  /* released == 2 for the same reason as party_duplicate above: both hits are
   * claimable; the "exactly one release" this test's name refers to is the single
   * clip_clear_box_slot() call, proven by the slot ending up cleared just once. */
  int released = gb_reconcile_plan(hits, 2, sb1, false, pc);
  CHECK(released == 2, "pc_duplicate: gb_reconcile_plan reports both hits claimable (got %d)",
        released);
  CHECK(hits[0].released && hits[1].released,
        "pc_duplicate: BOTH hits end up released (%d, %d)", hits[0].released, hits[1].released);
  CHECK(hits[1].duplicate, "pc_duplicate: the second hit is marked duplicate");
  uint8_t zero80[80]; memset(zero80, 0, 80);
  CHECK(memcmp(pk_box_slot(pc, 2, 7), zero80, 80) == 0, "pc_duplicate: the slot is cleared exactly once");
}

/* 8) The party floor: a LONE party member is never released, even when a hit
 * targets it directly (no duplicate involved -- this is app_release()'s own
 * "the party can't be empty" rule, re-checked live inside gb_reconcile_plan()). */
static void test_plan_party_floor(void) {
  static uint8_t sb1[G3_SAVEBLOCK1_BYTES];
  memset(sb1, 0, sizeof sb1);
  uint8_t rec[100]; mk_rec(rec, 0x99);
  party_append(sb1, false, rec);
  CHECK(party_count(sb1, false) == 1, "party_floor: 1-member party built");

  GbReconHit hits[1];
  mk_hit(&hits[0], -1, 0, 0x99);
  int released = gb_reconcile_plan(hits, 1, sb1, false, NULL);
  CHECK(released == 0, "party_floor: gb_reconcile_plan releases nothing (got %d)", released);
  CHECK(!hits[0].released, "party_floor: the hit itself is not marked released");
  CHECK(party_count(sb1, false) == 1, "party_floor: the party is still 1 member");
  CHECK(pk_party_slot(sb1, false, 0)[0] == 0x99, "party_floor: the lone member is untouched");
}

/* 9) A 3-hit party batch, given to gb_reconcile_plan() in a SCRAMBLED (not
 * highest-slot-first) array order, still releases correctly -- provable only if
 * the function itself sorts by slot descending internally: a party of 4 (ids
 * 0x01, 0x02, 0x03, 0x04 at slots 0..3), hits for slots 0, 3 and 1 (in THAT
 * array order), must leave EXACTLY slot-2's mon (id 0x03) as the sole survivor. A
 * naive low-to-high or array-order release would shift indices under itself and
 * leave the wrong mon standing (or crash on a stale index). */
static void test_plan_party_order(void) {
  static uint8_t sb1[G3_SAVEBLOCK1_BYTES];
  memset(sb1, 0, sizeof sb1);
  uint8_t rec[100];
  for (uint8_t id = 1; id <= 4; id++) { mk_rec(rec, id); party_append(sb1, false, rec); }
  CHECK(party_count(sb1, false) == 4, "party_order: 4-member party built");

  GbReconHit hits[3];
  mk_hit(&hits[0], -1, 0, 1);   /* deliberately NOT highest-slot-first in array order */
  mk_hit(&hits[1], -1, 3, 4);
  mk_hit(&hits[2], -1, 1, 2);

  int released = gb_reconcile_plan(hits, 3, sb1, false, NULL);
  CHECK(released == 3, "party_order: gb_reconcile_plan releases all 3 (got %d)", released);
  CHECK(party_count(sb1, false) == 1, "party_order: exactly one member remains");
  CHECK(pk_party_slot(sb1, false, 0)[0] == 3,
        "party_order: the survivor is id 0x03 (got 0x%02X) -- proves highest-slot-first order",
        pk_party_slot(sb1, false, 0)[0]);
}

int main(int argc, char** argv) {
  int examined = 0;
  for (int i = 1; i < argc; i++) {
    test_save(argv[i]);
    examined++;
  }
  if (!examined) printf("  (no .sav given on argv -- nothing to test)\n");

  printf("== gb_reconcile_plan: ordering, dedupe, re-verify, party floor ==\n");
  test_plan_party_duplicate();
  test_plan_pc_duplicate();
  test_plan_party_floor();
  test_plan_party_order();

  printf("\n%s: %d check(s), %d failure(s)\n", g_fail ? "FAIL" : "OK", g_check, g_fail);
  return g_fail ? 1 : 0;
}

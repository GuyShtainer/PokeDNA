/* Host test: BACKLOG #226 review D4-R(a) -- HONEST LABEL. This file does NOT test
 * app_party_deposit_undo() or app_party_full_deposit_offer() (source/pdna_main.c,
 * GBA-only, not host-compilable). It re-implements the same THREE-CALL SEQUENCE each
 * of them makes (box_to_party = gen3_edit_load+em_set_party_flag+gen3_edit_commit;
 * party_to_box the mirror) locally as t_box_to_party()/t_party_to_box(), and tests
 * the PRIMITIVES those sequences are built from: gen3_edit.c's round trip and
 * gen3_box.c's party_append/party_release/party_count, on a synthetic 6-mon party
 * plus a synthetic PC box. A bug in the SHIPPED wrapper's own control flow --
 * D2-R's "zero the cell before checking party_append's return" being the exact
 * example -- is invisible here: this file never calls pdna_main.c's functions, so
 * it cannot see their order of operations. The real coverage for that lives in
 * tests/host_escape_gate_sites_test.py's structural checks (which read
 * source/pdna_main.c and source/pdna_box.c's actual text) plus the behavioural
 * pixel chain committed alongside this fix (tools/xfer226_deposit_rollback_chain.py).
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_party_deposit_undo_test.c \
 *      source/gen3_save.c source/gen3_mon.c source/gen3_box.c source/gen3_edit.c \
 *      source/gen3_daycare.c source/gen3_clip.c source/data_tables.c \
 *      -o /tmp/hpdu && /tmp/hpdu
 *
 * Sections:
 *   A. party_to_box(mon) -> box_to_party(that) round-trips IDENTICAL to the original party
 *      record for a mon that was already at full HP (matches box_to_party's own derivation
 *      -- recompute_party_stats always yields full HP, so a mon fresh out of a box round-
 *      trips byte-for-byte; this is the exact math the undo relies on).
 *   B. the full deposit+undo sequence on a synthetic 6-slot party + one PC box slot:
 *      party_release(slot 0) -> count 6->5, box slot <- party_to_box(mon0); UNDO:
 *      box_to_party(box slot) -> party_append -> count 5->6, box slot zeroed. (D4-R:
 *      the three former "RED PROOF" CHECKs asserting the pre-undo intermediate state
 *      were deleted -- they asserted the state this LOCAL sequence is constructed to
 *      be in at that point regardless of whether the real wrapper's bug exists, so
 *      they passed on both a fixed and a broken pdna_main.c: decoration, not proof.)
 *   C. deposit+undo does not restore the original SLOT (retail-parity note, D1's own
 *      comment): the recovered mon lands at the new tail (index 5), not back at index 0 --
 *      slots 1..5 shifted down by party_release, unaffected by the append.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "gen3_save.h"
#include "gen3_mon.h"
#include "gen3_box.h"
#include "gen3_edit.h"
#include "gen3_clip.h"
#include "data_tables.h"

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  !! FAIL: %s\n", msg); g_fail++; } } while (0)

/* Mirrors source/pdna_main.c's static box_to_party()/party_to_box() exactly (same three
 * calls each) -- these are what app_party_full_deposit_offer()/app_party_deposit_undo()
 * actually run; kept here, not exported, because pdna_main.c cannot be linked on the host. */
static void t_box_to_party(const uint8_t* box80, uint8_t out100[100]) {
  EditMon e; gen3_edit_load(box80, false, &e); em_set_party_flag(&e, true); gen3_edit_commit(&e, out100);
}
static void t_party_to_box(const uint8_t* party100, uint8_t out80[80]) {
  EditMon e; gen3_edit_load(party100, true, &e); em_set_party_flag(&e, false); gen3_edit_commit(&e, out80);
}

static void identity_check(const uint8_t* a100, const uint8_t* b100, const char* label) {
  PkMon ma, mb;
  bool oka = pk_decode_mon(a100, true, &ma);
  bool okb = pk_decode_mon(b100, true, &mb);
  CHECK(oka && okb, "both records decode");
  if (!oka || !okb) return;
  char msg[128];
  snprintf(msg, sizeof msg, "%s: species preserved", label);
  CHECK(ma.species == mb.species, msg);
  snprintf(msg, sizeof msg, "%s: personality preserved", label);
  CHECK(ma.personality == mb.personality, msg);
  snprintf(msg, sizeof msg, "%s: otId preserved", label);
  CHECK(ma.otId == mb.otId, msg);
  snprintf(msg, sizeof msg, "%s: IVs preserved", label);
  CHECK(memcmp(ma.ivs, mb.ivs, sizeof ma.ivs) == 0, msg);
  snprintf(msg, sizeof msg, "%s: EVs preserved", label);
  CHECK(memcmp(ma.evs, mb.evs, sizeof ma.evs) == 0, msg);
  snprintf(msg, sizeof msg, "%s: moves preserved", label);
  CHECK(memcmp(ma.moves, mb.moves, sizeof ma.moves) == 0, msg);
}

int main(void) {
  bool frlg = false;

  /* ---- A: full-HP round trip is byte-identical ---- */
  {
    uint8_t box80[80], party100[100], box80b[80], party100b[100];
    gen3_build_mon(384 /* RAYQUAZA, internal id, arbitrary */, 70, 0xABCD1234u, 0x11112222u, "GUY", 3, box80);
    t_box_to_party(box80, party100);              /* fresh out of a box: full HP by construction */
    t_party_to_box(party100, box80b);
    t_box_to_party(box80b, party100b);
    CHECK(memcmp(party100, party100b, 100) == 0,
          "A: box->party->box->party is byte-identical for a full-HP mon (the undo's own math)");
    identity_check(party100, party100b, "A");
  }

  /* ---- B: the deposit+undo sequence on a synthetic 6-slot party ---- */
  {
    static uint8_t sb1[G3_SAVEBLOCK1_BYTES];
    memset(sb1, 0, sizeof sb1);
    uint8_t box80[6][80];
    uint32_t species[6] = {1, 4, 7, 25, 133, 384};   /* distinct, arbitrary internal ids */
    for (int i = 0; i < 6; i++) {
      gen3_build_mon((uint16_t)species[i], 50, 0x10000000u + (uint32_t)i, 0x33334444u, "GUY", 3, box80[i]);
      uint8_t p100[100];
      t_box_to_party(box80[i], p100);
      CHECK(party_append(sb1, frlg, p100), "B: party_append fills slot");
    }
    CHECK(party_count(sb1, frlg) == 6, "B: party starts full (6)");

    /* snapshot slot 0's identity (GUY's mon 0, species[0]) before anything moves */
    PkMon before0;
    CHECK(pk_decode_mon(pk_party_slot(sb1, frlg, 0), true, &before0), "B: slot 0 decodes before deposit");

    /* app_party_full_deposit_offer()'s own sequence: party_to_box(chosen) -> [land it in a
     * PC box slot] -> party_release(chosen). */
    uint8_t depbox[80];
    t_party_to_box(pk_party_slot(sb1, frlg, 0), depbox);
    uint8_t pc_cell[80];
    memcpy(pc_cell, depbox, 80);                    /* the deposit's own PC landing spot */
    party_release(sb1, frlg, 0);
    CHECK(party_count(sb1, frlg) == 5, "B: party_release drops the count to 5 (deposit half)");

    /* app_party_deposit_undo()'s own sequence: box_to_party(cell) -> memset(cell) ->
     * party_append. */
    uint8_t recovered100[100];
    t_box_to_party(pc_cell, recovered100);
    memset(pc_cell, 0, 80);
    CHECK(party_append(sb1, frlg, recovered100), "B: party_append succeeds (undo half)");

    CHECK(party_count(sb1, frlg) == 6, "B (post-undo): party count restored to 6");
    bool cell_occupied_post = (pc_cell[0] | pc_cell[1] | pc_cell[2] | pc_cell[3]) != 0;
    CHECK(!cell_occupied_post, "B (post-undo): PC cell zeroed, nothing left on the card");

    /* ---- C: the recovered mon lands at the tail (index 5), retail-parity note from
     * D1's own comment -- this does NOT restore the original party ORDER. ---- */
    PkMon tail;
    CHECK(pk_decode_mon(pk_party_slot(sb1, frlg, 5), true, &tail), "C: slot 5 decodes after undo");
    CHECK(tail.species == before0.species && tail.personality == before0.personality,
          "C: the undone mon is the SAME mon (species+personality), now at the tail, not slot 0");
    PkMon new_slot0;
    CHECK(pk_decode_mon(pk_party_slot(sb1, frlg, 0), true, &new_slot0), "C: slot 0 decodes after undo");
    CHECK(new_slot0.species == species[1],
          "C: slot 0 is now the ORIGINAL slot 1 mon (party_release compacted down, append never restores order)");
  }

  printf("host_party_deposit_undo_test: %s\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}

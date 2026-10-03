/* Host test for source/xfer_reconcile.c -- the pure classification core behind the
 * BACKLOG #150 S150-11 transfer-ledger reconcile (docs/BANK-CROSSGEN-DESIGN.md SS11.8/
 * SS11.13 row S150-11). Every case names the mutation that would make it fail.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_xfer_reconcile_test.c \
 *      source/xfer_reconcile.c source/gb_sidecar.c source/bank_cell.c source/xfer_rec.c \
 *      source/gb_edit.c source/gen1_save.c source/gen2_save.c source/gen1_write.c \
 *      source/gen2_write.c source/gen12_convert.c source/gen3_clip.c source/gen3_box.c \
 *      source/gen3_mon.c source/gen3_save.c source/gen3_edit.c source/gen3_daycare.c \
 *      source/data_tables.c source/item_map_g2g3.c -o /tmp/hxrc
 *   /tmp/hxrc /Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/ (.sav files)
 *
 * CLS-1: the whole decision-2 table, one call per row, mutation: swap the
 *   bank_slot_pending guard so XRC_DEFERRED becomes XRC_DUP_BANK -- CLS-1 fails.
 * KEY-1: over the real corpus, every occupied party/PC slot's own xr-key finds
 *   itself exactly once at the right (box,slot); a random key finds 0; a planted
 *   clone finds 2 with where left at -1/-1; the Day-Care slot of an Emerald save is
 *   found with box == -2.
 * BANK-1: a synthetic 2400-B box with three bc_pack()ed cells -- by-bytes finds the
 *   PENDING entry's own cell; a fresh-serial re-pack of the SAME mon (RESTORED case)
 *   is 0 by bytes, 1 by identity; two identical-identity cells are refused (2).
 *   D4 negatives: four single-field-changed cells (DV/otid16/otname/gen) each refuse
 *   the by-identity match (0) -- proves none of xrc_bank_match's four identity terms
 *   is dead.
 * PHASE2-1 (review D1): a transcription of xfer_reconcile_bank_phase2()'s own
 *   per-box "touched" loop (pdna_main.c is not host-compilable) -- a target cell
 *   planted only in box 5, decoys in boxes 0-4, proves the FIXED `bank_matches < 2`
 *   condition scans through and finds it; the ORIGINAL `< 0 || == 1` condition stops
 *   after box 0 and never finds it (a zero-match box lands bank_matches at 0, which
 *   neither branch of the old condition reads as "keep scanning").
 * ID-1: a corpus record with its PID rewritten in a RAM copy is found by identity,
 *   not by key (species/otid/nickname bytes are the identity; PID never enters it).
 *   D4 negatives: the same rerolled record with species, then otId, changed each
 *   refuses the identity match (0) -- proves identity_matches_rec's two decoded-field
 *   comparisons are both load-bearing, not just the raw nick10 bytes.
 * REBUILD-1: xrc_rebuild_cell() round-trips a corpus-derived native cell through
 *   bc_pack/bc_unpack with flags/origin_game/rtc_epoch preserved and a fresh serial.
 * ORDER-1: xrc_apply_order() over {1, 3, 6} sorts strictly descending; mutation:
 *   ascending order fails the very next assertion.
 * ROW-1: xrc_row_text() never exceeds 39 bytes + NUL for the worst species/game pair.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "xfer_reconcile.h"
#include "gb_sidecar.h"
#include "bank_cell.h"
#include "gb_edit.h"
#include "gen3_save.h"
#include "gen3_box.h"
#include "gen3_mon.h"
#include "gen3_clip.h"
#include "data_tables.h"
#include "xfer_rec.h"      /* xr_key_g3 (BANKG3-1) */
#include "gen3_edit.h"     /* ID-1: a genuine PID reroll (decode/re-encrypt), not a
                            * raw byte flip -- species lives in an ENCRYPTED substruct
                            * keyed by personality^otId, so flipping personality alone
                            * would corrupt the decode instead of simulating a reroll. */

static int g_check = 0, g_fail = 0;
#define CHECK(c, ...) do { \
    g_check++; \
    if (!(c)) { printf("  !! FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } \
  } while (0)

/* ==== CLS-1: the whole decision-2 table ============================================ */

static void cls(uint8_t kind, uint8_t state, uint8_t dir, int g3k, bool dc, int g3id,
                int bankm, bool pend, bool keep, XrcResult* out) {
  XrcInput in;
  memset(&in, 0, sizeof in);
  in.kind = kind; in.state = state; in.direction = dir;
  in.g3_key_matches = g3k; in.g3_in_daycare = dc; in.g3_identity_matches = g3id;
  in.bank_matches = bankm; in.bank_slot_pending = pend; in.bank_keep = keep;
  xrc_classify(&in, out);
}

static void test_cls1(void) {
  XrcResult r;
  printf("== CLS-1: decision-2 table ==\n");

  /* G3_HOME entries are never touched by this classifier regardless of everything else. */
  cls(XR_KIND_G3_HOME, XR_STATE_NONE, XR_DIR_ABROAD_GB, 1, false, 0, 1, false, false, &r);
  CHECK(r.kind == XRC_G3HOME && r.actions == XRC_ACT_NONE, "G3_HOME -> XRC_G3HOME, no actions");

  /* PENDING x {bank,g3} the four cells. */
  cls(XR_KIND_NATIVE_HOME, XR_STATE_PENDING, XR_DIR_ABROAD_G3, 1, false, 0, 1, false, false, &r);
  CHECK(r.kind == XRC_PENDING_BOTH && r.actions == XRC_ACT_NONE, "PENDING bank+g3 -> PENDING_BOTH, no actions");
  cls(XR_KIND_NATIVE_HOME, XR_STATE_PENDING, XR_DIR_ABROAD_G3, 0, false, 0, 1, false, false, &r);
  CHECK(r.kind == XRC_PENDING_ORPHAN && r.actions == XRC_ACT_DELETE, "PENDING bank-only -> PENDING_ORPHAN, DELETE");
  cls(XR_KIND_NATIVE_HOME, XR_STATE_PENDING, XR_DIR_ABROAD_G3, 1, false, 0, 0, false, false, &r);
  CHECK(r.kind == XRC_PENDING_NOBANK && r.actions == XRC_ACT_NONE, "PENDING g3-only -> PENDING_NOBANK, no actions");
  cls(XR_KIND_NATIVE_HOME, XR_STATE_PENDING, XR_DIR_ABROAD_G3, 0, false, 0, 0, false, false, &r);
  CHECK(r.kind == XRC_PENDING_LOST && r.actions == (XRC_ACT_RESTORE | XRC_ACT_DELETE),
        "PENDING neither -> PENDING_LOST, RESTORE|DELETE");

  /* CLAIMED. */
  cls(XR_KIND_NATIVE_HOME, XR_STATE_CLAIMED, XR_DIR_ABROAD_G3, 1, false, 0, 1, false, false, &r);
  CHECK(r.kind == XRC_DUP_BANK && r.actions == XRC_ACT_REMOVE, "CLAIMED bank+g3, not pending -> DUP_BANK, REMOVE");
  cls(XR_KIND_NATIVE_HOME, XR_STATE_CLAIMED, XR_DIR_ABROAD_G3, 1, false, 0, 1, true, false, &r);
  CHECK(r.kind == XRC_DEFERRED && r.actions == XRC_ACT_NONE, "CLAIMED bank+g3, slot_pending -> DEFERRED, no actions (G-H1)");
  cls(XR_KIND_NATIVE_HOME, XR_STATE_CLAIMED, XR_DIR_ABROAD_G3, 1, false, 0, 0, false, false, &r);
  CHECK(r.kind == XRC_ABROAD && r.actions == XRC_ACT_NONE, "CLAIMED g3-only -> ABROAD, no actions");
  cls(XR_KIND_NATIVE_HOME, XR_STATE_CLAIMED, XR_DIR_ABROAD_G3, 0, false, 0, 0, false, false, &r);
  CHECK(r.kind == XRC_LOST && r.actions == (XRC_ACT_RESTORE | XRC_ACT_DELETE), "CLAIMED neither -> LOST, RESTORE|DELETE");

  /* CLAIMED / bridge (ABROAD_GB) -- excl. 2, unconditional regardless of match counts. */
  cls(XR_KIND_NATIVE_HOME, XR_STATE_CLAIMED, XR_DIR_ABROAD_GB, 0, false, 0, 1, false, false, &r);
  CHECK(r.kind == XRC_ABROAD_GB && r.actions == XRC_ACT_NONE, "CLAIMED/bridge -> ABROAD_GB, no actions (excl.2)");

  /* RESTORED. */
  cls(XR_KIND_NATIVE_HOME, XR_STATE_RESTORED, XR_DIR_ABROAD_G3, 1, false, 0, 1, false, false, &r);
  CHECK(r.kind == XRC_DUP_G3 && r.actions == XRC_ACT_RELEASE, "RESTORED bank(identity)+g3 -> DUP_G3, RELEASE");
  cls(XR_KIND_NATIVE_HOME, XR_STATE_RESTORED, XR_DIR_ABROAD_G3, 0, false, 0, 1, false, false, &r);
  CHECK(r.kind == XRC_STALE && r.actions == XRC_ACT_DELETE, "RESTORED bank-only -> STALE, DELETE");
  cls(XR_KIND_NATIVE_HOME, XR_STATE_RESTORED, XR_DIR_ABROAD_G3, 1, false, 0, 0, false, false, &r);
  CHECK(r.kind == XRC_RESTORED_MOVED && r.actions == XRC_ACT_NONE, "RESTORED g3-only -> RESTORED_MOVED, no actions");
  cls(XR_KIND_NATIVE_HOME, XR_STATE_RESTORED, XR_DIR_ABROAD_G3, 0, false, 0, 0, false, false, &r);
  CHECK(r.kind == XRC_STALE && r.actions == XRC_ACT_DELETE, "RESTORED neither -> STALE, DELETE");

  /* Day-Care (decision 15) -- beats everything else except the 2+ ambiguity guard. */
  cls(XR_KIND_NATIVE_HOME, XR_STATE_CLAIMED, XR_DIR_ABROAD_G3, 1, true, 0, 1, false, false, &r);
  CHECK(r.kind == XRC_DAYCARE && r.actions == XRC_ACT_NONE, "Day-Care key match -> DAYCARE, no actions");

  /* Stale key (decision 15). */
  cls(XR_KIND_NATIVE_HOME, XR_STATE_RESTORED, XR_DIR_ABROAD_G3, 0, false, 1, 1, false, false, &r);
  CHECK(r.kind == XRC_STALE_KEY && r.actions == XRC_ACT_REKEY, "0 key, 1 identity -> STALE_KEY, REKEY");
  cls(XR_KIND_NATIVE_HOME, XR_STATE_RESTORED, XR_DIR_ABROAD_G3, 0, false, 2, 1, false, false, &r);
  CHECK(r.kind == XRC_AMBIGUOUS && r.actions == XRC_ACT_NONE, "0 key, 2 identity -> AMBIGUOUS, no actions");

  /* Ambiguity, both sides. */
  cls(XR_KIND_NATIVE_HOME, XR_STATE_CLAIMED, XR_DIR_ABROAD_G3, 2, false, 0, 1, false, false, &r);
  CHECK(r.kind == XRC_AMBIGUOUS, "g3_key_matches 2+ -> AMBIGUOUS regardless of state");
  cls(XR_KIND_NATIVE_HOME, XR_STATE_RESTORED, XR_DIR_ABROAD_G3, 1, false, 0, 2, false, false, &r);
  CHECK(r.kind == XRC_AMBIGUOUS, "bank_matches 2+ -> AMBIGUOUS regardless of state");

  /* KEEP BOTH -- orthogonal to row kind. */
  cls(XR_KIND_NATIVE_HOME, XR_STATE_CLAIMED, XR_DIR_ABROAD_G3, 1, false, 0, 1, false, true, &r);
  CHECK(r.kind == XRC_DUP_BANK && r.kept == true, "bank_keep sets `kept` without changing the row kind");

  /* MUTATION (in the commit body too): swap the bank_slot_pending guard's branch
   * order -- if the DEFERRED check ran AFTER the DUP_BANK check instead of before,
   * a slot_pending duplicate would classify as XRC_DUP_BANK (G-H1 violated). Prove
   * the real function does NOT do that: */
  cls(XR_KIND_NATIVE_HOME, XR_STATE_CLAIMED, XR_DIR_ABROAD_G3, 1, false, 0, 1, true, false, &r);
  CHECK(r.kind != XRC_DUP_BANK, "MUTATION GUARD: slot_pending duplicate must never read as DUP_BANK");
}

/* ==== BANK-1: synthetic box, bytes vs identity match ================================ */

static void mk_gb_mon(GbEditMon* m, uint8_t gen, uint16_t otid, uint8_t atk, uint8_t def,
                      uint8_t spe, uint8_t spc, const uint8_t otname[GB_NAME_BYTES]) {
  memset(m, 0, sizeof *m);
  m->gen = gen;
  m->rec_len = (gen == GB_GEN1) ? 33 : 32;
  m->is_party = false;
  m->list_species = 1;
  memcpy(m->otname, otname, GB_NAME_BYTES);
  gb_set_otid(m, otid);
  gb_set_dv(m, GB_ATK, atk); gb_set_dv(m, GB_DEF, def);
  gb_set_dv(m, GB_SPE, spe); gb_set_dv(m, GB_SPC, spc);
}

/* ==== BANKG3-1: #270 pass-through -- a Gen-3 copy parked in the Bank ================
 * Mutation: delete the `bank_g3_matches >= 1` branch in xrc_classify -- the CLAIMED
 * ABROAD_G3 entry with 0 save matches goes back to XRC_LOST + RESTORE|DELETE (a clone
 * offer / loss of the way home). */
static void test_bankg3(void) {
  printf("== BANKG3-1: Gen-3 copy parked in the Bank ==\n");
  XrcInput in; memset(&in, 0, sizeof in);
  in.kind = XR_KIND_NATIVE_HOME; in.state = XR_STATE_CLAIMED; in.direction = XR_DIR_ABROAD_G3;
  in.bank_g3_matches = 1;
  XrcResult r; xrc_classify(&in, &r);
  CHECK(r.kind == XRC_ABROAD_BANK && r.actions == XRC_ACT_NONE,
        "BANKG3-1: parked in Bank -> ABROAD_BANK (#387), no actions (got kind %d actions 0x%x)", r.kind, r.actions);
  in.bank_g3_matches = 0; xrc_classify(&in, &r);
  CHECK(r.kind == XRC_LOST, "BANKG3-1: nothing anywhere -> still LOST");
  in.bank_g3_matches = 1; in.g3_key_matches = 1; xrc_classify(&in, &r);
  CHECK(r.kind == XRC_ABROAD, "BANKG3-1: in save (not bank-gated branch) still ABROAD");

  /* the matcher: non-native cells only, by the FNV key of bytes 0..7 */
  uint8_t box[2400]; memset(box, 0, sizeof box);
  uint8_t rec[80]; memset(rec, 0, sizeof rec);
  for (int i = 0; i < 8; i++) rec[i] = (uint8_t)(0x11 * (i + 1));
  uint64_t key = xr_key_g3(rec);
  CHECK(xrc_bank_g3_match(box, key) == 0, "BANKG3-1: empty box -> 0");
  memcpy(box + 5 * 80, rec, 80);
  CHECK(xrc_bank_g3_match(box, key) == 1, "BANKG3-1: one parked copy -> 1");
  memcpy(box + 9 * 80, rec, 80);
  CHECK(xrc_bank_g3_match(box, key) == 2, "BANKG3-1: two parked copies -> 2");
  CHECK(xrc_bank_g3_match(box, key ^ 1u) == 0, "BANKG3-1: other key -> 0");
  CHECK(xrc_bank_g3_match(NULL, key) == 0, "BANKG3-1: NULL box -> 0");
}

/* ==== G3HOME-1: #280 -- a native GB cell PARKED in the Bank whose G3_HOME entry is live ======
 * (the lift is a pass-through now: the Gen-3 original's way home is that ledger entry). It must
 * classify ACTION-FREE in every state -- RESTORE would clone, DELETE would lose the way home.
 * Mutation: make xrc_classify's `kind != XR_KIND_NATIVE_HOME` first line fall through (or add an
 * action there) -- this goes RED. */
static void test_g3home_parked(void) {
  printf("== G3HOME-1: parked native GB cell with a G3_HOME entry ==\n");
  static const uint8_t states[] = { XR_STATE_NONE, XR_STATE_CLAIMED, XR_STATE_PENDING, XR_STATE_RESTORED };
  for (unsigned i = 0; i < sizeof states; i++) {
    XrcInput in; memset(&in, 0, sizeof in);
    in.kind = XR_KIND_G3_HOME; in.state = states[i]; in.direction = XR_DIR_ABROAD_GB;
    in.bank_matches = 1;               /* the parked native cell */
    XrcResult r; xrc_classify(&in, &r);
    CHECK(r.kind == XRC_G3HOME && r.actions == XRC_ACT_NONE,
          "G3HOME-1: state %d -> XRC_G3HOME, no actions (got kind %d actions 0x%x)", (int)states[i], r.kind, r.actions);
  }
}

static void test_bank1(void) {
  printf("== BANK-1: xrc_bank_match, bytes vs identity ==\n");
  uint8_t otname[GB_NAME_BYTES] = "GUY\x50\x50\x50\x50\x50\x50\x50\x50";

  GbEditMon m1; mk_gb_mon(&m1, GB_GEN2, 0x1234, 5, 6, 7, 8, otname);
  uint8_t cell1[80], cell2[80], cell3[80];
  CHECK(bc_pack(&m1, 0, BC_ORIGIN_GOLD, 0, 100u, cell1) == 0, "BANK-1: cell1 packed (serial 100)");

  uint8_t box[2400]; memset(box, 0, sizeof box);
  memcpy(box + 0 * 80, cell1, 80);   /* slot 0 */

  /* Build a GbscEntry whose original80 == cell1 (the PENDING/CLAIMED case). */
  GbscEntry e; memset(&e, 0, sizeof e);
  e.gen = GB_GEN2; e.otid16 = 0x1234; e.dv4[0] = 5; e.dv4[1] = 6; e.dv4[2] = 7; e.dv4[3] = 8;
  memcpy(e.otname_written, otname, GB_NAME_BYTES);
  memcpy(e.original80, cell1, 80);

  int slot = -99;
  CHECK(xrc_bank_match(box, &e, false, &slot) == 1 && slot == 0,
        "BANK-1: by-bytes finds the PENDING entry's own cell at slot 0");
  CHECK(xrc_bank_match(box, &e, true, &slot) == 1 && slot == 0,
        "BANK-1: the SAME cell also matches by identity (same mon, same bytes)");

  /* Re-pack the SAME mon with a FRESH serial -- ident32 differs (bytes 0..7 change),
   * but identity (gen/otid16/dv4/otname) is unchanged: by-bytes 0, by-identity 1. */
  CHECK(bc_pack(&m1, 0, BC_ORIGIN_GOLD, 0, 200u, cell2) == 0, "BANK-1: cell2 packed (serial 200)");
  CHECK(memcmp(cell1, cell2, 8) != 0, "BANK-1: fresh serial changes bytes 0..7 (ident32)");
  memset(box, 0, sizeof box);
  memcpy(box + 0 * 80, cell2, 80);
  CHECK(xrc_bank_match(box, &e, false, &slot) == 0, "BANK-1 RESTORED case: by-bytes 0 against the OLD original80");
  CHECK(xrc_bank_match(box, &e, true, &slot) == 1 && slot == 0,
        "BANK-1 RESTORED case: by-identity still finds it (finding (b))");

  /* Two identical-identity cells -> refused (2). */
  GbEditMon m2; mk_gb_mon(&m2, GB_GEN2, 0x1234, 5, 6, 7, 8, otname);   /* same identity as m1 */
  CHECK(bc_pack(&m2, 0, BC_ORIGIN_GOLD, 0, 300u, cell3) == 0, "BANK-1: cell3 packed (serial 300, same identity)");
  memcpy(box + 1 * 80, cell3, 80);
  CHECK(xrc_bank_match(box, &e, true, &slot) == 2, "BANK-1: two identical-identity cells -> refused (2)");

  /* D4 negatives (review D4): a single differing identity field must refuse the
   * match (0), never match -- proves none of the by-identity comparison's four
   * terms (gen/otid16/dv4/otname) is dead. Each cell alone in a fresh box, still
   * compared against the SAME `e` (identity: GEN2/0x1234/5,6,7,8/"GUY..."). */
  uint8_t neg_box[2400];

  GbEditMon dv_bad; mk_gb_mon(&dv_bad, GB_GEN2, 0x1234, 9 /* atk flipped, was 5 */, 6, 7, 8, otname);
  uint8_t cell_dv[80];
  CHECK(bc_pack(&dv_bad, 0, BC_ORIGIN_GOLD, 0, 400u, cell_dv) == 0, "BANK-1 D4: dv-changed cell packed");
  memset(neg_box, 0, sizeof neg_box); memcpy(neg_box + 0 * 80, cell_dv, 80);
  CHECK(xrc_bank_match(neg_box, &e, true, &slot) == 0, "BANK-1 D4(a): one DV changed -> no identity match");

  GbEditMon otid_bad; mk_gb_mon(&otid_bad, GB_GEN2, 0x9999, 5, 6, 7, 8, otname);
  uint8_t cell_otid[80];
  CHECK(bc_pack(&otid_bad, 0, BC_ORIGIN_GOLD, 0, 401u, cell_otid) == 0, "BANK-1 D4: otid16-changed cell packed");
  memset(neg_box, 0, sizeof neg_box); memcpy(neg_box + 0 * 80, cell_otid, 80);
  CHECK(xrc_bank_match(neg_box, &e, true, &slot) == 0, "BANK-1 D4(b): otid16 changed -> no identity match");

  uint8_t otname_bad[GB_NAME_BYTES]; memcpy(otname_bad, otname, GB_NAME_BYTES);
  otname_bad[0] = (uint8_t)(otname_bad[0] ^ 0xFFu);
  GbEditMon name_bad; mk_gb_mon(&name_bad, GB_GEN2, 0x1234, 5, 6, 7, 8, otname_bad);
  uint8_t cell_name[80];
  CHECK(bc_pack(&name_bad, 0, BC_ORIGIN_GOLD, 0, 402u, cell_name) == 0, "BANK-1 D4: otname-changed cell packed");
  memset(neg_box, 0, sizeof neg_box); memcpy(neg_box + 0 * 80, cell_name, 80);
  CHECK(xrc_bank_match(neg_box, &e, true, &slot) == 0, "BANK-1 D4(c): one OT-name byte changed -> no identity match");

  GbEditMon gen_bad; mk_gb_mon(&gen_bad, GB_GEN1, 0x1234, 5, 6, 7, 8, otname);
  uint8_t cell_gen[80];
  CHECK(bc_pack(&gen_bad, 0, BC_ORIGIN_RED, 0, 403u, cell_gen) == 0, "BANK-1 D4: gen-flipped cell packed");
  memset(neg_box, 0, sizeof neg_box); memcpy(neg_box + 0 * 80, cell_gen, 80);
  CHECK(xrc_bank_match(neg_box, &e, true, &slot) == 0, "BANK-1 D4(d): gen flipped -> no identity match");
}

/* ==== PHASE2-1: transcribed touched-loop pin (BACKLOG #150 S150-11 review D1) ======
 * xfer_reconcile_bank_phase2() (source/pdna_main.c ~10117) is static in pdna_main.c
 * and not host-compilable (tonc deps), so this transcribes its own per-box "touched"
 * loop exactly, driven by the real xrc_bank_match() over 16 synthetic 2400-byte boxes.
 * The bug: after a box scan finds NO match, bank_matches lands at 0 (h->bank_matches
 * < 0 ? 0 : ...) -- neither < 0 nor == 1, so the ORIGINAL condition
 * `bank_matches < 0 || bank_matches == 1` reads that hit as already resolved and the
 * scan breaks after box 0 whenever box 0 itself had no match. The fix,
 * `bank_matches < 2`, keeps scanning through every box until the candidate is either
 * found unique (1, and a later box could still make it ambiguous) or capped at 2. */

static bool phase2_touched_fixed(int8_t bank_matches) { return bank_matches < 2; }
static bool phase2_touched_buggy(int8_t bank_matches) {
  return bank_matches < 0 || bank_matches == 1;
}

/* One candidate's worth of the real loop body, single-hit version (the real loop's
 * `for (i...) if (cond(xrc[i].bank_matches)) touched=true;break;` collapses to
 * `touched_fn(h->bank_matches)` for exactly one candidate). */
static void phase2_scan(uint8_t box2400[16][2400], XrcHit* h, bool (*touched_fn)(int8_t)) {
  for (int box = 0; box < 16; box++) {
    if (!touched_fn(h->bank_matches)) break;
    GbscEntry e2; memset(&e2, 0, sizeof e2);
    e2.gen = h->gen; e2.otid16 = h->otid16;
    memcpy(e2.dv4, h->dv4, 4);
    memcpy(e2.otname_written, h->otname, GB_NAME_BYTES);
    memcpy(e2.original80, h->orig8, 8);
    int slot = -1;
    int m = xrc_bank_match(box2400[box], &e2, false, &slot);
    if (h->bank_matches < 0) h->bank_matches = 0;
    int total = h->bank_matches + m;
    h->bank_matches = (int8_t)(total > 2 ? 2 : total);
    if (m == 1 && h->bank_matches == 1) { h->bank_box = (int8_t)box; h->bank_slot = (int8_t)slot; }
  }
}

static void test_phase2(void) {
  printf("== PHASE2-1: xfer_reconcile_bank_phase2 touched-loop pin (review D1) ==\n");
  uint8_t t_otname[GB_NAME_BYTES] = "P2\x50\x50\x50\x50\x50\x50\x50\x50\x50";
  GbEditMon target; mk_gb_mon(&target, GB_GEN2, 0x4321, 1, 2, 3, 4, t_otname);
  uint8_t target_cell[80];
  CHECK(bc_pack(&target, 0, BC_ORIGIN_GOLD, 0, 500u, target_cell) == 0, "PHASE2-1: target cell packed");

  uint8_t d_otname[GB_NAME_BYTES] = "DE\x50\x50\x50\x50\x50\x50\x50\x50\x50";
  GbEditMon decoy; mk_gb_mon(&decoy, GB_GEN2, 0x9999, 9, 9, 9, 9, d_otname);
  uint8_t decoy_cell[80];
  CHECK(bc_pack(&decoy, 0, BC_ORIGIN_GOLD, 0, 501u, decoy_cell) == 0, "PHASE2-1: decoy cell packed");

  static uint8_t boxes[16][2400];
  for (int b = 0; b < 16; b++) memset(boxes[b], 0, 2400);
  for (int b = 0; b < 5; b++) memcpy(boxes[b] + 0 * 80, decoy_cell, 80);  /* boxes 0-4: no match, m==0 */
  memcpy(boxes[5] + 3 * 80, target_cell, 80);                            /* box 5 slot 3: the real match */

  XrcHit h; memset(&h, 0, sizeof h);
  h.bank_matches = -1; h.bank_box = -1; h.bank_slot = -1;
  h.gen = GB_GEN2; h.otid16 = 0x4321;
  h.dv4[0] = 1; h.dv4[1] = 2; h.dv4[2] = 3; h.dv4[3] = 4;
  memcpy(h.otname, t_otname, GB_NAME_BYTES);
  memcpy(h.orig8, target_cell, 8);

  phase2_scan(boxes, &h, phase2_touched_fixed);
  CHECK(h.bank_matches == 1 && h.bank_box == 5 && h.bank_slot == 3,
        "PHASE2-1: FIXED condition scans through box 5 and finds the match "
        "(got matches=%d box=%d slot=%d)", h.bank_matches, h.bank_box, h.bank_slot);

  /* MUTATION: the ORIGINAL `< 0 || == 1` condition breaks after box 0 (a decoy-only
   * scan leaves bank_matches at 0, read as "already resolved") -- box 5 is never
   * reached, so a CLAIMED entry with its real Bank copy in box 5 would mis-classify
   * as *_LOST (RESTORE clones it, DELETE's confirm lies). */
  XrcHit hb; memset(&hb, 0, sizeof hb);
  hb.bank_matches = -1; hb.bank_box = -1; hb.bank_slot = -1;
  hb.gen = GB_GEN2; hb.otid16 = 0x4321;
  hb.dv4[0] = 1; hb.dv4[1] = 2; hb.dv4[2] = 3; hb.dv4[3] = 4;
  memcpy(hb.otname, t_otname, GB_NAME_BYTES);
  memcpy(hb.orig8, target_cell, 8);
  phase2_scan(boxes, &hb, phase2_touched_buggy);
  CHECK(hb.bank_matches == 0 && hb.bank_box == -1,
        "PHASE2-1 MUTATION: the buggy condition stops at box 0 and never reaches "
        "box 5 (got matches=%d box=%d) -- proves the D1 fix is load-bearing",
        hb.bank_matches, hb.bank_box);
}

/* ==== ORDER-1 ======================================================================= */

static void test_order1(void) {
  printf("== ORDER-1: xrc_apply_order descending ==\n");
  uint8_t idx[3] = {1, 3, 6};
  xrc_apply_order(idx, 3);
  CHECK(idx[0] == 6 && idx[1] == 3 && idx[2] == 1, "ORDER-1: {1,3,6} sorts to {6,3,1}");

  uint8_t idx2[5] = {2, 2, 5, 0, 7};
  xrc_apply_order(idx2, 5);
  CHECK(idx2[0] == 7 && idx2[1] == 5 && idx2[2] == 2 && idx2[3] == 2 && idx2[4] == 0,
        "ORDER-1: duplicates and a full spread sort correctly");

  /* MUTATION: an ascending sort would leave {1,3,6} untouched -- the assertion above
   * already fails on that mutant (idx[0]==6 would be false). */
}

/* ==== ROW-1 ========================================================================= */

/* ==== REKEY-1: BACKLOG #215(b), xrc_rekey_should_attempt/xrc_rekey_mark_done ======= */

static void test_rekey1(void) {
  printf("== REKEY-1: two stale entries in one .pds file -> only the first attempts ==\n");
  bool fr[8]; memset(fr, 0, sizeof fr);

  /* row A: file_idx 3, no prior attempt this pass -- must be told to attempt. */
  CHECK(xrc_rekey_should_attempt(fr, 8, 3), "REKEY-1: a fresh file_idx is told to attempt");
  xrc_rekey_mark_done(fr, 8, 3);   /* simulates row A's real rename landing */

  /* row B: SAME file_idx 3 (the second stale entry inside the SAME physical file,
   * already moved by row A's own rename) -- must now be refused, the exact bug this
   * lane fixes (the old code's `f_stat(old_path) != FR_OK` test could not tell "I
   * just moved this" from "row A already moved this out from under me", and counted
   * row B as a second success for a rename that never happened). */
  CHECK(!xrc_rekey_should_attempt(fr, 8, 3), "REKEY-1: the same file_idx is refused a second attempt");

  /* row C: a DIFFERENT file_idx must be entirely unaffected -- the guard is per-file,
   * not global. */
  CHECK(xrc_rekey_should_attempt(fr, 8, 4), "REKEY-1: an unrelated file_idx is still told to attempt");

  /* a failed attempt (row D, file_idx 5) must NOT mark the file done -- a later row
   * for the same file still gets its own real attempt. */
  CHECK(xrc_rekey_should_attempt(fr, 8, 5), "REKEY-1: file_idx 5 starts attemptable");
  /* (row D's own attempt fails -- the real caller never calls xrc_rekey_mark_done()
   * on a failure path; simulated here by simply not calling it.) */
  CHECK(xrc_rekey_should_attempt(fr, 8, 5), "REKEY-1: a FAILED attempt leaves the file attemptable again");

  /* out-of-bounds file_idx never blocks (defensive -- the caller's own bound is
   * trusted, this predicate only ever narrows, never widens, what the caller allows). */
  CHECK(xrc_rekey_should_attempt(fr, 8, 200), "REKEY-1: an out-of-range file_idx is never blocked");
}

static void test_row1(void) {
  printf("== ROW-1: xrc_row_text worst case <= 39 bytes ==\n");
  char out[40];
  int n = xrc_row_text(XRC_PENDING_LOST, "NIDOKING", "SAPPHIRE", out);
  CHECK(n <= 39, "ROW-1: worst realistic species/game pair fits (got %d)", n);
  CHECK((int)strlen(out) == n, "ROW-1: strlen matches the returned length");

  /* Force an oversized species/game to prove the TRUNCATION, not just the happy path. */
  int n2 = xrc_row_text(XRC_AMBIGUOUS, "WAYTOOLONGASPECIESNAME", "WAYTOOLONGAGAME", out);
  CHECK(n2 <= 39, "ROW-1: an oversized species/game is truncated to fit (got %d)", n2);
  CHECK((int)strlen(out) == n2, "ROW-1: strlen matches after truncation too");
  /* #387: the Bank-parked row reads like the PC one ("restorable"); only its detail line differs */
  xrc_row_text(XRC_ABROAD_BANK, "NIDOKING", "SAPPHIRE", out);
  CHECK(strstr(out, "restorable") != NULL, "ROW-1: ABROAD_BANK row has no status word (got '%s')", out);
}

/* ==== INBANK-1 (BACKLOG #385) =========================================================
 * A RESTORE TO BANK whose ledger rewrite never landed leaves the mon in the Bank under a NEW serial: the entry's
 * first-8-byte match misses, so the row read LOST and offered RESTORE again (a clone). With the identity-only
 * Bank count the row is XRC_IN_BANK, DELETE only. Mutation: drop either bank_ident_matches branch in
 * xrc_classify -- the matching assertion fails. */
static void test_inbank1(void) {
  printf("== INBANK-1: identity already in the Bank -> IN_BANK, never RESTORE ==\n");
  XrcInput in; XrcResult r;
  memset(&in, 0, sizeof in);
  in.kind = XR_KIND_NATIVE_HOME; in.direction = XR_DIR_ABROAD_G3; in.state = XR_STATE_CLAIMED;
  in.bank_ident_matches = 1;
  xrc_classify(&in, &r);
  CHECK(r.kind == XRC_IN_BANK && r.actions == XRC_ACT_DELETE, "INBANK-1: CLAIMED, nothing but an identity hit -> IN_BANK, DELETE only (kind %d act 0x%x)", r.kind, r.actions);
  in.bank_ident_matches = 2;
  xrc_classify(&in, &r);
  CHECK(r.kind == XRC_IN_BANK && r.actions == XRC_ACT_DELETE, "INBANK-1: 2+ identity hits is still IN_BANK (a copy exists)");
  in.bank_ident_matches = 0;
  xrc_classify(&in, &r);
  CHECK(r.kind == XRC_LOST && r.actions == (XRC_ACT_RESTORE | XRC_ACT_DELETE), "INBANK-1: no identity hit -> LOST keeps RESTORE|DELETE");
  in.state = XR_STATE_PENDING; in.bank_ident_matches = 1;
  xrc_classify(&in, &r);
  CHECK(r.kind == XRC_IN_BANK && r.actions == XRC_ACT_DELETE, "INBANK-1: PENDING, nothing but an identity hit -> IN_BANK");
  in.bank_ident_matches = 0;
  xrc_classify(&in, &r);
  CHECK(r.kind == XRC_PENDING_LOST && (r.actions & XRC_ACT_RESTORE), "INBANK-1: PENDING, no identity hit -> PENDING_LOST keeps RESTORE");
  /* the override must not eat the normal rows */
  in.state = XR_STATE_CLAIMED; in.bank_ident_matches = 1; in.g3_key_matches = 1;
  xrc_classify(&in, &r);
  CHECK(r.kind == XRC_ABROAD && r.actions == XRC_ACT_NONE, "INBANK-1: a Gen-3 copy in the save wins (ABROAD)");
  in.g3_key_matches = 0; in.bank_matches = 1;
  xrc_classify(&in, &r);
  CHECK(r.kind == XRC_ABROAD, "INBANK-1: a first-8 Bank hit keeps the normal row");
  char out[40];
  int n = xrc_row_text(XRC_IN_BANK, "NIDOKING", "SAPPHIRE", out);
  CHECK(n <= 39 && strstr(out, "in the Bank") != NULL, "INBANK-1: row text says 'in the Bank' and fits (%s)", out);
  /* the real pair: an original cell and its RESTORE rebuild (new serial) -- first 8 differ, identity hits */
  uint8_t otname[GB_NAME_BYTES] = "INB\x50\x50\x50\x50\x50\x50\x50\x50";
  GbEditMon m; mk_gb_mon(&m, GB_GEN2, 0x4242, 3, 4, 5, 6, otname);
  uint8_t orig[80], rebuilt[80], box[2400];
  CHECK(bc_pack(&m, 0, BC_ORIGIN_GOLD, 7u, 1000u, orig) == 0, "INBANK-1: original cell packed");
  GbscEntry e; memset(&e, 0, sizeof e);
  e.gen = GB_GEN2; e.otid16 = 0x4242;
  e.dv4[0] = gb_get_dv(&m, GB_ATK); e.dv4[1] = gb_get_dv(&m, GB_DEF); e.dv4[2] = gb_get_dv(&m, GB_SPE); e.dv4[3] = gb_get_dv(&m, GB_SPC);
  memcpy(e.otname_written, otname, GB_NAME_BYTES); memcpy(e.original80, orig, 80);
  CHECK(xrc_rebuild_cell(&e, 2000u, rebuilt) == 0, "INBANK-1: rebuilt (RESTORE) cell");
  memset(box, 0, sizeof box); memcpy(box + 3 * 80, rebuilt, 80);
  int slot = -1;
  CHECK(xrc_bank_match(box, &e, false, &slot) == 0, "INBANK-1: the restored copy does NOT match by first-8 bytes (new serial)");
  CHECK(xrc_bank_match(box, &e, true, &slot) == 1 && slot == 3, "INBANK-1: ...but matches by identity (the #385 count)");
  /* D1 (review): the Bank match is a RE-SERIAL of the original, not bare identity. Mutant: helper skips the serial
   * swap (hash the cell as stored) -> the positive case below goes RED. */
  uint8_t ser[4]; memcpy(ser, orig + BC_OFF_BANK_SERIAL, 4);
  CHECK(xrc_bank_reserial_match(box, orig, ser) == 1, "INBANK-1: re-serial of the original -> 1 hit (=> XRC_IN_BANK)");
  in.state = XR_STATE_CLAIMED; in.g3_key_matches = 0; in.bank_matches = 0; in.bank_ident_matches = 1;
  xrc_classify(&in, &r);
  CHECK(r.kind == XRC_IN_BANK, "INBANK-1: re-serial hit classifies XRC_IN_BANK");
  /* negatives: a DIFFERENT Bank mon with the same gen + OT id + DVs + OT name */
  GbEditMon d; mk_gb_mon(&d, GB_GEN2, 0x4242, 3, 4, 5, 6, otname);
  uint8_t oth[80], bx2[2400];
  d.list_species = 130;                              /* different species */
  CHECK(bc_pack(&d, 0, BC_ORIGIN_GOLD, 7u, 3000u, oth) == 0, "INBANK-1: other-species cell packed");
  memset(bx2, 0, sizeof bx2); memcpy(bx2 + 5 * 80, oth, 80);
  CHECK(xrc_bank_match(bx2, &e, true, &slot) == 1, "INBANK-1: (precondition) the OLD identity match WOULD hit the other mon");
  CHECK(xrc_bank_reserial_match(bx2, orig, ser) == 0, "INBANK-1: different species, same OT+DVs -> NOT a re-serial");
  d.list_species = 1; d.nick[0] = 0x80; d.nick[1] = 0x50;     /* different nickname */
  CHECK(bc_pack(&d, 0, BC_ORIGIN_GOLD, 7u, 3000u, oth) == 0, "INBANK-1: other-nickname cell packed");
  memcpy(bx2 + 5 * 80, oth, 80);
  CHECK(xrc_bank_reserial_match(bx2, orig, ser) == 0, "INBANK-1: different nickname, same OT+DVs -> NOT a re-serial");
  d.nick[0] = 0; d.nick[1] = 0;                      /* same everything, other serial: the bare twin */
  CHECK(bc_pack(&d, 0, BC_ORIGIN_GOLD, 7u, 3000u, oth) == 0, "INBANK-1: twin cell packed");
  memcpy(bx2 + 5 * 80, oth, 80);
  CHECK(xrc_bank_reserial_match(bx2, orig, ser) == 1, "INBANK-1: byte-identical twin under a new serial IS a re-serial (same record)");
  /* the classify result for the lost-original-in-ledger fixture: no re-serial hit -> LOST with RESTORE */
  in.bank_ident_matches = 0;
  xrc_classify(&in, &r);
  CHECK(r.kind == XRC_LOST && (r.actions & XRC_ACT_RESTORE), "INBANK-1: different Bank mon (0 re-serial hits) -> stays LOST with RESTORE");
  /* D2 (review): PENDING ordering -- a Gen-3 copy on the card wins over an identity hit */
  memset(&in, 0, sizeof in);
  in.kind = XR_KIND_NATIVE_HOME; in.direction = XR_DIR_ABROAD_G3; in.state = XR_STATE_PENDING;
  in.g3_key_matches = 1; in.g3_on_card = true; in.bank_ident_matches = 1;
  xrc_classify(&in, &r);
  CHECK(r.kind == XRC_PENDING_NOBANK && r.actions == XRC_ACT_PROMOTE, "INBANK-1: PENDING + g3 copy + identity hit -> PENDING_NOBANK/PROMOTE (kind %d act 0x%x)", r.kind, r.actions);
}

/* ==== PROMOTE-1 (BACKLOG #377) ========================================================
 * A PENDING row whose Gen-3 copy is found exactly once offers MARK FINISHED (PROMOTE) -- but ONLY when the open save is
 * the verified one on the card (g3_on_card). Mutations: drop g3_on_card from either branch -> the "RAM only" check
 * fails; drop the PROMOTE bit -> the offering checks fail. PENDING_ORPHAN / PENDING_LOST must never offer it. */
static void test_promote1(void) {
  printf("== PROMOTE-1: MARK FINISHED only for a PENDING row whose Gen-3 copy is on the card ==\n");
  XrcInput in; XrcResult r;
  memset(&in, 0, sizeof in);
  in.kind = XR_KIND_NATIVE_HOME; in.direction = XR_DIR_ABROAD_G3; in.state = XR_STATE_PENDING;
  in.g3_key_matches = 1; in.g3_on_card = true;
  xrc_classify(&in, &r);
  CHECK(r.kind == XRC_PENDING_NOBANK && r.actions == XRC_ACT_PROMOTE, "PROMOTE-1: unproven + on card -> PROMOTE (kind %d act 0x%x)", r.kind, r.actions);
  in.bank_matches = 1;
  xrc_classify(&in, &r);
  CHECK(r.kind == XRC_PENDING_BOTH && r.actions == XRC_ACT_PROMOTE, "PROMOTE-1: unfinished + on card -> PROMOTE");
  in.g3_on_card = false;
  xrc_classify(&in, &r);
  CHECK(r.kind == XRC_PENDING_BOTH && r.actions == XRC_ACT_NONE, "PROMOTE-1: unfinished but the copy is RAM-only -> nothing offered");
  in.bank_matches = 0;
  xrc_classify(&in, &r);
  CHECK(r.kind == XRC_PENDING_NOBANK && r.actions == XRC_ACT_NONE, "PROMOTE-1: unproven but RAM-only -> nothing offered");
  in.g3_on_card = true; in.g3_key_matches = 2;
  xrc_classify(&in, &r);
  CHECK(r.kind == XRC_AMBIGUOUS && r.actions == XRC_ACT_NONE, "PROMOTE-1: 2 copies is ambiguous, not promotable");
  in.g3_key_matches = 0; in.bank_matches = 1;
  xrc_classify(&in, &r);
  CHECK(r.kind == XRC_PENDING_ORPHAN && r.actions == XRC_ACT_DELETE, "PROMOTE-1: never-landed (no Gen-3 copy) offers no PROMOTE");
  in.bank_matches = 0;
  xrc_classify(&in, &r);
  CHECK(r.kind == XRC_PENDING_LOST && !(r.actions & XRC_ACT_PROMOTE), "PROMOTE-1: record-only offers no PROMOTE");
  in.state = XR_STATE_CLAIMED; in.g3_key_matches = 1;
  xrc_classify(&in, &r);
  CHECK(r.kind == XRC_ABROAD && r.actions == XRC_ACT_NONE, "PROMOTE-1: a CLAIMED row never gets PROMOTE");
  /* the bit is its own and fits the popup mask */
  CHECK(XRC_ACT_PROMOTE == 0x20u && (XRC_ACT_PROMOTE & (XRC_ACT_REMOVE | XRC_ACT_RELEASE | XRC_ACT_RESTORE | XRC_ACT_DELETE | XRC_ACT_REKEY)) == 0,
        "PROMOTE-1: XRC_ACT_PROMOTE is a distinct bit");
}

/* ==== REBUILD-1 ====================================================================== */

static void test_rebuild1(void) {
  printf("== REBUILD-1: xrc_rebuild_cell round-trip ==\n");
  uint8_t otname[GB_NAME_BYTES] = "REB\x50\x50\x50\x50\x50\x50\x50\x50";
  GbEditMon m; mk_gb_mon(&m, GB_GEN1, 0x5678, 9, 10, 11, 12, otname);
  uint8_t cell[80];
  CHECK(bc_pack(&m, BC_FLAG_HOLDS_ITEM, BC_ORIGIN_RED, 0xAABBCCDDu, 42u, cell) == 0,
        "REBUILD-1: original cell packed");

  GbscEntry e; memset(&e, 0, sizeof e);
  memcpy(e.original80, cell, 80);

  uint8_t out80[80];
  CHECK(xrc_rebuild_cell(&e, 999u, out80) == 0, "REBUILD-1: rebuild succeeds");
  CHECK(memcmp(out80, cell, 8) != 0, "REBUILD-1: the fresh serial changes bytes 0..7");

  GbEditMon mon2; BcMeta meta2;
  CHECK(bc_unpack(out80, &mon2, &meta2), "REBUILD-1: rebuilt cell unpacks as native");
  CHECK(meta2.flags == BC_FLAG_HOLDS_ITEM, "REBUILD-1: flags preserved");
  CHECK(meta2.origin_game == BC_ORIGIN_RED, "REBUILD-1: origin_game preserved");
  CHECK(meta2.rtc_epoch == 0xAABBCCDDu, "REBUILD-1: rtc_epoch preserved");
  CHECK(meta2.bank_serial == 999u, "REBUILD-1: bytes 73..76 hold the NEW serial");
  CHECK(mon2.gen == GB_GEN1 && gb_get_otid(&mon2) == 0x5678,
        "REBUILD-1: identity fields survive the round trip");
}

/* ==== KEY-1 / ID-1: corpus =========================================================== */

static void test_corpus(const char* path) {
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

  /* dc_layout()'s own values (source/pdna_main.c), reproduced here -- this is a pure-C
   * host test with no link to pdna_main.c, so the constants are pinned by the same
   * derivation, not re-exported. */
  uint32_t dc_base = (info.version_guess == G3_VER_EMERALD) ? 0x3030 :
                      frlg ? 0x2F80 : 0x2F9C;
  uint32_t dc_stride = (info.version_guess == G3_VER_RS && !frlg) ? 80 : 140;

  printf("  %s: party=%d frlg=%d have_pc=%d emerald=%d\n", path, nparty, (int)frlg,
        (int)have_pc, info.version_guess == G3_VER_EMERALD);

  /* KEY-1: every occupied party slot's own key finds itself exactly once, at (-1, i). */
  for (int i = 0; i < nparty; i++) {
    const uint8_t* rec = pk_party_slot(sb1, frlg, i);
    uint64_t key = 0; { uint64_t h = 14695981039346656037ULL;
      for (int b = 0; b < 8; b++) { h ^= (uint64_t)rec[b]; h *= 1099511628211ULL; } key = h; }
    int wb = -99, ws = -99;
    int cnt = xrc_g3_match_key(sb1, frlg, have_pc ? pc : NULL, dc_base, dc_stride, key, &wb, &ws);
    CHECK(cnt >= 1, "%s: KEY-1 party[%d] finds itself (got %d)", path, i, cnt);
    if (cnt == 1) CHECK(wb == -1 && ws == i, "%s: KEY-1 party[%d] resolves to (-1,%d), got (%d,%d)",
                        path, i, i, wb, ws);
  }

  /* A random key never matches. */
  {
    int wb = -99, ws = -99;
    int cnt = xrc_g3_match_key(sb1, frlg, have_pc ? pc : NULL, dc_base, dc_stride,
                               0xDEADBEEFCAFEF00DULL, &wb, &ws);
    CHECK(cnt == 0 && wb == -1 && ws == -1, "%s: KEY-1 a random key matches 0", path);
  }

  /* A planted clone (a RAM-only copy of party[0] into PC slot 0..0) matches 2. */
  if (have_pc && nparty >= 1) {
    uint8_t pc2[G3_PC_BYTES]; memcpy(pc2, pc, sizeof pc2);
    const uint8_t* rec0 = pk_party_slot(sb1, frlg, 0);
    memcpy(pk_box_slot(pc2, 0, 0), rec0, 80);
    uint64_t key = 0; { uint64_t h = 14695981039346656037ULL;
      for (int b = 0; b < 8; b++) { h ^= (uint64_t)rec0[b]; h *= 1099511628211ULL; } key = h; }
    int wb = -99, ws = -99;
    int cnt = xrc_g3_match_key(sb1, frlg, pc2, dc_base, dc_stride, key, &wb, &ws);
    CHECK(cnt == 2 && wb == -1 && ws == -1,
          "%s: KEY-1 a planted clone matches 2, where left at -1/-1", path);
  }

  /* ID-1: a genuine PID reroll (decode -> em_set_pid -> lossless re-encode) in a RAM
   * copy; the key breaks (personality is part of it), identity still finds it
   * (species/otId/nickname are all either plaintext or survive re-encryption). */
  if (nparty >= 1) {
    const uint8_t* rec0 = pk_party_slot(sb1, frlg, 0);
    EditMon em;
    gen3_edit_load(rec0, true, &em);
    uint32_t new_pid = em.personality ^ 0xFFFFFFFFu;   /* guaranteed different */
    em_set_pid(&em, new_pid);
    uint8_t rec_out[100];
    gen3_edit_commit(&em, rec_out);
    CHECK(memcmp(rec_out, rec0, 4) != 0, "%s: ID-1 the reroll actually changed personality", path);

    uint8_t sb1c[G3_SAVEBLOCK1_BYTES]; memcpy(sb1c, sb1, sizeof sb1c);
    uint8_t* rec0c = (uint8_t*)pk_party_slot(sb1c, frlg, 0);
    memcpy(rec0c, rec_out, 100);

    uint64_t old_key = 0; { uint64_t h = 14695981039346656037ULL;
      for (int b = 0; b < 8; b++) { h ^= (uint64_t)rec0[b]; h *= 1099511628211ULL; } old_key = h; }
    int wb = -99, ws = -99;
    CHECK(xrc_g3_match_key(sb1c, frlg, NULL, dc_base, dc_stride, old_key, &wb, &ws) == 0,
          "%s: ID-1 the OLD key no longer finds the rerolled record", path);

    PkMon m0;
    CHECK(pk_decode_mon(rec0, true, &m0), "%s: ID-1 original party[0] decodes", path);
    uint16_t sp_written = pk_national_no(m0.species);
    uint16_t otid16 = (uint16_t)(m0.otId & 0xFFFFu);
    uint8_t nick10[10]; memcpy(nick10, rec0 + 0x08, 10);
    int cnt = xrc_g3_match_identity(sb1c, frlg, NULL, dc_base, dc_stride, sp_written, otid16,
                                    nick10, &wb, &ws);
    CHECK(cnt == 1 && wb == -1 && ws == 0,
          "%s: ID-1 the identity pass finds the rerolled record at (-1,0) (got cnt=%d box=%d slot=%d)",
          path, cnt, wb, ws);

    /* D4 negatives (review D4): identity_matches_rec() compares species AND otId
     * (both plaintext/decoded fields) AND the 10 raw nickname bytes -- a mismatch
     * in EITHER of the first two alone must refuse the match (cnt == 0), proving
     * neither comparison term is dead. Reuses the SAME rerolled record/nick10 so
     * only one field differs from the true positive above. */
    uint16_t sp_wrong = (uint16_t)(sp_written == 1 ? 2 : sp_written - 1);
    int cnt_sp = xrc_g3_match_identity(sb1c, frlg, NULL, dc_base, dc_stride, sp_wrong, otid16,
                                       nick10, &wb, &ws);
    CHECK(cnt_sp == 0, "%s: ID-1 D4 a species mismatch does not match by identity (got cnt=%d)",
          path, cnt_sp);

    uint16_t otid_wrong = (uint16_t)(otid16 ^ 0xFFFFu);
    int cnt_otid = xrc_g3_match_identity(sb1c, frlg, NULL, dc_base, dc_stride, sp_written, otid_wrong,
                                         nick10, &wb, &ws);
    CHECK(cnt_otid == 0, "%s: ID-1 D4 an otId mismatch does not match by identity (got cnt=%d)",
          path, cnt_otid);
  }
}

int main(int argc, char** argv) {
  printf("== xfer_reconcile ==\n");
  test_cls1();
  test_bank1();
  test_bankg3();
  test_g3home_parked();
  test_phase2();
  test_order1();
  test_rekey1();
  test_row1();
  test_rebuild1();
  test_inbank1();
  test_promote1();

  int examined = 0;
  for (int i = 1; i < argc; i++) { test_corpus(argv[i]); examined++; }
  if (!examined) printf("  (no .sav given on argv -- KEY-1/ID-1 corpus cases skipped)\n");

  printf("\n%s: %d check(s), %d failure(s)\n", g_fail ? "FAIL" : "OK", g_check, g_fail);
  return g_fail ? 1 : 0;
}

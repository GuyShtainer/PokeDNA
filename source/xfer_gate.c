#include "xfer_gate.h"
#include "bank_cell.h"      /* bc_is_native -- xg_native_escape_denied (BACKLOG #150 S150-3) */

#define XG_SCOPE_BANK 1u
#define XG_SCOPE_GB   2u

bool xg_pc_live(bool vinfo_valid, bool arena_held) { return vinfo_valid && !arena_held; }

bool xg_togame_row(bool is_bank, bool pc_live, bool have_pc) { return is_bank && pc_live && have_pc; }

bool xg_paste_row(bool clip_occupied, bool pc_live) { return clip_occupied && pc_live; }

bool xg_create_row(bool is_bank, bool pc_live) { return !is_bank || pc_live; }

bool xg_inject_refuse(bool arena_held, bool vinfo_valid) { return arena_held || !vinfo_valid; }

bool xg_clear_carry_on_gb_exit(bool carry_is_gb) { return carry_is_gb; }

bool xg_drop_denied(uint8_t dst_scope, uint8_t src_scope, bool have_xfer) {
  /* BACKLOG #150 S150-4 decision 9: exactly one allow-rule added to the S2 refusal --
   * BANK<-GB, and only when both sides actually have an xfer vtable (the vtable half
   * cannot be seen from a pure predicate, so the caller hands it in). Every other
   * GB-involving pair stayed denied UNTIL BACKLOG #246 added the second, symmetric
   * allow-rule below. */
  if (dst_scope == XG_SCOPE_BANK && src_scope == XG_SCOPE_GB && have_xfer) return false;
  /* BACKLOG #246 (#104 Phase 1): the ONLY new pair this lane opens -- a Gen-3 Bank
   * cell dropped onto a Game Boy destination, re-attaching gen3_to_gb.c to a source
   * for the first time since #239 closed the old COPY -> NV_GB mount -> PASTE route.
   * `have_xfer` is the same real-vtable test as the sibling rule above (pdna_box.c's
   * drop_held computes it once, before either check); the caller (drop_held's new
   * cross-scope branch) additionally requires the held cell to be a plain Gen-3
   * record, never a native "GBC1" cell -- a native cell reaching a GB destination is
   * intercepted earlier, by the bc_is_native-gated DOWN-arm dispatch block, and never
   * reaches this predicate at all (see that block's own comment in pdna_box.c). Every
   * OTHER GB-involving pair (PC<-GB, GB<-PC, PC<-BANK-via-GB, etc.) is UNCHANGED --
   * still denied by the base rule below. */
  if (dst_scope == XG_SCOPE_GB && src_scope == XG_SCOPE_BANK && have_xfer) return false;
  return dst_scope == XG_SCOPE_GB || src_scope == XG_SCOPE_GB;
}

bool xg_native_escape_denied(const uint8_t rec80[80], uint8_t dst_scope) {
  return bc_is_native(rec80) && dst_scope != XG_SCOPE_BANK;
}

bool xg_chunk_crossgen_denied(uint8_t dst_scope, uint8_t chunk_scope) {
  return dst_scope == XG_SCOPE_GB || chunk_scope == XG_SCOPE_GB;
}

uint8_t xg_bank_down_arm(uint8_t cell_gen, uint8_t dst_scope, uint8_t dst_gen) {
  if (cell_gen != 1u && cell_gen != 2u) return XG_DOWN_ARM_NONE;
  if (dst_scope == XG_SCOPE_BANK) return XG_DOWN_ARM_NONE;
  if (dst_scope != XG_SCOPE_GB) return XG_DOWN_ARM_GEN3;      /* BOXSCOPE_PC (0) */
  if (dst_gen != 1u && dst_gen != 2u) return XG_DOWN_ARM_NONE; /* no GB session   */
  return (dst_gen == cell_gen) ? XG_DOWN_ARM_EXACT : XG_DOWN_ARM_GB_BRIDGE;
}

/* BACKLOG #150 S150-12 decision 9: a native cell whose GB original still exists (the
 * read-only mount's COPY lift) never had its own bytes deleted from the GB save, so a
 * DOWN of it must never write a Gen-3 ledger entry (that entry would key on the SAME
 * gbsc_key the still-living original would derive -- SS11.7 G-L3, the clone-claims-
 * the-original hole). bc_is_native() first: a non-native cell (e.g. an ordinary Gen-3
 * mon) has an unrelated meaning for byte 11 and must never be asked this question. */
bool xg_cell_is_copy(const uint8_t rec80[80]) {
  return bc_is_native(rec80) && (rec80[BC_OFF_FLAGS] & BC_FLAG_COPY) != 0;
}

/* BACKLOG #150 S150-12 decision 13: the read-only mount's one exit offer fires only
 * when something is actually queued AND a live Gen-3 PC exists to receive it -- the
 * two halves of "would the offer make sense right now", kept as a trivial pure
 * predicate so the full 4-row truth table can be pinned on the host. */
bool xg_pc_offer(uint8_t queued, bool pc_live) {
  return queued > 0 && pc_live;
}

/* BACKLOG #239: see xfer_gate.h's own comment. Consulted from exactly one place,
 * source/nav_avail.c's nav_avail(NV_GB, ...) -- both of pdna_main.c's NV_GB call sites
 * (the plain-FIL pdna_gen12_show() and the PDNA_DELTA pdna_gen12_show_fused() arm) sit
 * behind that SAME nav_avail() check already (BACKLOG #58's own "no row dispatches
 * without asking first" contract), so gating the row here closes both without a second
 * call site. Hardcoded false, not state: golden rule 3, nothing to allocate or persist
 * for a predicate this simple. */
bool xfer_direct_allowed(void) { return false; }

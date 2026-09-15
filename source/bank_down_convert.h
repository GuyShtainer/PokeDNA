#ifndef BANK_DOWN_CONVERT_H
#define BANK_DOWN_CONVERT_H

#include <stdint.h>
#include <stdbool.h>
#include "bank_cell.h"      /* BcMeta, BC_CELL_BYTES                                    */
#include "gen12_convert.h"  /* Gb12Mon, Gb12Result, Gb12Notes                           */
#include "gen3_to_gb.h"     /* Gen3ToGbLoss, G3GbStatus, GbGen1Base                     */
#include "pdna_box.h"       /* BoxSource -- the arms' GBA-facing signature              */

/* BACKLOG #150 S150-8 / orchestrator D-Q1: the widened DOWN-dispatch outcome.
 * REFUSED/LANDED keep S150-7's own meaning (docs/briefs/s150-7-bank-down-exact-brief.md
 * D1); CONVERTED is this lane's third outcome. Unlike the pre-D-Q1 draft of this brief,
 * there is no trailing `out80` parameter: the arm that returns CONVERTED has ALREADY
 * written the converted record into its destination itself (bank_down_convert_gen3
 * writes straight into the Gen-3 PC box through `src`; bank_down_convert_gb writes
 * straight into the mounted Game Boy session through gbs_insert/gb_persist) -- the
 * caller's only remaining job is to consume the Bank origin (pdna_box.c decision 11). */
typedef enum { BANK_DOWN_REFUSED = 0, BANK_DOWN_LANDED = 1, BANK_DOWN_CONVERTED = 2 } BankDownResult;

/* ============================================================================
 * PURE cores -- no FatFs, no tonc/sys.h, host-testable (tests/host_xferdown_test.c).
 * These do the actual species/DV/move/item math; the GBA-facing arms below are a
 * thin shell around them that adds the ledger write, the UI screens and the
 * destination write, none of which can run on the host.
 * ============================================================================ */

/* Arm 2's pure core (decision 3+5+6): bc_unpack -> bc_view -> item map -> gen12_convert,
 * with the held-item relax applied BEFORE gen12_convert so GB12_ERR_HELD_ITEM can never
 * fire (decision 6 -- the item is never lost, it stays in original80/cell80; only the
 * Gen-3 record drops it when there is no legal home for it on `met_game`). `g3_item` is
 * the id actually written into the record (0 if the item did not travel and nothing was
 * held to begin with). Returns GB12_OK on success; `out`/`written`/`notes`/`g3_item` are
 * meaningful only then (mirrors gen12_convert's own "out untouched on refusal" contract).
 * `written` is the cell's own unpacked GbEditMon -- decision 8's ledger `written` param
 * for this arm (there is no Game-Boy-shaped record abroad to describe it with). */
Gb12Result bdc_convert_gen3_core(const uint8_t cell80[BC_CELL_BYTES], uint8_t met_game,
                                 uint8_t out80[80], GbEditMon* written, Gb12Notes* notes,
                                 uint16_t* g3_item);

/* Arm 1's pure core (decision 14+15): the time-capsule check first (dst_gen's floor),
 * then the item relax (a Gen-2 item can never reach Gen 1; zeroed, noted, named on the
 * loss screen -- the record keeps it), then gen12_convert(tgt.met_game=0, "the
 * intermediate never lands anywhere") into a scratch Gen-3 record, then gen3_to_gb.
 * `*tc` is xr_time_capsule_block's own return (0 = passed, checked FIRST -- everything
 * below is untouched when `*tc != 0`); `*g12` is gen12_can_convert's refusal (GB12_OK on
 * success); `*g3gb` is gen3_to_gb's status, including G3GB_ERR_NEEDS_BASE (Gen-1 target
 * with `g1base == NULL` -- the caller retries with a located base table, exactly
 * gb_paste_hook's own two-try shape). `out`/`loss`/`notes` are meaningful only when
 * `*tc == 0 && *g12 == GB12_OK && *g3gb == G3GB_OK`. */
void bdc_convert_gb_core(const uint8_t cell80[BC_CELL_BYTES], uint8_t dst_gen,
                         bool caught_available, const GbGen1Base* g1base,
                         int* tc, uint16_t* tc_bad, Gb12Result* g12, G3GbStatus* g3gb,
                         GbEditMon* out, Gen3ToGbLoss* loss, Gb12Notes* notes);

/* ============================================================================
 * GBA-facing arms -- called ONLY from pdna_box.c's bank_down_dispatch, one per
 * BANK_DOWN_* outcome this lane owns. Both do real card I/O (the ledger write,
 * gbs_insert/gb_persist or the PC box write) and drive UI screens (the loss
 * screen, the KEEP AS IS / MAKE LEGAL modal) -- neither is host-testable; their
 * pure innards above are what tests/host_xferdown_test.c proves.
 * ============================================================================ */

/* Arm 1: the native cell bridges into the CURRENTLY MOUNTED Game Boy session's OTHER
 * generation (Gen-2 cell -> a Gen-1 save, or Gen-1 cell -> a Gen-2 save, always
 * allowed the other way). `dst_box` is the destination box within that session the
 * cursor is on; `dst_cell`/`src` are accepted for signature symmetry with the Gen-3
 * arm but the Game Boy destination is not slot-addressable the way a Gen-3 grid is
 * (gbs_insert assigns the slot itself, exactly as gb_paste_hook already does).
 * Returns BANK_DOWN_LANDED on success (the destination is already written and
 * persisted) or BANK_DOWN_REFUSED (a message has already been shown; the hand keeps
 * the native cell). Never returns BANK_DOWN_CONVERTED. */
BankDownResult bank_down_convert_gb(BoxSource* src, int dst_box, int dst_cell,
                                    const uint8_t cell80[80]);

/* Arm 2: the native cell converts into a real Gen-3 record for the Gen-3 PC box
 * `dst_box`/`dst_cell` addresses. DEVIATION from the D-Q1 shape the bridge arm
 * above uses: this arm does NOT call any `src->*` member itself and does NOT touch
 * the card in the PC's own box buffer -- `tools/stack_budget.py`'s walker refuses
 * to certify a NEW caller of a BoxSource function-pointer field it has no
 * declaration for (tools/stack_edges.txt's `BoxSource.records @20 in
 * clear_origin,export_box_all,party_strip_overlay,release_box_all -> ...` and
 * `.note_add @56 in drop_held -> ...` are both qualified to SPECIFIC existing
 * caller functions, and this brand-new function is not one of them; GATED ok must
 * stay 4, and this lane may add no new stack_edges.txt row -- confirmed by an
 * actual build failure, see the S150-8 delivery report). So `dstrec` (the 80
 * bytes already at the destination cell, read-only, from the CALLER's own already-
 * loaded `recs`) is an INPUT for the occupancy check, and `out80` is an OUTPUT: on
 * BANK_DOWN_CONVERTED the caller (pdna_box.c's drop_held, which ALREADY calls
 * `src->note_add`/`src->mark_dirty` as a declared caller) does the actual
 * `memcpy`/`note_add`/`mark_dirty`, exactly the shape the ordinary BANK->PC true
 * MOVE branch beside it already uses. Never returns BANK_DOWN_LANDED. */
BankDownResult bank_down_convert_gen3(BoxSource* src, int dst_box, int dst_cell,
                                      const uint8_t cell80[80], const uint8_t dstrec[80],
                                      uint8_t out80[80]);

#endif /* BANK_DOWN_CONVERT_H */

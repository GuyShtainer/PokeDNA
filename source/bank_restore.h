#ifndef BANK_RESTORE_H
#define BANK_RESTORE_H

#include <stdint.h>
#include "gb_sidecar.h"   /* GbscEntry -- gb_sidecar.h's own entry-layout comment    */
#include "xfer_rec.h"     /* XrMergeReport                                            */

/* BACKLOG #150 S150-8b, ORCHESTRATOR DECISION D-Q1: the RESTORE edge's pure core,
 * in its own file so BOTH call sites (drop_held's PC->Bank arm in pdna_box.c, and
 * the GB lift's narrowed native-home case in pdna_gen12.c -- hop 3 of Guy's
 * 2->3->1->2, a follow-up lane per D-Q1's own stop-licence) share one
 * implementation. Pure C: no tonc.h, no sys.h, no FatFs (hard rule 5) -- it never
 * opens the ledger file itself (that lookup, and allocating `bank_serial` via
 * pdna_bank_next_serial(), are the CALLER's job, because both are file I/O a pure
 * core must not perform) and it never writes anywhere.
 *
 * `e` is the ledger entry the CALLER already found (the highest-index entry whose
 * kind is XR_KIND_NATIVE_HOME, gb_sidecar.h's own tiebreak rule) for the mon
 * `g3_rec80` currently is. `bank_serial` is a FRESH allocation from
 * pdna_bank_next_serial() (S150-4/5) -- 0 is never a valid serial, so passing 0 is
 * itself a refusal (the caller's allocation failed; nothing is written by either
 * side). `rep_out` may be NULL; when non-NULL it receives xr_merge_down()'s own
 * report, for a caller that wants to show the shipped confirm screen (decision 6)
 * before committing to the rebuilt cell.
 *
 * Returns:
 *   1  -- `out_cell80` holds the rebuilt NATIVE cell (home's own origin_game/flags
 *         carried forward, rtc_epoch passed through UNCHANGED -- this core has no
 *         RTC access, decision 8's own escape hatch for "if it is not available").
 *         Write THAT, not `g3_rec80`.
 *   0  -- `e->kind` is not XR_KIND_NATIVE_HOME: not this edge's job, the caller's
 *         own dispatch is wrong if it got here (defensive; the real "no entry"
 *         case is the caller simply never finding one to pass in).
 *  -1  -- refuse: `e->original80` is not actually a native cell (G-F4/G-H6 belt and
 *         braces), `bank_serial` is 0, `bc_unpack`/`xr_merge_down`/`bc_pack` failed.
 *         `out_cell80` is left untouched; nothing is written anywhere by this
 *         function -- the caller keeps holding the mon exactly as it was. */
int bank_restore_from_entry(const GbscEntry* e, const uint8_t g3_rec80[80],
                            uint32_t bank_serial, uint8_t out_cell80[80],
                            XrMergeReport* rep_out);

#endif

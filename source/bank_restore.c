#include "bank_restore.h"
#include "bank_cell.h"   /* bc_is_native, bc_unpack, bc_pack, BcMeta                */

int bank_restore_from_entry(const GbscEntry* e, const uint8_t g3_rec80[80], uint8_t accept,
                            uint32_t bank_serial, uint8_t out_cell80[80],
                            XrMergeReport* rep_out) {
  if (!e || !g3_rec80 || !out_cell80) return -1;
  if (e->kind != XR_KIND_NATIVE_HOME) return 0;
  if (bank_serial == 0) return -1;
  /* G-F4/G-H6, belt and braces: the kind byte is absent on pre-#150 entries, so a
   * caller filtering on kind alone still needs this independent check. */
  if (!bc_is_native(e->original80)) return -1;

  BcMeta meta;
  GbEditMon home;
  if (!bc_unpack(e->original80, &home, &meta)) return -1;

  GbEditMon merged;
  XrMergeReport local_rep;
  XrMergeReport* rep = rep_out ? rep_out : &local_rep;
  if (!xr_merge_down_sel(e, g3_rec80, accept, &merged, rep)) return -1;

  /* Carry the home's own origin_game and flags forward (with BC_FLAG_HAS_XFER_REC
   * added, decision 3 -- a restored cell is, by definition, one the ledger carried
   * home); re-stamp rtc_epoch only if an RTC source is available on this path -- it
   * is not (this core is pure C, no hardware access), so meta.rtc_epoch passes
   * through unchanged, per decision 8's own escape hatch (docs/BANK-CROSSGEN-DESIGN.md
   * S11.20 item 11). */
  if (bc_pack(&merged, (uint8_t)(meta.flags | BC_FLAG_HAS_XFER_REC), meta.origin_game,
              meta.rtc_epoch, bank_serial, out_cell80) != 0) {
    return -1;
  }
  return 1;
}

int bank_restore_from_entry_gb(const GbscEntry* e, const GbEditMon* now, uint8_t accept,
                               uint32_t bank_serial, uint8_t out_cell80[80],
                               XrMergeReport* rep_out) {
  if (!e || !now || !out_cell80) return -1;
  if (e->kind != XR_KIND_NATIVE_HOME) return 0;
  if (bank_serial == 0) return -1;
  if (!bc_is_native(e->original80)) return -1;

  BcMeta meta;
  GbEditMon home;
  if (!bc_unpack(e->original80, &home, &meta)) return -1;

  GbEditMon merged;
  XrMergeReport local_rep;
  XrMergeReport* rep = rep_out ? rep_out : &local_rep;
  if (!xr_merge_down_gb_sel(e, now, accept, &merged, rep)) return -1;

  if (bc_pack(&merged, (uint8_t)(meta.flags | BC_FLAG_HAS_XFER_REC), meta.origin_game,
              meta.rtc_epoch, bank_serial, out_cell80) != 0) {
    return -1;
  }
  return 1;
}

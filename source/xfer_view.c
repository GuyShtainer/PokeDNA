/*
 * xfer_view -- BACKLOG #150 S150-15: read-only lookup of a converted Gen-1/2 mon's
 * ORIGINAL (native) bytes. See xfer_view.h for the contract. No write primitive is
 * called anywhere below -- tests/host_xfer_view_readonly_test.py greps this file for
 * every one and fails if it finds any.
 */
#include "xfer_view.h"
#include "xfer_io.h"
#include "xfer_rec.h"      /* xr_key_g3                                              */
#include "gb_sidecar.h"    /* GbscEntry, GBSC_FILE_MAX, XR_KIND_NATIVE_HOME           */
#include "bank_cell.h"     /* bc_is_native, BC_OFF_ORIGIN_GAME, BC_ORIGIN_*           */
#include "savefile.h"      /* SfStatus, sf_status_str                                */
#include "log.h"
#include "pdna_layout.h"   /* PDNA_XFER_GAME_.. and PDNA_XFER_ORIG_.. format strings   */
#include <string.h>
#include <stdio.h>          /* siprintf (host: -Dsiprintf=sprintf)                    */

/* out <- e.original80, e.rtc_epoch, e.original80[8]/[BC_OFF_ORIGIN_GAME], e.state. */
static void fill_from_entry(const GbscEntry* e, XvOriginal* out) {
  memcpy(out->original80, e->original80, 80);
  out->rtc_epoch   = e->rtc_epoch;
  out->gen         = e->original80[8];
  out->origin_game = e->original80[BC_OFF_ORIGIN_GAME];
  out->state       = e->state;
}

/* xr_path_for_key's own contract: "Returns true iff a file exists at the path it
 * wrote" -- decision 4's ONE f_stat, no parse, no separate xr_open. */
bool xv_has_original(const uint8_t rec80[80]) {
  if (!rec80) return false;
  char path[GBSC_PATH_MAX];
  return xr_path_for_key(path, xr_key_g3(rec80));
}

/* decision 5/8: the s150-8b walk verbatim (source/pdna_box.c's pc_bank_restore_up),
 * minus its restore-specific tail (no xr_merge_down, no confirm, no write). `noinline`
 * + its own 1042-B stack frame, decision 10's two-phase shape. */
int __attribute__((noinline)) xv_find_original(const uint8_t rec80[80], XvOriginal* out) {
  if (!rec80 || !out) return -1;
  uint8_t buf[GBSC_FILE_MAX];
  uint32_t len = 0;
  uint64_t key = xr_key_g3(rec80);
  SfStatus rst = xr_open(key, buf, sizeof buf, &len, NULL);
  if (rst == SF_ERR_OPEN) return 0;              /* no ledger file at all -- an ordinary mon */
  if (rst != SF_OK) {
    log_line("xfer: view: open: %s", sf_status_str(rst));
    return -1;
  }
  int count = gbsc_count(buf, len);
  if (count < 0) {
    log_line("xfer: view: ledger file failed to validate");
    return -1;
  }

  int best = -1;
  GbscEntry e;
  for (int i = 0; i < count; i++) {
    GbscEntry cand;
    if (!gbsc_get(buf, len, i, &cand)) continue;
    if (cand.kind != XR_KIND_NATIVE_HOME) continue;
    if (!bc_is_native(cand.original80)) continue;
    best = i;
    e = cand;
  }
  if (best < 0) return 0;   /* only G3_HOME entries, or the file is empty -- decision 4's caveat */

  fill_from_entry(&e, out);
  return 1;
}

const char* xv_origin_name(uint8_t origin_game) {
  switch (origin_game) {
    case BC_ORIGIN_RED:     return PDNA_XFER_GAME_RED;
    case BC_ORIGIN_BLUE:    return PDNA_XFER_GAME_BLUE;
    case BC_ORIGIN_YELLOW:  return PDNA_XFER_GAME_YELLOW;
    case BC_ORIGIN_GOLD:    return PDNA_XFER_GAME_GOLD;
    case BC_ORIGIN_SILVER:  return PDNA_XFER_GAME_SILVER;
    case BC_ORIGIN_CRYSTAL: return PDNA_XFER_GAME_CRYSTAL;
    default:                return NULL;    /* BC_ORIGIN_UNKNOWN or any unmapped value */
  }
}

/* decision 8: mirror of pdna_gen12.c's xfer_down_write epoch decoder --
 * year = 2000 + (e >> 26), month = (e >> 22) & 15, day = (e >> 17) & 31. */
void xv_format_note(char out[24], uint8_t gen, uint8_t origin_game, uint32_t rtc_epoch) {
  if (!out) return;
  const char* name = xv_origin_name(origin_game);
  unsigned year  = 2000u + (rtc_epoch >> 26);
  unsigned month = (rtc_epoch >> 22) & 15u;
  unsigned day   = (rtc_epoch >> 17) & 31u;
  unsigned yy    = year % 100u;
  if (name) {
    if (rtc_epoch) siprintf(out, PDNA_XFER_ORIG_NOTE_FMT, name, yy, month, day);
    else           siprintf(out, PDNA_XFER_ORIG_NODATE_FMT, name);
  } else {
    /* origin_game has no name (BC_ORIGIN_UNKNOWN or unmapped) -- fall back to the
     * GENERATION byte, which xv_find_original always has (original80[8]). */
    char gbuf[8];
    siprintf(gbuf, PDNA_XFER_ORIG_GEN_FMT, (unsigned)gen);
    if (rtc_epoch) siprintf(out, PDNA_XFER_ORIG_NOTE_FMT, gbuf, yy, month, day);
    else           siprintf(out, PDNA_XFER_ORIG_NODATE_FMT, gbuf);
  }
}

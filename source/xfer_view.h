#ifndef XFER_VIEW_H
#define XFER_VIEW_H

#include <stdint.h>
#include <stdbool.h>

/* BACKLOG #150 S150-15: read-only lookup of a converted Gen-1/2 mon's ORIGINAL
 * (native, pre-loss) bytes, for the Gen-3 mon menu's "GB ORIGINAL" row. May include
 * ff.h/savefile.h/log.h/pdna_layout.h; never tonc.h/sys.h (source/xfer_io.h's own
 * rule, xfer_io.h:9-11). No write primitive is called anywhere in this file -- the
 * structural test (tests/host_xfer_view_readonly_test.py) enforces it.
 *
 * This lane never edits xfer_io, xfer_rec, gb_sidecar or bank_cell (any of their .c/.h
 * files) -- only calls their existing exports. */

/* One resolved ORIGINAL record, decoded straight off the ledger entry's own bytes
 * (no bc_unpack here -- the Game Boy summary itself unpacks `original80`). */
typedef struct {
  uint8_t  original80[80];   /* the native cell verbatim, as xfer_down_write filed it */
  uint32_t rtc_epoch;        /* the transfer time, packed per gb_sidecar.h's own format;
                               * 0 = absent by convention (no RTC at transfer time) */
  uint8_t  gen;               /* original80[8] -- 1 or 2 */
  uint8_t  origin_game;       /* original80[BC_OFF_ORIGIN_GAME] -- BC_ORIGIN_* */
  uint8_t  state;             /* XR_STATE_* -- copied for a future caller; this lane
                               * does not draw it (decision 9) */
} XvOriginal;

/* True iff a ledger file exists for `rec80`'s key (xr_key_g3) -- ONE f_stat, no
 * parse. Does NOT guarantee the file holds a NATIVE_HOME entry (decision 4's stated
 * caveat: a G3_HOME-only file, or a legacy/evicted one, also returns true here; A on
 * the row then answers with the "no original" plaque). `rec80` may be an 80-byte
 * box/Bank record or the first 80 bytes of a 100-byte party record -- xr_key_g3
 * reads only bytes 0..7. */
bool xv_has_original(const uint8_t rec80[80]);

/* Resolve `rec80`'s ORIGINAL, if any. Walks every entry in the ledger file, keeping
 * the HIGHEST index whose kind is XR_KIND_NATIVE_HOME and whose original80 passes
 * bc_is_native() (decision 5/8, the s150-8b walk verbatim, belt-and-braces since the
 * kind byte is absent on pre-#150 entries). `noinline`, its own GBSC_FILE_MAX (1042 B)
 * stack frame -- callers hold it for the shortest time they can (decision 10's
 * two-phase shape).
 *
 * Returns 1 and fills `*out` on a match; 0 when the file is absent (SF_ERR_OPEN) or
 * present and readable but the walk finds no XR_KIND_NATIVE_HOME entry (decision 4's
 * caveat: a G3_HOME-only file); -1 on an actual read/validate failure -- a non-OK
 * `SfStatus` other than SF_ERR_OPEN, or `gbsc_count` rejecting the bytes (a truncated
 * or corrupted file) -- logged via log_line, `"xfer: view: ..."`, decision 11. */
int xv_find_original(const uint8_t rec80[80], XvOriginal* out);

/* NULL for BC_ORIGIN_UNKNOWN (or any value outside the known table) -- the caller
 * then falls back to the PDNA_XFER_ORIG_GEN_FMT "GEN %u" form (decision 8). */
const char* xv_origin_name(uint8_t origin_game);

/* Composes the ORIGIN card's note line into `out[24]` (decision 8): "<GAME> yy-mm-dd"
 * when both a name and a non-zero epoch are known, "<GAME> (no date)" when the name
 * is known but rtc_epoch == 0, "GEN <n> yy-mm-dd" / "GEN <n> (no date)" when
 * origin_game has no name. Always NUL-terminated; the caller's own textfit tests
 * (host_textfit_test.c) prove every composed worst case fits PDNA_SUM_CARD_W. */
void xv_format_note(char out[24], uint8_t gen, uint8_t origin_game, uint32_t rtc_epoch);

#endif

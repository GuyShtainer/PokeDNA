#ifndef FLAGS_FOLD_H
#define FLAGS_FOLD_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_flags.h"   /* NamedFlag, NAMED_FLAG_HEADER */

/* BACKLOG #2a: pure row-visibility math for the Gen-3 data editor's collapsible
 * FLAGS list, factored out of pdna_main.c (nf_cache/nf_hdr_ord/nf_visible/
 * nf_step) so tests/host_flagsfold_test.c can drive it directly on the host --
 * pdna_main.c's own wrappers call these exact functions and keep their session
 * state (s_nf_for/s_nf_ord/s_flags_folded) exactly where it was, so there is no
 * second copy of this logic to drift out of sync with what ships. This module
 * is pure C (no GBA headers, no static/global state of its own): every input is
 * a parameter, which is what makes it host-testable.
 *
 * A NamedFlag table (gen3_flags.h) is a flat list of rows where a header row
 * (num == NAMED_FLAG_HEADER) starts a new "fold group" running up to (not
 * including) the next header or the end of the table. `ord[r]` is the 0-based
 * ordinal of the header that owns row r; a `folded` bitmask has bit k set when
 * fold group k is collapsed -- every row inside it is hidden except the header
 * itself (headers are always visible, so their own +/- marker and the section
 * title stay reachable to toggle back open). The trailing "raw flag browser"
 * row pdna_main.c appends after the table (index == nc) is always visible too;
 * it is not part of any fold group. */

#define FF_MAX_GROUPS 32   /* bits available in a uint32_t fold mask */

/* Fill ord[0 .. min(nc, ord_cap)) with each row's owning header ordinal,
 * clamped to FF_MAX_GROUPS-1 past that many headers (a row past the cap folds
 * under the LAST group rather than an assert -- matching the "silently
 * un-folds" tradeoff pdna_main.c's own comment documents, since a fold mask
 * only has 32 bits to give out). Rows before the first header (a malformed
 * table -- every real table starts with a "Badges" header) get ordinal 0. A
 * NULL `nf`/`ord` or ord_cap <= 0 is a no-op. */
void ff_build_ord(const NamedFlag* nf, int nc, uint8_t* ord, int ord_cap);

/* Ordinal of the header owning row r, as built by ff_build_ord into `ord`
 * (capacity ord_cap). Out-of-range r (or a NULL ord) reads as group 0 rather
 * than reading out of bounds -- mirrors pdna_main.c's own nf_hdr_ord. */
int ff_hdr_ord(const uint8_t* ord, int ord_cap, int r);

/* True if row r should be drawn given the current fold mask: row nc (the
 * trailing raw-browser row) and every header row are always visible; any
 * other row is visible iff its owning group's bit in `folded` is clear. A
 * NULL `nf` is treated as an empty table (every row "visible" is moot since
 * r >= nc is always true then). */
bool ff_row_visible(const NamedFlag* nf, int nc, const uint8_t* ord, int ord_cap,
                     uint32_t folded, int r);

/* Next (dir > 0) or previous (dir < 0) VISIBLE row from r over [0, total)
 * (total == nc + 1, counting the trailing raw-browser row). Returns r itself
 * if there is no visible row further in that direction -- a top/bottom stop,
 * never an out-of-range index, so callers can assign the result straight back
 * to a cursor variable unconditionally. dir == 0 also returns r unchanged. */
int ff_step(const NamedFlag* nf, int nc, const uint8_t* ord, int ord_cap,
            uint32_t folded, int total, int r, int dir);

#endif /* FLAGS_FOLD_H */

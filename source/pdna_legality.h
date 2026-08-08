#ifndef PDNA_LEGALITY_H
#define PDNA_LEGALITY_H

#include <stdint.h>
#include "gen3_mon.h"

/* Legality report screen — READ-ONLY analysis, Legality V2 (gen3_legality2.h).
 *
 * A verdict banner (OK / QUESTIONABLE / ILLEGAL), the findings grouped by category
 * with a per-row severity colour, an on-demand PID/IV RNG check (A), a box-wide
 * sweep of all 30 cells (R), and a help page (START). B returns.
 *
 * This screen NEVER writes: no save block is touched, no commit function is called,
 * and nothing here can change a Pokemon. That is a promise the help page makes to
 * the user, so keep it true.
 */

/* Single Pokemon, no box context: the sweep entry is hidden. */
void pdna_legality_show(const PkMon* m);

/* With a box context, which enables the BOX SWEEP.
 *
 *   `block` is a PC-LAYOUT buffer: a 4-byte header then 30 x 80-byte box records
 *           per box — i.e. exactly what app_mon_menu is handed (g_pc for a PC box,
 *           the bank's mini-block for a bank box). NULL disables the sweep.
 *   `box`   the box index inside `block` (0 for the bank's mini-block). < 0
 *           disables the sweep — party records are 100 bytes and live elsewhere.
 *   `slot`  the cell `m` came from (0..29), so the sweep can mark it. -1 if unknown.
 *
 * Returns the slot of the Pokemon the user was LAST looking at (they can re-target
 * the screen from the sweep list), or -1 if that is not a cell of this box. A caller
 * may use it to move its own cursor; ignoring it is fine.
 */
int pdna_legality_show_box(const PkMon* m, const uint8_t* block, int box, int slot);

#endif /* PDNA_LEGALITY_H */

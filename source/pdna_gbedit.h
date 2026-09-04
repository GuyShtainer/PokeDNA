#ifndef PDNA_GBEDIT_H
#define PDNA_GBEDIT_H

#include <stdbool.h>
#include "gb_edit.h"

/* Field-edit screen for ONE Game Boy Pokemon — the Gen-1/2 twin of pdna_edit().
 *
 * Edits `e` IN PLACE with a live preview; the rows, their text and what the d-pad
 * does to them come from gb_editor.c (pure C, host-tested), this file only draws,
 * opens the keyboard / move picker, and asks before it returns.
 *
 * START -> confirm -> returns true with `e` holding the edited record (party stats
 * already recalculated where this tree can, gb_editor.h gbe_settle_stats). B returns
 * false; the caller should then discard `e` and reload from the list — nothing is
 * restored here. It does NOT touch the list, the image or the SD: the caller commits
 * `e` into its list blob with gb_commit_checked(), the blob into the image with
 * gbs_commit_list(), and the image to the card with sf_write_verified().
 *
 * `note` (may be NULL) is one short line drawn under the header — the session's
 * honest line about what is being edited.
 *
 * `has_sidecar` (S5-B, docs/GEN3-TO-GB-SIDECAR-DESIGN.md section 10): true when `e`'s
 * CURRENT key already has a /PokeDNA/sidecar/<key>.pds entry (pdna_gen12.c's
 * gb_has_sidecar, computed by the caller since it owns the FatFs call this pure-UI
 * file does not make). dv4 is part of that key (gb_sidecar.h), so editing a DV here
 * moves `e` to a DIFFERENT key the next gbsc_find() will never associate with this
 * sidecar again -- orphaning it. On the FIRST adjust/press of a DV row this shows a
 * one-time warning (A to continue) before letting the edit through; every DV edit
 * after the first in the same visit proceeds silently, since the sidecar is already
 * orphaned by then and a second warning would say nothing new. */
bool pdna_gbedit(GbEditMon* e, const char* note, bool has_sidecar);

#endif /* PDNA_GBEDIT_H */

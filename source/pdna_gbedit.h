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
 * honest line about what is being edited. */
bool pdna_gbedit(GbEditMon* e, const char* note);

#endif /* PDNA_GBEDIT_H */

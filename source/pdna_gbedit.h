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

/* ---- shared with pdna_gbsummary.c (BACKLOG #41) ---------------------------------
 * The Gen-1/2 summary editor mirrors this screen's row dispatch and confirm-before-
 * write panel rather than re-deriving (and risking drifting from) the same rules, so
 * the pieces that are not pure drawing are exposed here instead of duplicated. Every
 * one of these draws on screen (msg_wait/osk_input/pick_move/ui_*), which is why they
 * live in this file and not gb_editor.h's pure-C row model. */

/* dv4 is part of the sidecar's own fingerprint (gb_sidecar.h gbsc_key) — true for the
 * four STORED DVs (Atk/Def/Spe/Spc); the derived HP DV (GBE_DVH) is excluded since it
 * cannot itself be the edit that orphans anything. */
bool gbedit_is_dv_field(int f);
/* Shown once per editor visit on the FIRST adjust/press of a DV row while `has_sidecar`
 * is true, then never again (`*warned` is the caller's own one-shot latch). */
void gbedit_dv_orphan_warn(bool has_sidecar, bool* warned);
/* d-pad/L/R adjust, DV-warning-checked — the shared tail of every LEFT/RIGHT/L/R
 * branch that touches a row: warn once on a DV row, then gbe_adjust(). */
void gbedit_adjust_checked(GbEditMon* e, int f, int dir, bool big,
                            bool has_sidecar, bool* dv_warned);
/* A on a row: TEXT opens the keyboard, MOVE opens the move picker, NUM presses via
 * gbe_press — each through the DV-warning check first. Reports a refusal (bad
 * charset / an in-a-later-generation or duplicate move) with the same messages this
 * screen shows. */
void gbedit_press(GbEditMon* e, int f, bool has_sidecar, bool* dv_warned);
/* START/B-with-unsaved-edits confirm panel: names the mon, shows gb_check()'s first
 * issue (if any, with the write-anyway warning) and gbe_stale_note() (if any), then
 * asks A = write / B = cancel. Does NOT call gbe_settle_stats() itself — the caller
 * does that only once A is chosen, exactly like pdna_gbedit()'s own START handler. */
bool gbedit_confirm(const GbEditMon* e);

#endif /* PDNA_GBEDIT_H */

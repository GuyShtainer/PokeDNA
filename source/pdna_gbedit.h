#ifndef PDNA_GBEDIT_H
#define PDNA_GBEDIT_H

#include <stdbool.h>
#include "gb_edit.h"

/* Field-edit screen for ONE Game Boy Pokemon — the Gen-1/2 twin of pdna_edit().
 *
 * BACKLOG #41 (2026-09-05): the box-menu's EDIT and VIEW rows no longer call this
 * screen directly — both now open source/pdna_gbsummary.c over the same GbEditMon.
 * This screen is reachable ONLY via SELECT from inside that summary (pdna_gbsummary.c
 * itself), as the REVIEWED FALLBACK: pdna_gbsummary.c has not yet had its own
 * hardware-testing-protocol pass, so a real write can still go through this
 * already-signed-off path in the meantime. LOAD-BEARING for HW sign-off — do not
 * delete or bypass this screen until pdna_gbsummary.c has cleared its own HW run.
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
/* #362: scoped override for the held-item picker's game list: true = Crystal list even when the mon's capture
 * bytes are zero (Bank records). Set/cleared around pdna_gbsummary by native_summary_run; has_caught unchanged. */
void gbedit_set_crystal_origin(bool on);

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
 * branch that touches a row: warn once on a DV row, then gbe_adjust(). Returns
 * gbe_adjust()'s own result (G1 review LOW-1, 2026-09-08: used to be void, so a
 * legitimate refusal -- today, only GBE_GENDER: a shiny of a heavily-skewed-
 * ratio species can have no Atk DV that both flips gender and keeps the sparkle
 * -- was a silent no-op). false -> the caller should play gbedit_adjust_refused(). */
bool gbedit_adjust_checked(GbEditMon* e, int f, int dir, bool big,
                            bool has_sidecar, bool* dv_warned);
/* What a refused gbedit_adjust_checked() sounds/looks like -- shared so both
 * callers (this file's own pdna_gbedit(), pdna_gbsummary.c's card editor) say
 * the same thing the same way. Always a deny buzz; GBE_GENDER additionally
 * explains why (every other field's refusal is presently unreachable in
 * practice, so a bare buzz is enough for them). */
void gbedit_adjust_refused(int f);
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
/* Same panel, same gb_check()/gbe_stale_note() lines, Gen 3's OWN create-flow
 * wording instead (G1 review LOW-6, 2026-09-08): "Keep this Pokemon?" / "A =
 * write (backup first)" / "B = discard it" -- pdna_summary.c's confirm_keep(),
 * verbatim. The CREATE path's own keep-or-discard confirm (pdna_gbsummary.c's
 * gbsum_create_keep) uses this one instead of gbedit_confirm(). */
bool gbedit_confirm_keep(const GbEditMon* e);

#endif /* PDNA_GBEDIT_H */

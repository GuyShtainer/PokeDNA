#ifndef PDNA_GBSUMMARY_H
#define PDNA_GBSUMMARY_H

#include <stdint.h>
#include <stdbool.h>
#include "gb_edit.h"

/* pdna_gbsummary — the Gen-1/2 twin of pdna_summary.c's pdna_inspect(), restyled
 * in BACKLOG #41 slice E1 (docs/SPRITE-ERA-DESIGN.md sec 3, Guy 2026-09-05: "the
 * edit page for gen 2 and 1 should feel the same as gen 3 ... [with] less
 * editable stats") to the SAME Gen-3 CARD chrome pdna_summary.c uses — the shared
 * left info panel (portrait/name/level/type via the origin-art router), the card
 * dots, the moving-outline selection — over FOUR cards (INFO / SKILLS / MOVES /
 * ORIGIN) instead of Gen-3's eight. Reads and writes the NATIVE GbEditMon `e` —
 * never the lossy Gen-3-converted copy the box grid shows
 * (docs/GEN12-EDIT-DESIGN.md 3.3); the shared left panel's own Pokemon IS such a
 * conversion, but a THROWAWAY one, decoded fresh for display only and never
 * committed anywhere (see pdna_gbsummary.c's own header comment).
 *
 * Starts in VIEW unless `start_editing` (the EDIT row on the read-only mon menu
 * opens straight into edit mode, exactly like pdna_gbedit() used to). A enters
 * edit mode when `can_edit`; L/R flip cards; U/D (VIEW only) ask the caller for
 * the prev/next mon; B leaves, with the same "write?" confirm pdna_gbedit() used
 * (gbedit_confirm, shared — see pdna_gbedit.h). SELECT drops to the flat field-
 * list editor (pdna_gbedit.c), kept as the reviewed fallback until this screen
 * has had its own hardware pass.
 *
 * Returns 0 (exit), +1 (next mon) or -1 (prev mon) — the caller loads that slot
 * and calls again, same contract as pdna_inspect(). `saved` (may be NULL) is set
 * true iff the user confirmed a write; `e` then holds the committed record and
 * the caller runs the same gb_commit_checked -> gbs_commit_list -> gb_persist
 * chain gb_edit_hook always has (pdna_gen12.c's gb_edit_commit()).
 *
 * `card` carries the current card (0..3) in and out, exactly like pdna_inspect's
 * own `card` — scrolling to the next mon stays on the same card. May be NULL
 * (treated as card 0, not written back). `note` and `has_sidecar` are the same
 * caller-supplied strings/flag pdna_gbedit() takes (`note` is now drawn inside
 * the ORIGIN card rather than a top-right corner label). */
int pdna_gbsummary(GbEditMon* e, bool can_edit, bool start_editing, const char* note,
                    bool has_sidecar, bool* saved, int* card);

#endif /* PDNA_GBSUMMARY_H */

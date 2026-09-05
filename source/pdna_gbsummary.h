#ifndef PDNA_GBSUMMARY_H
#define PDNA_GBSUMMARY_H

#include <stdint.h>
#include <stdbool.h>
#include "gb_edit.h"

/* pdna_gbsummary — the Gen-1/2 twin of pdna_summary.c's pdna_inspect(), for
 * BACKLOG #41 ("the edit page for gen 2 and 1 should feel the same as gen 3 ... we
 * edit within the summary page"). Three cards styled after the retail Gen-1/2
 * summary screens (pokered/engine/pokemon/status_screen.asm,
 * pokecrystal/engine/pokemon/stats_screen.asm — clean-room: text placement and
 * field order only, no ripped art), over the NATIVE GbEditMon `e` — never the
 * lossy Gen-3-converted copy the box grid shows (docs/GEN12-EDIT-DESIGN.md 3.3).
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
 * `card` carries the current card (0..2) in and out, exactly like pdna_inspect's
 * own `card` — scrolling to the next mon stays on the same card. May be NULL
 * (treated as card 0, not written back). `note` and `has_sidecar` are the same
 * caller-supplied strings/flag pdna_gbedit() takes. */
int pdna_gbsummary(GbEditMon* e, bool can_edit, bool start_editing, const char* note,
                    bool has_sidecar, bool* saved, int* card);

#endif /* PDNA_GBSUMMARY_H */

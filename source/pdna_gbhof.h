#ifndef PDNA_GBHOF_H
#define PDNA_GBHOF_H

#include <stdbool.h>

#include "gb_session.h"   /* GbSession                                            */

/* Gen 1/2's own Hall of Fame screen (BACKLOG #89), over source/gb_hof.h's honest
 * core. THERE IS NO GEN-3 TWIN TO MIRROR: PokeDNA never reads the Gen-3 HoF sectors
 * at all (source/gb_hof.h's own header explains why) -- the "Records" nav row
 * (NV_BATTLEREC, pdna_layout.h) hosts THIS new screen on a Game Boy save instead of
 * the Emerald Frontier battle record it opens on Gen 3 (gb_nav_from_start,
 * pdna_gen12.c, is where the row's actual dispatch differs per generation).
 *
 * List -> detail, like the GB box/summary pair: a scrollable list of recorded teams
 * (newest first on both gens, gb_hof.h's own ordering), A opens a 6-mon detail page,
 * L/R fast-jumps the list a page at a time (the same convention
 * docs/kb/file-browser-conventions.md's LEFT/RIGHT fast-jump uses elsewhere in this
 * tree, and the paging idiom source/pdna_main.c's pdna_battle_record() uses for its
 * own "Records" screen on Gen 3 -- reused here in spirit, not in code, since there is
 * nothing Gen-3-shaped to literally share). START opens CLEAR ALL / SET COUNT, both
 * gated on `can_edit`; edits commit through gb_persist() (gb_rollback() on any
 * refusal, same convention as pdna_gbclock.c/pdna_gbfly.c/pdna_gbdaycare.c).
 *
 * `s` must already be open (gbs_open) and BE the live resident session g_ed->s wraps
 * (gb_persist() commits g_ed->img, not an arbitrary GbSession) -- same gate every
 * sibling GB screen's header documents. `can_edit`: true for the editable session
 * (Omega); false shows the list/detail read-only with the "needs an EZ-Flash Omega"
 * line and refuses START, same posture as pdna_gbclock.c's own read-only branch.
 *
 * Per-mon editing of a recorded team (renaming, stat edits, ...) is a later slice
 * (filed, not this one -- gb_hof.h's own core has no write path for individual mon
 * fields inside a stored team on purpose). */
void pdna_gbhof(GbSession* s, bool can_edit);

#endif /* PDNA_GBHOF_H */

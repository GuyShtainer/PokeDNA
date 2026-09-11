#ifndef PDNA_GBFLAGS_H
#define PDNA_GBFLAGS_H

#include <stdbool.h>
#include "gb_session.h"

/* Gen-1/2 "Flags & counters" screen -- BACKLOG #88. Mirrors pdna_main.c's own
 * data_editor_tab() shell (two tabs, COUNTERS | FLAGS, L/R swaps; 9-px rows, 14
 * visible; a foldable named-flags list with a trailing raw-flag-browser row) over a
 * live GbSession instead of SaveBlock1 -- see source/pdna_gbflags.c's header comment
 * for the full design. `can_edit` gates every write the same way pdna_gbclock's own
 * signature does (the caller, gb_nav_from_start, decides Omega-vs-read-only once). */
void pdna_gbflags(GbSession* s, bool can_edit);

#endif /* PDNA_GBFLAGS_H */

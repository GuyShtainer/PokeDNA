#ifndef PDNA_PROGRESS_H
#define PDNA_PROGRESS_H

#include "gen3_mon.h"

/* Full-screen batch-progress panel (Mode-3 software; the caller must have OBJ
 * suspended). Shows the current mon's 64x64 front sprite (Egg / icon fallback),
 * a `title`, a `done/total` counter with a filled bar, and an optional `note`
 * line. Draw one frame per step inside a batch loop; the sprite that is on screen
 * when an SD write starts stays visible for the whole (screen-frozen) write, so the
 * user sees each Pokemon "as it is exported". Does NOT poll input or wait. */
void pdna_progress_frame(const char* title, const PkMon* m, int done, int total, const char* note);

#endif /* PDNA_PROGRESS_H */

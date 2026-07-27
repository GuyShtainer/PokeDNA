#ifndef PDNA_BAG_H
#define PDNA_BAG_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_trainer.h"   /* PkGame */

/* Real Gen-3 bag screen — the data editor's bag tab wearing the loaded game's
 * in-game bag chrome (RS / Emerald / FRLG, per-gender; see bag_bg.h for the
 * art + per-game layout). Requires the generated art: gate the call on
 * bag_bg(game, female) != NULL — art-free builds keep the plain tab. `female`
 * is sb2[SB2_OFF_GENDER] (re-read on entry — the trainer card can flip it
 * mid-session).
 *
 * L/R switch pocket (game behavior), U/D scroll, A edit (the plain tab's
 * pick_item -> quantity -> pk_bag_set powers, wrong-pocket routing included),
 * B exit. Edits SaveBlock1 in RAM only; returns true iff anything changed —
 * the CALLER owns the confirm + verified commit (data_editor's
 * app_commit_block(1,4,...)), so this adds no SD write paths. */
bool bag_screen(uint8_t* sb1, const uint8_t* sb2, PkGame game, int female);

#endif /* PDNA_BAG_H */

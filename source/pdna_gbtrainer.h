#ifndef PDNA_GBTRAINER_H
#define PDNA_GBTRAINER_H

#include <stdbool.h>

#include "gb_session.h"   /* GbSession                                            */

/* Gen-1/2 trainer card (BACKLOG #49 P1b, docs/GEN12-PARITY-DESIGN.md sections 1.1,
 * 4.1 P1, 4.3, 4.4). UX-PARITY RULE (Guy): looks and flows like the Gen-3 card
 * (source/pdna_trainer.c) -- same row layout, red selection panel, num_entry, badge
 * toggle screen and key legend, all reused via pdna_trainer.h's exported painters.
 * Rows that do not exist for this game are OMITTED, never renamed or greyed-out.
 *
 * NO CARD ART: Gen 1/2 have no card front table in the ROM by shape the way Emerald's
 * card_bg.h does (docs/GEN12-PARITY-DESIGN.md 4.4 note); this is always the plain
 * row-list page, never card_bg()/rom_card_frame(). If that ever changes, this screen
 * is where a card_bg(GBF_G_*, ...) branch would go, mirroring pdna_trainer()'s own
 * card_bg(game,...).blob != 0 fork.
 *
 * Edits are staged in a LOCAL GbTrainer (gbt_read at entry, mutated in place by the
 * row editors) and committed ONLY on START, in one gbt_write() batch -- gbt_write's
 * own unchanged-field discipline (gb_trainer.h) means a field the user never touched
 * is never rewritten. B always discards (the session's image was never touched
 * unless START already ran and returned).
 *
 * `s` must already be open (gbs_open). `can_edit`: true for the editable session
 * (gb_nav_from_start passes &g_ed->s, true), false for a view-only visit -- U/D
 * still moves the cursor so every field can be read, but A/START do nothing. */
void pdna_gbtrainer(GbSession* s, bool can_edit);

#endif /* PDNA_GBTRAINER_H */

#ifndef PDNA_GBTRAINER_H
#define PDNA_GBTRAINER_H

#include <stdbool.h>

#include "gb_session.h"   /* GbSession                                            */

/* Gen-1/2 trainer card (BACKLOG #49 P1b + P1c, docs/GEN12-PARITY-DESIGN.md sections
 * 1.1, 4.1 P1, 4.3, 4.4). UX-PARITY RULE (Guy): looks and flows like the Gen-3 card
 * (source/pdna_trainer.c). P1c: when Emerald's own card art is available (the
 * generated build, or an artless build with a matching ROM open -- card_bg.h's
 * usual fork), this IS the real Emerald card front/back (card_bg()/CARD_LAYOUTS),
 * with GB's own field values painted at Emerald's field positions via
 * pdna_trainer.h's shared card painters (card_front_fields_paint / card_field_one /
 * card_back_row_paint / ...) -- see pdna_gbtrainer.c's own top comment for the row
 * lists. When no card art is available at all, this degrades to P1b's plain
 * row-list page (same red selection panel, num_entry, badge toggle screen and key
 * legend as before, reused via pdna_trainer.h's exported plain-page painters).
 * Rows that do not exist for this game are OMITTED, never renamed or greyed-out.
 *
 * Gen 1/2 have no card front table of their OWN in the ROM by shape the way
 * Emerald's card_bg.h does (docs/GEN12-PARITY-DESIGN.md 4.4 note) -- this screen
 * always paints on EMERALD's own art/layout (pdna_gbtrainer.c explains why
 * specifically Emerald), never a per-GB-game frame.
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

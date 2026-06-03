#ifndef PKVIEW_EDIT_H
#define PKVIEW_EDIT_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_mon.h"
#include "gen3_edit.h"

/* Editable field ids (shared with the inline-editable summary). */
enum {
  F_SPECIES, F_NICK, F_LEVEL, F_NATURE, F_ABILITY, F_SHINY, F_GENDER,
  F_ITEM, F_FRIEND,
  F_IV0, F_IV1, F_IV2, F_IV3, F_IV4, F_IV5,
  F_EV0, F_EV1, F_EV2, F_EV3, F_EV4, F_EV5,
  F_MV0, F_MV1, F_MV2, F_MV3,
  F_OT,
  F_NUM
};

/* Edit one field: A-press (picker / keyboard / toggle) and dpad LEFT/RIGHT adjust
 * (dir -1/+1, big = L/R shoulders). Shared by the field list and the inline summary. */
void em_field_press(int field, EditMon* e, const PkMon* cur);
void em_field_adjust(int field, int dir, bool big, EditMon* e, const PkMon* cur);

/* Field-edit screen for ONE Pokémon (PKHeX-style). is_party => 100-byte record,
 * else 80-byte box record. Edits an in-RAM working copy with a live preview.
 * On START + confirm it writes the edited record (100/80 bytes) to out_rec and
 * returns true; B cancels and returns false. It does NOT touch the SD — the
 * caller persists out_rec via the gated verified-write path. */
bool pkview_edit(const uint8_t* rec, bool is_party, uint8_t* out_rec);

#endif /* PKVIEW_EDIT_H */

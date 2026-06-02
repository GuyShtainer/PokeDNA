#ifndef PKVIEW_EDIT_H
#define PKVIEW_EDIT_H

#include <stdint.h>
#include <stdbool.h>

/* Field-edit screen for ONE Pokémon (PKHeX-style). is_party => 100-byte record,
 * else 80-byte box record. Edits an in-RAM working copy with a live preview.
 * On START + confirm it writes the edited record (100/80 bytes) to out_rec and
 * returns true; B cancels and returns false. It does NOT touch the SD — the
 * caller persists out_rec via the gated verified-write path. */
bool pkview_edit(const uint8_t* rec, bool is_party, uint8_t* out_rec);

#endif /* PKVIEW_EDIT_H */

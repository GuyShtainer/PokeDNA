#ifndef PKVIEW_SUMMARY_H
#define PKVIEW_SUMMARY_H

#include <stdint.h>
#include <stdbool.h>

/* Inline VIEW + EDIT of one Pokémon record (the 6 summary cards). Starts in VIEW:
 * A enters edit mode (can_edit only), U/D request the prev/next mon, L/R flip card,
 * B leaves. The "save changes?" prompt appears only when leaving or changing mon
 * with unsaved edits — on save it writes the edited 100/80-byte record to out_rec
 * and sets *saved. Returns 0 (exit), +1 (next mon) or -1 (prev mon); the caller
 * loads that mon and calls again. `saved` may be NULL. */
int pkview_inspect(uint8_t* rec, bool is_party, bool can_edit, uint8_t* out_rec, bool* saved);

#endif /* PKVIEW_SUMMARY_H */

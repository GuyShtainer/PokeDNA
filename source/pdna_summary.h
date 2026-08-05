#ifndef PDNA_SUMMARY_H
#define PDNA_SUMMARY_H

#include <stdint.h>
#include <stdbool.h>

/* Inline VIEW + EDIT of one Pokémon record (the 6 summary cards). Starts in VIEW:
 * A enters edit mode (can_edit only), U/D request the prev/next mon, L/R flip card,
 * B leaves. The "save changes?" prompt appears only when leaving or changing mon
 * with unsaved edits — on save it writes the edited 100/80-byte record to out_rec
 * and sets *saved. Returns 0 (exit), +1 (next mon) or -1 (prev mon); the caller
 * loads that mon and calls again. `saved` may be NULL.
 *
 * `card` carries the current summary card (0..5) in and out, so scrolling U/D to
 * the next mon stays on the same card (real-PC behaviour) instead of resetting to
 * card 0. Pass a caller-owned int that persists across the scroll loop; may be NULL
 * (treated as card 0, not written back). */
int pdna_inspect(uint8_t* rec, bool is_party, bool can_edit, uint8_t* out_rec,
                   bool* saved, int* card);

/* CREATE mode: the same six cards over a brand-new record that is NOT in a save slot
 * yet (one that gen3_build_mon just made), so making a Pokémon looks like inspecting
 * one instead of dropping into a flat field list. Differences from pdna_inspect:
 *   - U/D never scroll to a prev/next mon — there isn't one.
 *   - START, or B, raises "Keep this Pokémon?"; A keeps, B discards.
 *   - the keep path fills out_rec and sets *saved EVEN IF nothing was edited, because
 *     the record itself is the new thing (gen3_edit_commit is lossless, so a no-edit
 *     keep reproduces gen3_build_mon's bytes exactly).
 * A green NEW chip replaces the VIEW/EDIT banner so it is obvious the mon is not
 * saved yet. Always returns 0. Editing is always allowed (the caller is Omega-gated). */
int pdna_inspect_create(uint8_t* rec, uint8_t* out_rec, bool* saved, int* card);

#endif /* PDNA_SUMMARY_H */

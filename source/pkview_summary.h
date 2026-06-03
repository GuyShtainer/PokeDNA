#ifndef PKVIEW_SUMMARY_H
#define PKVIEW_SUMMARY_H

#include <stdint.h>
#include <stdbool.h>

/* Inline view + edit of one Pokémon record (the 6 summary cards). If can_edit,
 * fields are selectable (U/D), edited in place (A / LEFT-RIGHT), and on B a
 * "save changes?" prompt commits — writing the edited 100/80-byte record to
 * out_rec and returning true. Read-only (can_edit=false) just pages the cards
 * and always returns false. */
bool pkview_inspect(uint8_t* rec, bool is_party, bool can_edit, uint8_t* out_rec);

#endif /* PKVIEW_SUMMARY_H */

#ifndef PKVIEW_PICK_H
#define PKVIEW_PICK_H

#include <stdint.h>

/* Rich pickers for the editor. Each returns the chosen id, or 0xFFFF if the user
 * cancelled (B). `current` pre-selects the starting entry.
 *
 * pick_species: HGSS-style icon grid with live search (SELECT -> keyboard),
 *   filter (L: All / Gen1-3 / Legendary / by-Type) and sort (R: No. / A-Z).
 * pick_move:    list with the move's type chip, power/accuracy/PP, description,
 *   a type filter (L) and search (SELECT).
 * pick_item / pick_nature: searchable lists. */
uint16_t pick_species(uint16_t current_internal);
uint16_t pick_move(uint16_t current_move);
uint16_t pick_item(uint16_t current_item);
uint8_t  pick_nature(uint8_t current_nature);

/* Ability picker. Gen-3 stores only a 1-bit ability SLOT, so the choices are the
 * species' two abilities (shown by name + description). Returns the chosen slot
 * (0 or 1), or `current` on cancel. */
uint8_t  pick_ability(uint16_t species_internal, uint8_t current_slot);

#endif /* PKVIEW_PICK_H */

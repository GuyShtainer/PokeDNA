#ifndef PDNA_PICK_H
#define PDNA_PICK_H

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
int      pick_unown_form(int current_form);     /* 0..27 = A..?, -1 cancel */

/* Poke Ball picker. Gen 3 keeps the ball in four bits, and the twelve balls are item ids
 * 1..12, so this shows the item name + icon + blurb for each. Returns the chosen ball
 * (1..12), or `current` on cancel — the field can never hold anything else. */
uint8_t pick_ball(uint8_t current);

/* Ability picker. Gen-3 stores only a 1-bit ability SLOT, so the choices are the
 * species' two abilities (shown by name + description). Returns the chosen slot
 * (0 or 1), or `current` on cancel. */
uint8_t  pick_ability(uint16_t species_internal, uint8_t current_slot);

/* ---- Pokedex viewer / editor ---------------------------------------------
 * HGSS-style Pokedex built on the same species grid + filters. The dex flags
 * live in the loaded save (pdna_main owns the buffers), so the screen reaches
 * them through these callbacks. State: 0 = unseen, 1 = seen, 2 = caught. */
typedef int  (*DexGetState)(int nat);          /* National no. (1..386) -> 0/1/2 */
typedef void (*DexSetState)(int nat, int state);
/* National Dex unlock hooks: reading/writing the magic+var+flag trio so the in-game
 * dex actually shows #152..386. getnat returns whether national is currently live;
 * setnat turns it on/off. The DEX:ALL menu shows a toggle and auto-enables on Catch ALL.
 * Both may be NULL (then the toggle is hidden). */
typedef bool (*DexGetNat)(void);
typedef void (*DexSetNat)(bool on);

/* Run the Pokedex screen. Three views (Grid / List / by-Type), the species-grid
 * filters (Gen/type/legendary) + a caught/seen/unseen status filter + name search,
 * and per-state sprite rendering (greyscale unseen, colour seen, colour+bob caught
 * with a Poke-Ball marker). When `can_edit`, A cycles a species unseen->seen->caught
 * and a "Mark all" bulk op is offered. Returns true iff any dex state changed (so
 * the caller can offer to save). */
bool pdna_dex_screen(DexGetState get, DexSetState set,
                     DexGetNat getnat, DexSetNat setnat, bool can_edit);

#endif /* PDNA_PICK_H */

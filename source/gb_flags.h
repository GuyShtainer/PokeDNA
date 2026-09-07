#ifndef GB_FLAGS_H
#define GB_FLAGS_H

#include <stdint.h>
#include <stdbool.h>

#include "gb_fields.h"   /* GbGame */

/* Per-game named event-flag shortlist (BACKLOG #49 P0, docs/GEN12-PARITY-DESIGN.md §1.3).
 *
 * "The flag NUMBERING is per-game, not per-generation" — EVENT_GOT_RAINBOW_WING is bit
 * #120 in Gold and bit #822 in Crystal, and five Red/Blue-vs-Yellow names sit at
 * different indices too. A shared "Gen 2" table would silently toggle the wrong bit on
 * one of the two games, so this is four independent tables, generated (never hand-typed)
 * from each game's own event-constants file by tools/gen_gbfields.py — regenerate with
 * `python3 tools/gen_gbfields.py`. source/gb_flags_fallback.c is the COMMITTED weak
 * fallback for a clone that never ran the generator (every game reports 0 flags).
 *
 * Bit addressing (identical to Gen 3's): byte = GBF_EVENT_FLAGS_BASE(_G2) + n/8,
 * bit = n & 7 (docs/GEN12-PARITY-DESIGN.md §1.3).
 *
 * Licensing (docs/kb/licensing.md): the per-game bit INDEX is a fact, read out of the
 * pinned, reference-only decomp's own auto-incrementing const table. The LABEL text is
 * this app's own writing, never the decomp's identifier or a comment translated in
 * place — see tools/gen_gbfields.py's LABELS dict. */

typedef struct {
  uint16_t index;       /* the flag's bit number in GBF_EVENT_FLAGS_BASE(_G2)'s bitfield */
  const char* label;    /* our own text, e.g. "Defeated Brock" -- never a decomp string  */
} GbFlagEntry;

/* How many named flags this game's shortlist has (0..~40). */
int gbfl_count(GbGame g);
/* Our own label for bit `flag` on game `g`, or NULL if `flag` is not on the shortlist
 * (most of the 2560/2048 bits are not — this is the ~40-entry curated set, not every
 * named decomp constant). */
const char* gbfl_name(GbGame g, uint16_t flag);
/* Row `i` (0..gbfl_count(g)-1) of the shortlist, sorted by bit index. False if `i` is
 * out of range. Lets a UI page the list without knowing the bit numbers up front. */
bool gbfl_at(GbGame g, int i, uint16_t* flag_out, const char** label_out);

#endif /* GB_FLAGS_H */

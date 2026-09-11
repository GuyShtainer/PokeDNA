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

/* ---- BACKLOG #88: the FLAGS tab's own foldable-section row list ------------------
 *
 * gbfl_count/gbfl_name/gbfl_at above are UNCHANGED (flags only, sorted by bit index --
 * the old contract host_gbfields_test.c already exercises). The UI needs section
 * HEADER rows interleaved in a curated SCREEN order (not sorted by bit) plus a
 * per-row edit posture (plain toggle / bag-item-grant read-only / story read-only /
 * extra-warning toggle) the research doc (docs/GB-FLAGS-RESEARCH.md) assigned per
 * flag -- gbfl_row_at() is the one new accessor that carries all of that, generated
 * into a SEPARATE table (k_rows_<game>) alongside the existing k_flags_<game>. */
#define GBFL_HEADER 0xFFFFu   /* mirrors NAMED_FLAG_HEADER (gen3_flags.h) -- same value,
                                * so a GbFlagRow can be reinterpreted as flags_fold.h's
                                * NamedFlag (identical {uint16_t, const char*} prefix). */
typedef enum {
  GBFL_KIND_HEADER = 0,   /* a section header row; `kind` is otherwise meaningless    */
  GBFL_KIND_TOGGLE,       /* plain A-to-toggle, gated by the screen's one-time CAUTION */
  GBFL_KIND_BAG_GRANT,    /* read-only: "(grant it in the Bag)" -- HM/Bicycle/GS Ball  */
  GBFL_KIND_READONLY,     /* read-only: story/Champion display, no toggle offered      */
  GBFL_KIND_WARN          /* toggle behind an EXTRA confirm naming the consequence     */
} GbFlagKind;

typedef struct {
  uint16_t index;    /* the flag's bit number, or GBFL_HEADER for a section header row */
  const char* label; /* our own text -- a flag's name, or the section header's title   */
  uint8_t kind;       /* GbFlagKind; GBFL_KIND_HEADER for a header row                  */
} GbFlagRow;

/* How many rows (headers + flags) this game's FLAGS-tab screen shows. */
int gbfl_row_count(GbGame g);
/* Row `i` (0..gbfl_row_count(g)-1), in curated SCREEN order (never sorted by bit).
 * False if `i` is out of range. */
bool gbfl_row_at(GbGame g, int i, GbFlagRow* out);

/* EVENT_IN_SAFARI_ZONE's own bit index on `g` (Gen 1 only), or -1 if this game has no
 * such flag (Gen 2). BACKLOG #88: gates whether the Safari Zone step counter
 * (GBF_SAFARI_STEPS) is editable -- this tool never SETS this flag itself (entering
 * the Safari Zone is a map-placement operation, out of scope for a counter edit). */
int gbfl_safari_zone_flag(GbGame g);

#endif /* GB_FLAGS_H */

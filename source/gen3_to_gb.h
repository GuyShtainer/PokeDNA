#ifndef GEN3_TO_GB_H
#define GEN3_TO_GB_H

#include <stdint.h>
#include <stdbool.h>
#include "gb_edit.h"     /* GbEditMon, GbGen1Base, GB_GLYPH_MAX, the gb_set_ and gb_get_ API */

/* Gen 3 -> Game Boy (Gen 1/2) DOWN converter — pure C, host-testable.
 *
 * docs/GEN3-TO-GB-SIDECAR-DESIGN.md section 5 is the design; this is the mechanism for
 * its point 1. The Game Boy record it builds is deliberately NOT the whole story: this
 * module also fills a `Gen3ToGbLoss` report of everything the record cannot carry, so
 * the sidecar (gb_sidecar.h) can be written before the loss becomes permanent, and the
 * merge back up (gbsc_merge_up) can restore it later.
 *
 * WHAT THIS DOES NOT DO. It never opens a file, never touches the SD card, and never
 * mutates a GB save's box/party list — it only fills a caller-owned GbEditMon (an
 * unattached record) that some later gbs_/g2w_/gen1_write_ call has to insert. It also
 * never DEGRADES silently outside the documented table: species/move/egg/base-table
 * problems are outright REFUSALS (G3GbStatus other than G3GB_OK), and `out` is left
 * completely untouched on a refusal — the same "refuse rather than guess" rule every
 * other edit core in this tree follows.
 *
 * SPECIES MAPPING. For national dex 1..251 the Gen-3 INTERNAL species index and the
 * National Dex number are the SAME NUMBER (gen12_convert.c:304-307, independently
 * confirmed by pdna_origin_art.c:407-410 for the up path) — Hoenn's own species start
 * at internal index 277. Since a Game Boy species is always in 1..251
 * (gb_max_species() tops out at 251 for Gen 2), gen3_mon.h's PkMon.species (the
 * INTERNAL index) can be handed straight to pk_national_no() for the range check and
 * gb_set_species() takes the National Dex number directly — no lookup table is needed
 * on this side, and the merge-up direction (gb_sidecar.c) reuses the same identity to
 * go the other way. */

/* Everything a Gen-3 record carries that a Game Boy record has no room for. All false
 * is the ideal case (never actually reachable — every Gen-3 mon has SOME PID-derived
 * nature); the UI shows a line per true field. Each *_lossy pair (nick/ot) also fills
 * the first glyph gb_text_lossy() could not represent, for a caller that wants to say
 * which character was the problem. */
typedef struct {
  bool nature, ability, shiny_lost, gender_lost, ribbons, contest, met_data, ball,
       item_dropped, secret_id, markings, evs_scaled, ivs_halved, nick_lossy, ot_lossy,
       pokerus_dropped, friendship_dropped;
  char nick_first_bad[GB_GLYPH_MAX];
  char ot_first_bad[GB_GLYPH_MAX];
} Gen3ToGbLoss;

typedef enum {
  G3GB_OK = 0,
  G3GB_ERR_ARG,          /* NULL pointer or an unrecognised `gen`                      */
  G3GB_ERR_EGG,          /* isEgg or isBadEgg — neither has a Game Boy shape           */
  G3GB_ERR_SPECIES,      /* national dex 0, or > gb_max_species(gen)                   */
  G3GB_ERR_MOVE,         /* a non-empty move slot this generation's games do not have  */
  G3GB_ERR_NEEDS_BASE,   /* gen == GB_GEN1 and g1base == NULL (types/catch rate)       */
  G3GB_ERR_GLITCH,       /* pk_decode_mon could not make sense of rec80                */
} G3GbStatus;

const char* g3gb_status_text(G3GbStatus st);

/* Convert one Gen-3 record (80-byte box core; a party record's own box-shaped core
 * works too, since gen3_mon.h's PkMon reads party fields as an overlay of the same
 * bytes) into a brand-new Game Boy BOX record. `out` is completely rebuilt from zero —
 * this is not a patch of an existing GbEditMon — and is left untouched unless the
 * return value is G3GB_OK. `g1base` is required (and used) only for gen == GB_GEN1;
 * pass NULL for GB_GEN2. `loss` may be NULL if the caller does not want the report.
 *
 * `caught_available` (BACKLOG #95 review C11, gbmon lane): the 0x1D/0x1E capture
 * record this module can synthesize is CRYSTAL-only real data -- on a Gold/Silver
 * target those two bytes are Unused1/Unused2, so writing them is "harmless dead data"
 * for a target save that already holds arbitrary garbage there, but is WRONG to claim
 * for a freshly synthesized target the caller is about to commit as a real record.
 * Pass true only when the target save is confirmed Crystal (gb_session_is_crystal());
 * false leaves the two bytes untouched (0x00) and the write is silently skipped, not
 * an error -- gen == GB_GEN1 ignores this argument entirely (no capture record at all). */
G3GbStatus gen3_to_gb(const uint8_t* rec80, uint8_t gen, bool caught_available,
                     const GbGen1Base* g1base, GbEditMon* out, Gen3ToGbLoss* loss);

/* ---- BACKLOG #104 R1: the MAKE LEGAL correction (evolution-minimum level) ------
 * docs/TRANSFER-ROUNDTRIP-DESIGN.md section 3c: MAKE LEGAL's ONLY correction beyond
 * what KEEP AS IS already keeps is raising an under-levelled EVOLVED species to its
 * the lowest level the species can legally stand at (evolutions.h's own forward table — already shipped,
 * unrelated to this feature). Nothing else is corrected here.
 *
 * `out` must be a just-converted, ACCEPTED record (gen3_to_gb() returned G3GB_OK).
 * Species is read via gb_get_species_dex(out), always in 1..251 for anything
 * gen3_to_gb() accepted (see the species-mapping note above this file's header
 * comment), so it is handed to pk_evo_floor() directly (the checker's floor, not the builder's
 * minimum) — no lookup table needed.
 *
 * Read-only: never mutates `out`. Returns true and fills from_level/to_level with
 * the correction MAKE LEGAL would apply when the mon is standing below its species'
 * evolution floor; false (levels untouched) when it is already at or above that
 * floor, or when evolutions.h has no table linked in (pk_evo_have_data() false) —
 * "no data" must never manufacture a correction. Applying the fix, once the user
 * has chosen MAKE LEGAL, is a plain `gb_set_level(out, to_level)` call by the
 * caller (gb_edit.h, already shipped) — this function does not do it, so the
 * dialog can show the "from -> to" numbers before anything is decided. */
bool gen3_to_gb_evo_needs_fix(const GbEditMon* out, uint8_t* from_level, uint8_t* to_level);

#endif /* GEN3_TO_GB_H */

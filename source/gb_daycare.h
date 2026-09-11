#ifndef GB_DAYCARE_H
#define GB_DAYCARE_H

#include <stdint.h>
#include <stdbool.h>

#include "gb_fields.h"    /* GbGame, GbField, gbf_off/len/kind                          */
#include "gb_session.h"   /* GbSession, GbsStatus, gbs_read_field/gbs_write_field/finish */
#include "gb_edit.h"      /* GbEditMon, gb_load_parts/gb_commit_parts, GB_GEN1/GB_GEN2   */

/* gb_daycare — pure-C day-care core over the generated field table (BACKLOG #85,
 * docs/GEN12-PARITY-DESIGN.md §1.7). Mirrors gen3_daycare.h's shape (view/withdraw/
 * deposit) as far as the format allows; Gen 1's day-care is a LEVEL-UP SERVICE ONLY
 * (no breeding, one boarder, no compatibility line — same degrade the design doc's own
 * §1.7 note describes for the screen this backs).
 *
 * SLOT NUMBERING (this module's own, not a decomp constant): 0 = the Day-Care Man's
 * mon (Gen 1's only slot; Gen 2's first of two). 1 = the Day-Care Lady's mon, Gen 2
 * only. The egg is its own set of functions (gbd_egg_*), not a third numbered slot —
 * it has no otname/nickname-editable-by-deposit shape, and "depositing" one is
 * meaningless (an egg is PRODUCED by the game's own breeding step, never handed in).
 *
 * OCCUPANCY BITS, verified against BOTH pokegold's and pokecrystal's
 * constants/ram_constants.asm (identical bit layout in both) and cross-checked against
 * Guy's own Gold.sav (probe: GBF_DAYCARE_REC there decodes to a real CROCONAW/lvl 21/
 * OT "MattiaPK" even though wDayCareMan reads 0x80 -- bit 0, DAYCAREMAN_HAS_MON_F, is
 * CLEAR, so this is a WITHDRAWN slot whose record bytes are leftover residue, not an
 * occupied one; the retail withdraw code, engine/events/daycare.asm's .AskWithdrawMon,
 * clears bit 0 and bit 5 and never touches the record -- exactly what this fixture
 * shows):
 *   wDayCareMan  bit 0 (DAYCAREMAN_HAS_MON_F)         slot 0 occupied
 *                bit 5 (DAYCAREMAN_MONS_COMPATIBLE_F) the breeding-compatibility hint
 *                bit 6 (DAYCAREMAN_HAS_EGG_F)          an egg is ready to collect
 *                bit 7 (DAYCAREMAN_ACTIVE_F / Gold's DAYCARE_INTRO_SEEN_F -- same bit,
 *                       different decomp name, same "intro dialogue already seen"
 *                       meaning per Gold's own comment "shared flag between wDayCareMan
 *                       and wDayCareLady") -- VIEW-ONLY here, not occupancy.
 *   wDayCareLady bit 0 (DAYCARELADY_HAS_MON_F)  slot 1 occupied
 *                bit 7  same shared intro-seen bit, VIEW-ONLY.
 * Gen 1's wDayCareInUse is a plain 0/1 byte (GBFK_U8, not a bitfield) -- no bit games.
 */

typedef struct {
  bool     occupied;
  GbEditMon mon;            /* valid only when `occupied` (residue may otherwise decode
                             * to garbage/a stale record -- see the header note above) */
  uint8_t  nick_raw[GB_NAME_BYTES];
  char     nick[GB_TEXT_MAX];
  uint8_t  ot_raw[GB_NAME_BYTES];
  char     ot[GB_TEXT_MAX];
} GbDaycareSlot;

typedef struct {
  GbGame        game;
  bool          gen1;              /* true: only slot[0] and steps_to_egg are absent  */

  GbDaycareSlot slot[2];           /* slot[1] is zeroed/unused on Gen 1               */

  bool          has_slot2;         /* Gen 2 only                                      */
  bool          compatible;        /* wDayCareMan bit 5, Gen 2 only                   */
  bool          egg_ready;         /* wDayCareMan bit 6, Gen 2 only                   */
  bool          intro_seen;        /* wDayCareMan bit 7, VIEW-ONLY, Gen 2 only        */
  uint8_t       steps_to_egg;      /* GBF_DAYCARE_STEPS raw byte, Gen 2 only          */

  bool          has_egg;           /* == egg_ready, kept as its own bool for symmetry
                                    * with GbDaycareSlot's `occupied` -- gbd_egg_read()
                                    * fills egg below iff this is true                */
  GbEditMon     egg;
  uint8_t       egg_nick_raw[GB_NAME_BYTES];
  char          egg_nick[GB_TEXT_MAX];
  /* P1a review D3: the egg's own OT (GBF_DAYCARE_EGG_OT, wEggMonOT), distinct from the
   * nickname -- an earlier revision reused the nickname bytes for both, so a withdrawn
   * egg's "OT" was always its nickname. `egg.list_species` is loaded as G2_LIST_EGG
   * (see gb_edit.h) so the egg also reads as an egg (gb_is_egg()), not as whatever
   * raw species its record byte happens to hold. */
  uint8_t       egg_ot_raw[GB_NAME_BYTES];
  char          egg_ot[GB_TEXT_MAX];
} GbDaycare;

GbGame gbd_game(const GbSession* s);

/* Fill `out` from the session's resident image. False only on a malformed session. A
 * Gen-1 session fills slot[1]/egg/compatible/egg_ready/intro_seen/steps_to_egg at their
 * zero defaults and out->gen1 = true -- this is a successful "no second slot, no
 * breeding" read, not a failure, same shape gbt_read/gbc_read use for has_mom/present. */
bool gbd_read(const GbSession* s, GbDaycare* out);

/* Deposit `mon` into `slot` (0 or, Gen 2 only, 1).
 *   GBS_ERR_ARG   `slot` out of range for this generation (1 on Gen 1), `mon` is NULL,
 *                 `mon->gen` disagrees with the session's generation, or `mon->is_party`
 *                 is true (a party-shaped record handed to a box-shaped destination --
 *                 same refusal gbs_insert() already makes for exactly this reason; the
 *                 caller converts party -> box itself first, e.g. via gbs_move()'s own
 *                 machinery, before calling this).
 *   GBS_ERR_FULL  the slot already reads occupied (gbd_read().slot[slot].occupied) --
 *                 deposit one at a time, like the retail counter does.
 * Writes the record, OT name and nickname, then sets ONLY the occupancy bit (bit 0 for
 * Gen 2; the whole byte for Gen 1) -- never the compatibility/egg/intro-seen bits,
 * which this slice does not compute (the design doc's own §1.7 leaves "the game's
 * computed flag" as a retail-only computation; a caller wanting a fresh compatibility
 * read still has to let the game itself recompute it, same as retail does on the next
 * counter visit). Calls gbs_finish() once at the end. */
GbsStatus gbd_deposit(GbSession* s, int slot, const GbEditMon* mon);

/* Withdraw `slot`, filling `out` with the departing mon's record (SAME shape gbt_read's
 * scope note uses: "the core never places" -- gbd_withdraw() never touches a box or
 * party, only the caller decides where `out` lands, e.g. via gbs_insert()/gbs_move()).
 *   GBS_ERR_SLOT  `slot` is not occupied, or out of range for this generation.
 * Clears the occupancy bit and the compatibility bit (on EITHER slot: both games' withdrawal routines reset DAYCAREMAN_MONS_COMPATIBLE_F for the Lady too) -- exactly what
 * engine/events/daycare.asm's .AskWithdrawMon does (`res DAYCAREMAN_HAS_MON_F` +
 * `res DAYCAREMAN_MONS_COMPATIBLE_F`). The record bytes themselves are left exactly as
 * found (retail leaves residue too, per the header's own Gold.sav witness) -- a caller
 * that wants them zeroed does that itself. */
GbsStatus gbd_withdraw(GbSession* s, int slot, GbEditMon* out);

/* The egg (Gen 2, Crystal or GS -- both define the fields, docs/GEN12-PARITY-DESIGN.md
 * §1.7's own table). GBS_ERR_ARG on Gen 1 or a malformed session, GBS_ERR_SLOT if
 * !egg_ready. Clears wDayCareMan bit 6 only; the egg record is left as found (same
 * residue policy as gbd_withdraw). There is no gbd_egg_deposit(): an egg is produced by
 * the game's own breeding step, never handed in by a caller. */
GbsStatus gbd_withdraw_egg(GbSession* s, GbEditMon* out);

#endif /* GB_DAYCARE_H */

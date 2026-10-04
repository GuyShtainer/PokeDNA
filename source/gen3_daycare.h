#ifndef GEN3_DAYCARE_H
#define GEN3_DAYCARE_H

#include <stdint.h>
#include <stdbool.h>
#include "dc_learn.h"
#include "gen3_edit.h"   /* EditMon */
#include "gen3_trainer.h" /* PkGame */

/* Day-Care breeding compatibility (pure C, host-testable). Replicates Gen-3
 * GetDaycareCompatibilityScore (pokeemerald daycare.c) — the verdict the in-game
 * Day-Care man bases his "they seem to get along" line on. Like the games, the
 * SCORE itself ignores gender; use pk_daycare_can_breed() for the practical check. */

/* Egg group of an internal species id (which = 0 or 1). 0..15; 15 = UNDISCOVERED. */
uint8_t pk_egg_group(uint16_t internal, int which);

typedef enum {
  DC_INCOMPATIBLE = 0,   /* "they'd rather play with others"   */
  DC_LOW,                /* "don't seem to like each other"    */
  DC_MED,                /* "seem to get along"                */
  DC_HIGH,               /* "get along very well!"             */
} DcCompat;

/* The Day-Care man's verdict for two parents (full 32-bit OT ids). */
DcCompat    pk_daycare_compat(uint16_t spA, uint32_t otA, uint16_t spB, uint32_t otB);
const char* pk_daycare_compat_msg(DcCompat c);

/* Practical breedability: one male (0) + one female (1), OR exactly one Ditto
 * paired with a non-Ditto breedable mon. Genderless (2) only breeds as Ditto. */
bool pk_daycare_can_breed(uint16_t spA, uint8_t genA, uint16_t spB, uint8_t genB);

/* The line the yard screen prints under the Day-Care man's verdict, to say which
 * Pokemon in the yard are actually the player's.
 *
 * The Gen-3 Day-Care holds EXACTLY TWO Pokemon. Any other mon the yard shows is
 * scenery this viewer invents for the picture; it is never read from or written to
 * the save and can never be selected. `boarders` is 0..2, `visitors` is that
 * scenery count (>= 0). Pure C, so tests/host_daycare_test.c pins the wording. */
const char* pk_daycare_yard_note(int boarders, int visitors);

/* ---- Take-out growth (BACKLOG #373) -------------------------------------------------
 * Retail adds the boarding steps to a mon's EXP when you take it out (pokeemerald
 * TakeSelectedPokemonFromDaycare) and teaches the level-up moves of every level it
 * crossed (ApplyDaycareExperience: oldest move out when all four slots are full).
 * gen3_dc_preview computes that on a COPY and changes nothing; gen3_dc_apply writes it.
 * `learnset_game` = the game the save belongs to (a boarded mon learns from the game it
 * is in). */
typedef struct {
  uint16_t species;                   /* internal id, for the log line */
  uint8_t  lv_before, lv_after;
  uint32_t exp_before, exp_after;
  int      n_learn;                   /* TRUE count; only the first DC_LEARN_MAX are in learn[] */
  DcLearn  learn[DC_LEARN_MAX];
  bool     overflow;                  /* n_learn > DC_LEARN_MAX */
  uint16_t moves_after[4];
  uint8_t  pp_after[4], ppups_after[4];
} DcGrow;

/* Pure read. False only for a bad record (species 0, an egg: an egg never grows). */
bool gen3_dc_preview(PkGame learnset_game, const uint8_t* rec, bool is_party,
                     uint32_t steps, DcGrow* out);
/* Writes e: EXP always (and the party level + stats); the moves/PP/PP-Ups only when
 * with_moves. */
void gen3_dc_apply(EditMon* e, const DcGrow* g, bool with_moves);

#endif /* GEN3_DAYCARE_H */

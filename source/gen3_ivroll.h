#ifndef GEN3_IVROLL_H
#define GEN3_IVROLL_H

#include <stdint.h>
#include <stdbool.h>
#include "gen3_mon.h"
#include "gen3_edit.h"
#include "gen3_pidiv.h"
#include "pdna_layout.h"   /* IVH_CAP — see the note there */

/* ONE LEGITIMATE IV DRAW, AND WHAT IT COST.
 *
 * "Reroll the IVs" is not a free action in Gen 3. For a CAUGHT Pokemon the PID and both IV
 * words come off ONE LCRNG stream (gen3_pidiv.h), so new IVs mean a new PID — and the PID
 * IS the nature, the sex, the ability slot, the shininess and Unown's letter. A reroll that
 * ignored that would either hand back a different Pokemon without saying so, or produce a
 * spread PokeDNA's own checker calls suspect. This module does neither: it searches for a
 * seed that keeps every derived property, and reports anything it could not keep.
 *
 * For a PIDIV-EXEMPT record (egg, hatched, event, Colosseum/XD, non-GBA origin) the PID and
 * IVs are legitimately independent, so the PID is kept outright and NOTHING else moves.
 * NOTE: that is every Pokemon PokeDNA itself creates, because gen3_build_mon stamps met
 * level 0 for anything breedable — so a created mon's IVs are intentionally NOT
 * PID-correlated after a reroll, and that is legal precisely because metLevel 0 exempts it.
 * Do not "fix" this to match the create flow's Method-1 pair.
 *
 * THE ONE CASE NOTHING CAN PRESERVE is an NPC trade. Its PID and IVs are constants in the
 * trade template (pokeemerald src/trade.c:4570), not RNG output, so no seed reproduces
 * them and no reroll outcome leaves the mon being the Pokemon that trade hands out.
 * pk_pidiv_exempt_reason cannot see it — PK_PIDIV_EX_TRADE is stamped by the legality B3
 * hook (gen3_legality_hooks.c:496) from met location 0xFE, and that is the same test used
 * here. It is deliberately NOT turned into an exempt path: keeping the PID and moving only
 * the IVs would ALSO break the template, while claiming nothing changed. It takes the
 * ordinary PID-moving path — the result is at least a real Method-1 pair — and raises
 * IvRoll.chg_trade so the caller's confirm panel says the provenance is gone BEFORE
 * anything is written.
 *
 * It does not own an LCRNG walk: it drives pk_spread_roll (gen3_pidiv.c) with progressively
 * relaxed constraints, so the generator and the auditor can never drift apart.
 *
 * Pure C (no tonc, no GBA headers): tests/host_ivroll_test.c runs all of it on the PC. */

typedef struct {
  uint32_t pid;            /* the PID to use (== the old one when !pid_locked)          */
  uint32_t ivword;         /* the new Misc word: IVs + the egg bit + the right ability  */
  uint8_t  ivs[PK_NSTATS];
  bool     pid_locked;     /* false = PIDIV-exempt, so the PID was KEPT                  */
  uint8_t  exempt;
  bool     chg_nature, chg_gender, chg_ability, chg_shiny, chg_form;
  bool     chg_trade;      /* an NPC trade: the template's fixed PID cannot be kept      */
  uint8_t  old_nature, new_nature, old_gender, new_gender, old_ability, new_ability;
  bool     old_shiny,  new_shiny;
  uint8_t  old_form,   new_form;
  uint32_t rolls;          /* candidates consumed across every slice of this search      */
} IvRoll;

/* Chunked search state, so the caller can s_vsync() between slices. Zero it to start.
 * `rolls` is the running candidate count for the whole search and `rung_tries` the count
 * spent on the CURRENT rung: both belong to the STATE and not to IvRoll, because IvRoll is
 * only filled in on the last slice — and because a rung's budget is spent across many
 * slices, so comparing one slice's tries against it would leave the ladder inert (measured:
 * the shiny worst case went from a designed ~200k candidates to 17,880,120). */
typedef struct {
  uint32_t seed, rolls, rung_tries;
  uint8_t  rung, started;
} IvRollState;

/* Drive the search for at most `slice` seeds. Returns 0 = still searching (call again next
 * vblank), 1 = done (*out is valid), -1 = cannot roll (bad egg). MUST be chunked: the only
 * slow case is a SHINY, where keeping the sparkle costs ~110k-190k candidates, and the
 * search runs on the main loop — s_vsync() is the sole caller of snd_vblank(), which is the
 * sole caller of rmbl_vblank(), which is the only thing that ENDS a rumble cue. A monolithic
 * search would leave the motor energised for up to a second on Guy's Omega. */
int iv_roll_step(const EditMon* e, const PkMon* cur, IvRollState* st, uint32_t slice,
                 IvRoll* out);

/* Write a completed roll into the record. Split from the search so the caller can ASK. */
void iv_roll_apply(EditMon* e, const IvRoll* r);

/* ---- the undo/redo list ----------------------------------------------------------
 * ENTRY 0 IS PINNED to the state the record walked in with, so stepping all the way back is
 * bit-exact FOREVER — at capacity the SECOND-oldest is dropped, never the first. (Dropping
 * index 0 was measured to break it: 20 rolls then walk-back gave ivword 0x00374E6B where the
 * original was 0x00000000, while the UI still printed "Roll 1/16  the original".)
 * A new push after a step-back truncates the redo tail. 16 x 8 bytes + 4 = 132 B. */
typedef struct { uint32_t pid, ivword; } IvSnap;
typedef struct { IvSnap e[IVH_CAP]; uint16_t species; uint8_t n, cur; } IvHistory;

void ivh_reset(IvHistory* h, const EditMon* e);
/* Rebase if the record no longer matches the entry the cursor is on, or if the species
 * changed. Call once per repaint: it is why an old entry can never undo an unrelated edit. */
bool ivh_sync (IvHistory* h, const EditMon* e);
void ivh_push (IvHistory* h, const EditMon* e);
bool ivh_step (IvHistory* h, EditMon* e, int dir);

#endif /* GEN3_IVROLL_H */
